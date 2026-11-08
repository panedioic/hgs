#include "session.h"
#include "platform/ohos/presentation/cpu_surface.h"
#include "platform/ohos/process/xwayland_process.h"
#ifdef HGS_BUILD_XIM
#include "platform/ohos/process/xim_process.h"
#endif

#include <libdrm/drm_fourcc.h>
#include <hilog/log.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/render/pixman.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/xwayland/server.h>
#include <wlr/xwayland/xwayland.h>

// 来自固定的 OHOS wlroots 构建。使用已有 Wayland client 与 XWM 握手，
// 由 HGS 接管 fd、进程和端点，避免复用 wlroots 中基于环境变量的启动器。
extern struct wlr_xwayland *wlr_xwayland_create_with_server(struct wl_display *,
    struct wlr_compositor *, struct wlr_xwayland_server *);

struct hgs_x11_session {
    struct wl_display *display;
    struct wl_event_loop *loop;
    struct wlr_backend *backend;
    struct wlr_renderer *renderer;
    struct wlr_output *output;
    struct wlr_seat *seat;
    struct wlr_keyboard keyboard;
    bool keyboard_initialized, ready, child_lost;
    struct wlr_xwayland_server server;
    struct wlr_xwayland *xwayland;
    struct hgs_xwayland_process child;
    int ready_fd;
    char reply[16];
    size_t reply_size;
    struct wl_event_source *ready_source, *wake_source, *child_source;
    char diagnostic[1024];
    size_t diagnostic_size;
    struct wl_listener child_destroy, xwayland_ready, new_surface, frame;
    struct wl_list windows;
    uint64_t next_client, next_generation;
    uint32_t resource_mask;
    xcb_atom_t wm_state, wm_state_modal;
    hgs_x11_observer observer;
    enum hgs_x11_external_result (*present_external)(void *, uint32_t, uint32_t, uint64_t,
                                                     struct NativeWindow *);
    void (*close_external)(void *, uint64_t, bool);
    void *external_context;
    struct wl_event_source *external_timer;
    int external_period_ms;
    void *context;
#ifdef HGS_BUILD_XIM
    const char *ime_server_name, *authority_path;
    struct wl_event_source *xim_source;
    int xim_fd;
    uint32_t xim_xid;
    bool xim_failed;
#endif
};

struct xwindow {
    struct hgs_x11_session *session;
    struct wlr_xwayland_surface *xsurface;
    struct wl_list link;
    uint64_t client_id, generation;
    uint64_t surface_generation;
    uint64_t popup_parent_generation;
    bool mapped;
    bool text_input_active;
    struct hgs_cpu_surface output;
    unsigned present_attempts;
    bool presented;
    struct wl_listener destroy, associate, dissociate;
    struct wl_listener map, unmap, commit;
    struct wl_listener title, app_class, geometry, parent, hints, configure, close;
    struct wl_listener map_request;
    struct wl_event_source *modal_update;
};

static void release_window_input(struct xwindow *window);

static void listen_to(struct wl_signal *signal, struct wl_listener *listener,
                      wl_notify_func_t callback) {
    listener->notify = callback;
    wl_signal_add(signal, listener);
}

static void detach(struct wl_listener *listener) {
    if (!listener->link.next) return;
    wl_list_remove(&listener->link);
    listener->link.next = listener->link.prev = NULL;
}

static const struct xwindow *parent_window(const struct xwindow *window) {
    const struct wlr_xwayland_surface *parent = window->xsurface->parent;
    struct xwindow *other;
    wl_list_for_each(other, &window->session->windows, link) {
        if (other == window || !other->mapped) continue;
        if (parent ? other->xsurface == parent : other->generation == window->popup_parent_generation)
            return other;
    }
    return NULL;
}

static void publish(struct xwindow *window, enum hgs_x11_event_kind kind) {
    struct wlr_xwayland_surface *xs = window->xsurface;
    struct hgs_x11_session *session = window->session;
    const struct xwindow *parent = parent_window(window);
    struct hgs_x11_window info = {
        .xid = xs->window_id,
        .client_id = window->client_id,
        .pid = xs->pid > 0 ? (uint32_t)xs->pid : 0,
        .generation = window->generation,
        .surface_generation = window->output.id ? window->surface_generation : 0,
        .parent_xid = parent ? parent->xsurface->window_id : 0,
        .parent_client_id = parent ? parent->client_id : 0,
        .parent_generation = parent ? parent->generation : 0,
        .title = xs->title,
        .app_id = xs->class,
        .x = xs->x, .y = xs->y, .width = xs->width, .height = xs->height,
        .popup = xs->override_redirect, .modal = xs->modal,
        .minimized = xs->minimized, .maximized = xs->maximized_vert && xs->maximized_horz,
        .fullscreen = xs->fullscreen,
        .text_input_active = window->text_input_active,
        .frame_presented = window->presented,
    };
    const xcb_size_hints_t *hints = xs->size_hints;
    if (hints && (hints->flags & XCB_ICCCM_SIZE_HINT_P_MIN_SIZE)) {
        info.min_width = hints->min_width;
        info.min_height = hints->min_height;
    }
    if (hints && (hints->flags & XCB_ICCCM_SIZE_HINT_P_MAX_SIZE)) {
        info.max_width = hints->max_width;
        info.max_height = hints->max_height;
    }
    session->observer(session->context, kind, &info);
}

static void publish_children(struct xwindow *parent) {
    struct xwindow *other;
    wl_list_for_each(other, &parent->session->windows, link) {
        if (other->mapped && other != parent &&
            (other->xsurface->parent == parent->xsurface || other->popup_parent_generation == parent->generation))
            publish(other, HGS_X11_UPDATED);
    }
}

static uint64_t window_client_id(const struct xwindow *window) {
    struct hgs_x11_session *session = window->session;
    const uint32_t base = window->xsurface->window_id & ~session->resource_mask;
    struct xwindow *other;
    wl_list_for_each(other, &session->windows, link) {
        if (other->client_id && (other->xsurface->window_id & ~session->resource_mask) == base) {
            return other->client_id;
        }
    }
    return ++session->next_client;
}

static void map_window(struct wl_listener *listener, void *data) {
    (void)data;
    struct xwindow *window = wl_container_of(listener, window, map);
    if (window->mapped) return;
    window->mapped = true;
    // X server 会复用资源 ID 槽。只在该槽仍有可观察 X 窗口时复用代次。
    if (!window->client_id) window->client_id = window_client_id(window);
    window->generation = ++window->session->next_generation;
    window->popup_parent_generation = 0;
    if (window->xsurface->override_redirect && !window->xsurface->parent) {
        // Override-redirect menus often omit WM_TRANSIENT_FOR. Only adopt
        // them into a focused, mapped window on the same X connection.
        struct xwindow *other;
        wl_list_for_each(other, &window->session->windows, link) {
            if (other != window && other->mapped && other->client_id == window->client_id &&
                other->xsurface->surface == window->session->seat->keyboard_state.focused_surface) {
                window->popup_parent_generation = other->generation;
                break;
            }
        }
    }
    wlr_surface_send_enter(window->xsurface->surface, window->session->output);
    publish(window, HGS_X11_MAPPED);
    publish_children(window);
    wlr_output_schedule_frame(window->session->output);
}

static void close_window_output(struct xwindow *window, bool unmapped) {
    if (window->session->close_external)
        window->session->close_external(window->session->external_context, window->generation, unmapped);
    hgs_cpu_surface_close(&window->output);
    window->present_attempts = 0;
    window->presented = false;
}

static void withdraw_window(struct xwindow *window) {
    if (!window->mapped) return;
    release_window_input(window);
    window->text_input_active = false;
    window->mapped = false;
    close_window_output(window, true);
    publish(window, HGS_X11_UNMAPPED);
    publish_children(window);
}

static void unmap_window(struct wl_listener *listener, void *data) {
    (void)data;
    struct xwindow *window = wl_container_of(listener, window, unmap);
    wlr_surface_send_leave(window->xsurface->surface, window->session->output);
    withdraw_window(window);
}

static void commit_window(struct wl_listener *listener, void *data) {
    (void)data;
    struct xwindow *window = wl_container_of(listener, window, commit);
    if (window->mapped) {
        if (window->output.id) window->present_attempts = 8;
        wlr_output_schedule_frame(window->session->output);
    }
}

static void associate_window(struct wl_listener *listener, void *data) {
    (void)data;
    struct xwindow *window = wl_container_of(listener, window, associate);
    struct wlr_surface *surface = window->xsurface->surface;
    listen_to(&surface->events.map, &window->map, map_window);
    listen_to(&surface->events.unmap, &window->unmap, unmap_window);
    listen_to(&surface->events.commit, &window->commit, commit_window);
    if (surface->mapped) map_window(&window->map, NULL);
}

static void dissociate(struct xwindow *window) {
    detach(&window->map);
    detach(&window->unmap);
    detach(&window->commit);
    withdraw_window(window);
}

static void dissociate_window(struct wl_listener *listener, void *data) {
    (void)data;
    struct xwindow *window = wl_container_of(listener, window, dissociate);
    dissociate(window);
}

static void free_window(struct xwindow *window) {
    dissociate(window);
    detach(&window->associate);
    detach(&window->dissociate);
    detach(&window->destroy);
    detach(&window->title);
    detach(&window->app_class);
    detach(&window->geometry);
    detach(&window->parent);
    detach(&window->hints);
    detach(&window->configure);
    detach(&window->close);
    detach(&window->map_request);
    if (window->modal_update) wl_event_source_remove(window->modal_update);
    wl_list_remove(&window->link);
    free(window);
}

static void destroy_window(struct wl_listener *listener, void *data) {
    (void)data;
    struct xwindow *window = wl_container_of(listener, window, destroy);
    free_window(window);
}

static void title_changed(struct wl_listener *listener, void *data) {
    (void)data;
    struct xwindow *window = wl_container_of(listener, window, title);
    if (window->mapped) publish(window, HGS_X11_UPDATED);
}

static void class_changed(struct wl_listener *listener, void *data) {
    (void)data;
    struct xwindow *window = wl_container_of(listener, window, app_class);
    if (window->mapped) publish(window, HGS_X11_UPDATED);
}

static void geometry_changed(struct wl_listener *listener, void *data) {
    (void)data;
    struct xwindow *window = wl_container_of(listener, window, geometry);
    if (window->mapped) publish(window, HGS_X11_UPDATED);
}

static void configure_window(struct wl_listener *listener, void *data) {
    struct xwindow *window = wl_container_of(listener, window, configure);
    const struct wlr_xwayland_surface_configure_event *request = data;
    wlr_xwayland_surface_configure(window->xsurface, request->x, request->y,
                                  request->width, request->height);
}

static void parent_changed(struct wl_listener *listener, void *data) {
    (void)data;
    struct xwindow *window = wl_container_of(listener, window, parent);
    if (window->mapped) publish(window, HGS_X11_UPDATED);
}

static void hints_changed(struct wl_listener *listener, void *data) {
    (void)data;
    struct xwindow *window = wl_container_of(listener, window, hints);
    if (window->mapped) publish(window, HGS_X11_UPDATED);
}

static void request_close(struct wl_listener *listener, void *data) {
    (void)data;
    struct xwindow *window = wl_container_of(listener, window, close);
    if (window->mapped) publish(window, HGS_X11_CLOSE_REQUESTED);
}

static void request_map(struct wl_listener *listener, void *data) {
    (void)data;
    struct xwindow *window = wl_container_of(listener, window, map_request);
    struct hgs_x11_session *session = window->session;
    if (!session->wm_state || !session->wm_state_modal) return;
    xcb_connection_t *connection = wlr_xwayland_get_xwm_connection(session->xwayland);
    // XWM writes _NET_WM_STATE at MapNotify, which can precede its initial
    // property read during Wayland surface association. Preserve client modality first.
    const xcb_get_property_cookie_t cookie = xcb_get_property(connection, false,
        window->xsurface->window_id, session->wm_state, XCB_ATOM_ATOM, 0, 64);
    xcb_get_property_reply_t *reply = xcb_get_property_reply(connection, cookie, NULL);
    if (!reply) return;
    window->xsurface->modal = false;
    if (reply->type == XCB_ATOM_ATOM && reply->format == 32) {
        const xcb_atom_t *states = xcb_get_property_value(reply);
        for (uint32_t i = 0; i < reply->value_len; ++i) {
            if (states[i] == session->wm_state_modal) {
                window->xsurface->modal = true;
                break;
            }
        }
    }
    free(reply);
}

static void publish_modal_update(void *data) {
    struct xwindow *window = data;
    window->modal_update = NULL;
    if (window->mapped) publish(window, HGS_X11_UPDATED);
}

static bool observe_x11_event(struct wlr_xwayland *xwayland, xcb_generic_event_t *event) {
    struct hgs_x11_session *session = xwayland->data;
    if ((event->response_type & 0x7f) != XCB_CLIENT_MESSAGE || !session->wm_state_modal) return false;
    const xcb_client_message_event_t *message = (const xcb_client_message_event_t *)event;
    if (message->type != session->wm_state || message->format != 32 ||
        (message->data.data32[1] != session->wm_state_modal && message->data.data32[2] != session->wm_state_modal))
        return false;
    struct xwindow *window;
    wl_list_for_each(window, &session->windows, link) {
        if (window->mapped && window->xsurface->window_id == message->window && !window->modal_update) {
            // Read the result after wlroots handles the request.
            window->modal_update = wl_event_loop_add_idle(session->loop, publish_modal_update, window);
            break;
        }
    }
    return false;
}

static void new_window(struct wl_listener *listener, void *data) {
    struct hgs_x11_session *session = wl_container_of(listener, session, new_surface);
    struct wlr_xwayland_surface *surface = data;
    struct xwindow *window = calloc(1, sizeof(*window));
    if (!window) return;
    window->session = session;
    window->xsurface = surface;
    wl_list_insert(&session->windows, &window->link);
    listen_to(&surface->events.destroy, &window->destroy, destroy_window);
    listen_to(&surface->events.associate, &window->associate, associate_window);
    listen_to(&surface->events.dissociate, &window->dissociate, dissociate_window);
    listen_to(&surface->events.set_title, &window->title, title_changed);
    listen_to(&surface->events.set_class, &window->app_class, class_changed);
    listen_to(&surface->events.set_geometry, &window->geometry, geometry_changed);
    listen_to(&surface->events.set_parent, &window->parent, parent_changed);
    listen_to(&surface->events.set_size_hints, &window->hints, hints_changed);
    listen_to(&surface->events.request_configure, &window->configure, configure_window);
    listen_to(&surface->events.request_close, &window->close, request_close);
    listen_to(&surface->events.map_request, &window->map_request, request_map);
}

static pixman_format_code_t pixman_format(uint32_t fourcc) {
    switch (fourcc) {
        case DRM_FORMAT_XRGB8888: return PIXMAN_x8r8g8b8;
        case DRM_FORMAT_ARGB8888: return PIXMAN_a8r8g8b8;
        case DRM_FORMAT_XBGR8888: return PIXMAN_x8b8g8r8;
        case DRM_FORMAT_ABGR8888: return PIXMAN_a8b8g8r8;
        case DRM_FORMAT_RGB565: return PIXMAN_r5g6b5;
        default: return 0;
    }
}

static bool present_window(struct xwindow *window) {
    struct wlr_client_buffer *client = window->xsurface->surface->buffer;
    struct wlr_buffer *buffer = client ? client->source : NULL;
    if (!buffer) return false;
    void *pixels = NULL;
    uint32_t fourcc;
    size_t stride;
    if (!wlr_buffer_begin_data_ptr_access(buffer, WLR_BUFFER_DATA_PTR_ACCESS_READ,
                                          &pixels, &fourcc, &stride)) return false;
    const pixman_format_code_t format = pixman_format(fourcc);
    pixman_image_t *image = pixels && format && stride <= INT_MAX && stride % 4 == 0 &&
        stride >= (size_t)buffer->width * PIXMAN_FORMAT_BPP(format) / 8
        ? pixman_image_create_bits_no_clear(format, buffer->width, buffer->height, pixels, (int)stride)
        : NULL;
    const bool shown = image && hgs_cpu_surface_present(&window->output, image);
    if (image) pixman_image_unref(image);
    wlr_buffer_end_data_ptr_access(buffer);
    return shown;
}

static void mark_presented(struct xwindow *window) {
    if (window->presented) return;
    window->presented = true;
    publish(window, HGS_X11_UPDATED);
}

static void output_frame(struct wl_listener *listener, void *data) {
    (void)data;
    struct hgs_x11_session *session = wl_container_of(listener, session, frame);
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    struct xwindow *window;
    wl_list_for_each(window, &session->windows, link) {
        if (!window->mapped) continue;
        wlr_surface_send_frame_done(window->xsurface->surface, &now);
        if (session->present_external && window->output.window) {
            const pid_t pid = window->xsurface->pid;
            const enum hgs_x11_external_result result =
                session->present_external(session->external_context, window->xsurface->window_id,
                                          pid > 0 ? (uint32_t)pid : 0, window->generation,
                                          window->output.window);
            if (result == HGS_X11_EXTERNAL_OWNED) continue;
            if (result == HGS_X11_EXTERNAL_PRESENTED) {
                mark_presented(window);
                continue;
            }
            if (result == HGS_X11_EXTERNAL_RELEASED) {
                hgs_cpu_surface_reset(&window->output);
                window->present_attempts = 8;
            }
        }
        if (!window->present_attempts) continue;
        if (present_window(window)) {
            window->present_attempts = 0;
            if (!window->presented) {
                OH_LOG_Print(LOG_APP, LOG_INFO, 0, "HGS_X11", "Presented first frame xid=%{public}u surface=%{public}llu",
                             window->xsurface->window_id, (unsigned long long)window->output.id);
                mark_presented(window);
            }
        } else if (--window->present_attempts) {
            wlr_output_schedule_frame(session->output);
        } else if (!window->presented) {
            OH_LOG_Print(LOG_APP, LOG_WARN, 0, "HGS_X11", "No frame available for xid=%{public}u surface=%{public}llu",
                         window->xsurface->window_id, (unsigned long long)window->output.id);
        }
    }
}

static int external_tick(void *context) {
    struct hgs_x11_session *session = context;
    struct xwindow *window;
    wl_list_for_each(window, &session->windows, link) {
        if (!window->mapped || !window->output.window) continue;
        wlr_output_schedule_frame(session->output);
        wl_event_source_timer_update(session->external_timer, session->external_period_ms);
        break;
    }
    return 0;
}

#ifdef HGS_BUILD_XIM
static void sync_xim_focus(struct hgs_x11_session *session) {
    struct xwindow *window;
    wl_list_for_each(window, &session->windows, link) {
        if (!window->mapped) continue;
        const bool active = window->xsurface->window_id == session->xim_xid &&
            session->seat->keyboard_state.focused_surface == window->xsurface->surface;
        if (window->text_input_active != active) {
            window->text_input_active = active;
            publish(window, HGS_X11_UPDATED);
        }
    }
}

static void close_xim(struct hgs_x11_session *session) {
    if (session->xim_source) wl_event_source_remove(session->xim_source);
    session->xim_source = NULL;
    if (session->xim_fd >= 0) close(session->xim_fd);
    session->xim_fd = -1;
}

static int read_xim(int fd, uint32_t mask, void *data) {
    (void)mask;
    struct hgs_x11_session *session = data;
    struct hgs_xim_notice notice;
    ssize_t count;
    do { count = recv(fd, &notice, sizeof(notice), MSG_DONTWAIT | MSG_TRUNC); } while (count < 0 && errno == EINTR);
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    if (count == sizeof(notice)) {
        if (notice.type == HGS_XIM_READY && !session->ready) {
            session->ready = true;
            return 0;
        }
        if (notice.type == HGS_XIM_FOCUS && session->ready) {
            session->xim_xid = notice.xid;
            sync_xim_focus(session);
            return 0;
        }
    }
    close_xim(session);
    if (!session->ready) session->xim_failed = true;
    session->xim_xid = 0;
    sync_xim_focus(session);
    return 0;
}
#endif

static xcb_atom_t lookup_atom(xcb_connection_t *connection, const char *name) {
    xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(connection,
        xcb_intern_atom(connection, true, strlen(name), name), NULL);
    const xcb_atom_t atom = reply ? reply->atom : XCB_ATOM_NONE;
    free(reply);
    return atom;
}

static void xwayland_ready(struct wl_listener *listener, void *data) {
    (void)data;
    struct hgs_x11_session *session = wl_container_of(listener, session, xwayland_ready);
    xcb_connection_t *connection = wlr_xwayland_get_xwm_connection(session->xwayland);
    if (!connection || xcb_connection_has_error(connection)) return;
    session->resource_mask = xcb_get_setup(connection)->resource_id_mask;
    session->wm_state = lookup_atom(connection, "_NET_WM_STATE");
    session->wm_state_modal = lookup_atom(connection, "_NET_WM_STATE_MODAL");
#ifdef HGS_BUILD_XIM
    if (session->ime_server_name && session->ime_server_name[0]) {
        session->xim_fd = hgs_spawn_xim(session->server.display_name, session->authority_path,
                                       session->ime_server_name);
        if (session->xim_fd >= 0) {
            session->xim_source = wl_event_loop_add_fd(session->loop, session->xim_fd,
                                                       WL_EVENT_READABLE, read_xim, session);
            if (session->xim_source) return;
        }
        close_xim(session);
        session->xim_failed = true;
        return;
    }
#endif
    session->ready = true;
}

static void child_disconnected(struct wl_listener *listener, void *data) {
    (void)data;
    struct hgs_x11_session *session = wl_container_of(listener, session, child_destroy);
    detach(listener);
    session->server.client = NULL;
    session->child_lost = true;
}

static void close_ready_channel(struct hgs_x11_session *session) {
    if (session->ready_source) wl_event_source_remove(session->ready_source);
    if (session->ready_fd >= 0) close(session->ready_fd);
    session->ready_source = NULL;
    session->ready_fd = -1;
}

static int read_ready(int fd, uint32_t mask, void *data) {
    struct hgs_x11_session *session = data;
    ssize_t count = 0;
    if (mask & WL_EVENT_READABLE) {
        count = read(fd, session->reply + session->reply_size,
                     sizeof(session->reply) - session->reply_size);
        if (count < 0 && (errno == EAGAIN || errno == EINTR)) return 0;
    }
    if (count > 0) {
        session->reply_size += (size_t)count;
        if (session->reply[session->reply_size - 1] == '\n') {
            char expected[16];
            int size = snprintf(expected, sizeof(expected), "%d\n", session->server.display);
            close_ready_channel(session);
            if (size > 0 && session->reply_size == (size_t)size &&
                memcmp(session->reply, expected, (size_t)size) == 0) {
                session->server.ready = true;
                struct wlr_xwayland_server_ready_event event = {
                    .server = &session->server, .wm_fd = session->server.wm_fd[0],
                };
                wl_signal_emit_mutable(&session->server.events.ready, &event);
                return 0;
            }
        } else if (session->reply_size < sizeof(session->reply) &&
                   !(mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR))) {
            return 0;
        }
    }
    close_ready_channel(session);
    session->child_lost = true;
    return 0;
}

static int read_wake(int fd, uint32_t mask, void *data) {
    (void)mask;
    (void)data;
    uint64_t value;
    while (read(fd, &value, sizeof(value)) < 0 && errno == EINTR) {}
    return 0;
}

static bool setup_keyboard(struct hgs_x11_session *session, const char *xkb_root) {
    struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_DEFAULT_INCLUDES);
    if (!context) return false;
    struct xkb_rule_names rules = {.rules = "evdev", .model = "pc105", .layout = "us"};
    struct xkb_keymap *keymap = NULL;
    if (xkb_context_include_path_append(context, xkb_root)) {
        keymap = xkb_keymap_new_from_names(context, &rules, XKB_KEYMAP_COMPILE_NO_FLAGS);
    }
    xkb_context_unref(context);
    if (!keymap) return false;
    static const struct wlr_keyboard_impl impl = {.name = "hgs-x11-keyboard"};
    wlr_keyboard_init(&session->keyboard, &impl, impl.name);
    session->keyboard_initialized = true;
    bool ok = wlr_keyboard_set_keymap(&session->keyboard, keymap);
    xkb_keymap_unref(keymap);
    if (ok) wlr_seat_set_keyboard(session->seat, &session->keyboard);
    return ok;
}

static int read_child(int fd, uint32_t mask, void *data) {
    (void)mask;
    struct hgs_x11_session *session = data;
    char buffer[256];
    for (;;) {
        ssize_t count = recv(fd, buffer, sizeof(buffer), MSG_DONTWAIT);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (count <= 0) {
            session->child_lost = true;
            break;
        }
        size_t capacity = sizeof(session->diagnostic) - 1;
        size_t retain = capacity - (size_t)count;
        if (session->diagnostic_size > retain) {
            memmove(session->diagnostic, session->diagnostic + session->diagnostic_size - retain, retain);
            session->diagnostic_size = retain;
        }
        memcpy(session->diagnostic + session->diagnostic_size, buffer, count);
        session->diagnostic_size += count;
        session->diagnostic[session->diagnostic_size] = '\0';
    }
    return 0;
}

static bool start_child(struct hgs_x11_session *session, const struct hgs_x11_settings *settings) {
    int wl[2] = {-1, -1}, wm[2] = {-1, -1}, ready[2] = {-1, -1};
    bool ok = false;
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, wl) != 0 ||
        socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, wm) != 0 ||
        pipe2(ready, O_CLOEXEC | O_NONBLOCK) != 0) goto out;
    if (!hgs_spawn_xwayland(session->server.display_name, settings->session_dir,
                            settings->xkb_root, settings->authority_path, settings->x_socket,
                            wl[1], wm[1], ready[1], &session->child)) goto out;
    session->child_source = wl_event_loop_add_fd(session->loop, session->child.control_fd,
                                                 WL_EVENT_READABLE, read_child, session);
    if (!session->child_source) goto out;
    // 在 NCP 启动 IPC 完成后注册 wl_client，避免设备上已注册的 epoll 事件丢失。
    session->server.client = wl_client_create(session->display, wl[0]);
    if (!session->server.client) goto out;
    wl[0] = -1;
    session->child_destroy.notify = child_disconnected;
    wl_client_add_destroy_listener(session->server.client, &session->child_destroy);
    session->server.wm_fd[0] = wm[0];
    wm[0] = -1;
    session->ready_source = wl_event_loop_add_fd(session->loop, ready[0], WL_EVENT_READABLE,
                                                read_ready, session);
    if (!session->ready_source) goto out;
    session->ready_fd = ready[0];
    ready[0] = -1;
    wl_signal_emit_mutable(&session->server.events.start, NULL);
    ok = true;
out:
    for (int i = 0; i < 2; ++i) {
        if (wl[i] >= 0) close(wl[i]);
        if (wm[i] >= 0) close(wm[i]);
        if (ready[i] >= 0) close(ready[i]);
    }
    return ok;
}

struct hgs_x11_session *hgs_x11_create(const struct hgs_x11_settings *settings, const char **stage) {
    *stage = "x11-display";
    struct hgs_x11_session *session = calloc(1, sizeof(*session));
    if (!session) return NULL;
    session->ready_fd = session->child.control_fd = session->server.wm_fd[0] = -1;
#ifdef HGS_BUILD_XIM
    session->xim_fd = -1;
    session->ime_server_name = settings->ime_server_name;
    session->authority_path = settings->authority_path;
#endif
    session->observer = settings->observer;
    session->present_external = settings->present_external;
    session->close_external = settings->close_external;
    session->external_context = settings->external_context;
    session->context = settings->context;
    wl_list_init(&session->windows);
    session->display = wl_display_create();
    if (!session->display) goto failed;
    session->loop = wl_display_get_event_loop(session->display);
    session->wake_source = wl_event_loop_add_fd(session->loop, settings->wake_fd,
                                                WL_EVENT_READABLE, read_wake, session);
    if (!session->wake_source) goto failed;
    if (session->present_external) {
        session->external_period_ms = (1000000 + settings->refresh_millihz / 2) / settings->refresh_millihz;
        session->external_timer = wl_event_loop_add_timer(session->loop, external_tick, session);
        if (!session->external_timer) goto failed;
    }
    session->backend = wlr_headless_backend_create(session->loop);
    session->renderer = wlr_pixman_renderer_create();
    if (!session->backend || !session->renderer ||
        !wlr_renderer_init_wl_display(session->renderer, session->display)) goto failed;
    struct wlr_compositor *compositor = wlr_compositor_create(session->display, 5, session->renderer);
    if (!compositor || !wlr_backend_start(session->backend)) goto failed;

    *stage = "x11-output";
    session->output = wlr_headless_add_output(session->backend, settings->width, settings->height);
    if (!session->output) goto failed;
    struct wlr_output_state output_state;
    wlr_output_state_init(&output_state);
    wlr_output_state_set_enabled(&output_state, true);
    wlr_output_state_set_custom_mode(&output_state, settings->width, settings->height,
                                     settings->refresh_millihz);
    bool committed = wlr_output_commit_state(session->output, &output_state);
    wlr_output_state_finish(&output_state);
    if (!committed) goto failed;
    wlr_output_create_global(session->output, session->display);
    listen_to(&session->output->events.frame, &session->frame, output_frame);

    *stage = "x11-keymap";
    session->seat = wlr_seat_create(session->display, "hgs-x11");
    if (!session->seat || !setup_keyboard(session, settings->xkb_root)) goto failed;
    wlr_seat_set_capabilities(session->seat, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);

    *stage = "x11-xwm";
    session->server.wl_display = session->display;
    session->server.options.enable_wm = true;
    session->server.display = settings->display_number;
    snprintf(session->server.display_name, sizeof(session->server.display_name),
             ":%d", settings->display_number);
    wl_signal_init(&session->server.events.start);
    wl_signal_init(&session->server.events.ready);
    wl_signal_init(&session->server.events.destroy);
    session->xwayland = wlr_xwayland_create_with_server(session->display, compositor, &session->server);
    if (!session->xwayland) goto failed;
    session->xwayland->data = session;
    session->xwayland->user_event_handler = observe_x11_event;
    wlr_xwayland_set_seat(session->xwayland, session->seat);
    listen_to(&session->xwayland->events.ready, &session->xwayland_ready, xwayland_ready);
    listen_to(&session->xwayland->events.new_surface, &session->new_surface, new_window);

    *stage = "x11-child";
    if (!start_child(session, settings)) goto failed;
    return session;
failed:
    hgs_x11_destroy(session);
    return NULL;
}

int hgs_x11_dispatch(struct hgs_x11_session *session, int timeout_ms) {
    wl_display_flush_clients(session->display);
    int result = wl_event_loop_dispatch(session->loop, timeout_ms);
    return result < 0 && errno == EINTR ? 0 : result;
}

bool hgs_x11_ready(const struct hgs_x11_session *session) { return session->ready; }
const char *hgs_x11_diagnostic(const struct hgs_x11_session *session) { return session->diagnostic; }
bool hgs_x11_child_lost(const struct hgs_x11_session *session) { return session->child_lost; }
bool hgs_x11_xim_failed(const struct hgs_x11_session *session) {
#ifdef HGS_BUILD_XIM
    return session->xim_failed;
#else
    (void)session;
    return false;
#endif
}

void hgs_x11_destroy(struct hgs_x11_session *session) {
    if (!session) return;
#ifdef HGS_BUILD_XIM
    close_xim(session);
#endif
    close_ready_channel(session);
    if (session->wake_source) wl_event_source_remove(session->wake_source);
    if (session->external_timer) wl_event_source_remove(session->external_timer);
    if (session->child_source) wl_event_source_remove(session->child_source);
    detach(&session->xwayland_ready);
    detach(&session->new_surface);
    detach(&session->frame);
    struct xwindow *window, *next;
    wl_list_for_each_safe(window, next, &session->windows, link) free_window(window);
    if (session->xwayland) wlr_xwayland_destroy(session->xwayland);
    if (session->server.client) wl_client_destroy(session->server.client);
    if (session->server.wm_fd[0] >= 0) close(session->server.wm_fd[0]);
    hgs_stop_xwayland(&session->child);
    if (session->display) wl_display_destroy_clients(session->display);
    if (session->seat) wlr_seat_destroy(session->seat);
    if (session->keyboard_initialized) wlr_keyboard_finish(&session->keyboard);
    if (session->backend) wlr_backend_destroy(session->backend);
    if (session->renderer) wlr_renderer_destroy(session->renderer);
    if (session->display) wl_display_destroy(session->display);
    free(session);
}

static struct xwindow *find_window(struct hgs_x11_session *session, uint32_t xid, uint64_t generation) {
    struct xwindow *window;
    wl_list_for_each(window, &session->windows, link) {
        if (window->mapped && window->xsurface->window_id == xid &&
            window->generation == generation) return window;
    }
    return NULL;
}

void hgs_x11_commit_text(struct hgs_x11_session *session, uint32_t xid, uint64_t generation,
                         const char *text, size_t length) {
#ifdef HGS_BUILD_XIM
    const struct xwindow *window = find_window(session, xid, generation);
    if (session->xim_fd < 0 || !window || !window->text_input_active || !length ||
        length > HGS_XIM_TEXT_LIMIT) return;
    struct hgs_xim_commit message = {.xid = xid};
    memcpy(message.text, text, length);
    ssize_t count;
    do {
        count = send(session->xim_fd, &message, offsetof(struct hgs_xim_commit, text) + length,
                     MSG_DONTWAIT | MSG_NOSIGNAL);
    } while (count < 0 && errno == EINTR);
#else
    (void)session; (void)xid; (void)generation; (void)text; (void)length;
#endif
}

void hgs_x11_bind_surface(struct hgs_x11_session *session, uint32_t xid, uint64_t generation,
                          uint64_t surface_id, bool attach) {
    struct xwindow *window = find_window(session, xid, generation);
    if (!window) return;
    if (!attach) {
        if (window->output.id != surface_id) return;
        close_window_output(window, false);
        publish(window, HGS_X11_UPDATED);
        return;
    }
    if (window->output.id != surface_id) {
        struct hgs_cpu_surface replacement = {0};
        if (!hgs_cpu_surface_open(&replacement, surface_id)) {
            OH_LOG_Print(LOG_APP, LOG_WARN, 0, "HGS_X11", "Could not bind surface=%{public}llu",
                         (unsigned long long)surface_id);
            return;
        }
        close_window_output(window, false);
        window->output = replacement;
        ++window->surface_generation;
        publish(window, HGS_X11_UPDATED);
    }
    window->present_attempts = 8;
    if (session->external_timer)
        wl_event_source_timer_update(session->external_timer, session->external_period_ms);
    wlr_output_schedule_frame(session->output);
}

static struct wlr_xwayland_surface *target(struct hgs_x11_session *session, uint32_t xid, uint64_t generation) {
    struct xwindow *window = find_window(session, xid, generation);
    return window ? window->xsurface : NULL;
}

static uint32_t now_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint32_t)((uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000);
}

static void focus_keyboard(struct hgs_x11_session *session, struct wlr_xwayland_surface *window) {
    if (session->seat->keyboard_state.focused_surface == window->surface) return;
    wlr_xwayland_surface_activate(window, true);
    wlr_seat_keyboard_notify_enter(session->seat, window->surface, session->keyboard.keycodes,
                                    session->keyboard.num_keycodes, &session->keyboard.modifiers);
#ifdef HGS_BUILD_XIM
    sync_xim_focus(session);
#endif
}

static void notify_key(struct hgs_x11_session *session, uint32_t code, bool pressed) {
    struct wlr_keyboard_key_event event = {
        .time_msec = now_ms(), .keycode = code, .update_state = true,
        .state = pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED,
    };
    wlr_keyboard_notify_key(&session->keyboard, &event);
    wlr_seat_keyboard_notify_key(session->seat, event.time_msec, code, event.state);
    wlr_seat_keyboard_notify_modifiers(session->seat, &session->keyboard.modifiers);
}

static void release_window_input(struct xwindow *window) {
    struct hgs_x11_session *session = window->session;
    struct wlr_surface *surface = window->xsurface->surface;
    if (!surface) return;
    if (session->seat->keyboard_state.focused_surface == surface) {
        uint32_t keys[WLR_KEYBOARD_KEYS_CAP];
        const size_t count = session->keyboard.num_keycodes;
        memcpy(keys, session->keyboard.keycodes, count * sizeof(keys[0]));
        for (size_t i = 0; i < count; ++i) notify_key(session, keys[i], false);
        wlr_seat_keyboard_notify_clear_focus(session->seat);
    }
    struct wlr_seat_pointer_state *pointer = &session->seat->pointer_state;
    if (pointer->focused_surface == surface) {
        const uint32_t now = now_ms();
        while (pointer->button_count > 0) {
            wlr_seat_pointer_notify_button(session->seat, now, pointer->buttons[0].button,
                                           WL_POINTER_BUTTON_STATE_RELEASED);
        }
        wlr_seat_pointer_notify_frame(session->seat);
        wlr_seat_pointer_notify_clear_focus(session->seat);
    }
}

void hgs_x11_key(struct hgs_x11_session *session, uint32_t xid, uint64_t generation,
                 uint32_t code, bool pressed) {
    struct wlr_xwayland_surface *window = target(session, xid, generation);
    if (!window) return;
    focus_keyboard(session, window);
    notify_key(session, code, pressed);
}

void hgs_x11_control_window(struct hgs_x11_session *session, uint32_t xid, uint64_t generation,
                            enum hgs_x11_control action, int32_t width, int32_t height) {
    struct xwindow *window = find_window(session, xid, generation);
    if (!window) return;
    struct wlr_xwayland_surface *surface = window->xsurface;
    switch (action) {
        case HGS_X11_FOCUS:
            focus_keyboard(session, surface);
            break;
        case HGS_X11_BLUR:
            if (session->seat->keyboard_state.focused_surface == surface->surface)
                wlr_xwayland_surface_activate(surface, false);
            release_window_input(window);
#ifdef HGS_BUILD_XIM
            sync_xim_focus(session);
#endif
            break;
        case HGS_X11_RESIZE:
            wlr_xwayland_surface_configure(surface, surface->x, surface->y, width, height);
            break;
        case HGS_X11_CLOSE:
            wlr_xwayland_surface_close(surface);
            break;
    }
}

static bool map_position(struct wlr_surface *surface, double *x, double *y) {
    const int width = surface->current.width, height = surface->current.height;
    if (width <= 0 || height <= 0) return false;
    *x = fmin(*x * width, width - 1);
    *y = fmin(*y * height, height - 1);
    return true;
}

void hgs_x11_pointer(struct hgs_x11_session *session, uint32_t xid, uint64_t generation,
                     double x, double y, uint32_t button, int action, bool normalized) {
    struct wlr_xwayland_surface *window = target(session, xid, generation);
    if (!window) return;
    if (normalized && !map_position(window->surface, &x, &y)) return;
    uint32_t now = now_ms();
    wlr_seat_pointer_notify_enter(session->seat, window->surface, x, y);
    wlr_seat_pointer_notify_motion(session->seat, now, x, y);
    if (action == 1 && !window->override_redirect) focus_keyboard(session, window);
    if (action != 3 && button) {
        wlr_seat_pointer_notify_button(session->seat, now, button,
            action == 1 ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
    }
    wlr_seat_pointer_notify_frame(session->seat);
}

void hgs_x11_scroll(struct hgs_x11_session *session, uint32_t xid, uint64_t generation,
                    double x, double y, int axis, double value, bool normalized) {
    struct wlr_xwayland_surface *window = target(session, xid, generation);
    if (!window || value == 0) return;
    if (normalized && !map_position(window->surface, &x, &y)) return;
    wlr_seat_pointer_notify_enter(session->seat, window->surface, x, y);
    wlr_seat_pointer_notify_axis(session->seat, now_ms(),
        axis == 0 ? WL_POINTER_AXIS_VERTICAL_SCROLL : WL_POINTER_AXIS_HORIZONTAL_SCROLL,
        value, value > 0 ? 120 : -120, WL_POINTER_AXIS_SOURCE_WHEEL,
        WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
    wlr_seat_pointer_notify_frame(session->seat);
}

// 固定的 wlroots 静态库同时包含旧启动入口。若误走该入口，明确失败，
// 防止它绕过 HGS 管理的认证、端点和子进程通道。
__attribute__((visibility("hidden")))
bool wlr_ohos_spawn_xwayland(struct wlr_xwayland_server *server, int notify_fd) {
    (void)server;
    (void)notify_fd;
    return false;
}
