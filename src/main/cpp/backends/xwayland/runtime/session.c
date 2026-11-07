#include "session.h"
#include "platform/ohos/presentation/cpu_surface.h"
#include "platform/ohos/process/xwayland_process.h"

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
    hgs_x11_observer observer;
    void *context;
};

struct xwindow {
    struct hgs_x11_session *session;
    struct wlr_xwayland_surface *xsurface;
    struct wl_list link;
    uint64_t client_id, generation;
    bool mapped;
    struct hgs_cpu_surface output;
    unsigned present_attempts;
    bool presented;
    struct wl_listener destroy, associate, dissociate;
    struct wl_listener map, unmap, commit;
    struct wl_listener title, app_class, geometry, configure, close;
};

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

static void publish(struct xwindow *window, enum hgs_x11_event_kind kind) {
    struct wlr_xwayland_surface *xs = window->xsurface;
    struct hgs_x11_session *session = window->session;
    struct hgs_x11_window info = {
        .xid = xs->window_id,
        .client_id = window->client_id,
        .pid = xs->pid > 0 ? (uint32_t)xs->pid : 0,
        .generation = window->generation,
        .title = xs->title,
        .app_id = xs->class,
        .x = xs->x, .y = xs->y, .width = xs->width, .height = xs->height,
        .popup = xs->override_redirect, .modal = xs->modal,
        .minimized = xs->minimized, .maximized = xs->maximized_vert && xs->maximized_horz,
        .fullscreen = xs->fullscreen,
    };
    session->observer(session->context, kind, &info);
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
    wlr_surface_send_enter(window->xsurface->surface, window->session->output);
    publish(window, HGS_X11_MAPPED);
    wlr_output_schedule_frame(window->session->output);
}

static void withdraw_window(struct xwindow *window) {
    if (!window->mapped) return;
    window->mapped = false;
    hgs_cpu_surface_close(&window->output);
    window->present_attempts = 0;
    publish(window, HGS_X11_UNMAPPED);
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
    detach(&window->configure);
    detach(&window->close);
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

static void request_close(struct wl_listener *listener, void *data) {
    (void)data;
    struct xwindow *window = wl_container_of(listener, window, close);
    if (window->mapped) publish(window, HGS_X11_CLOSE_REQUESTED);
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
    listen_to(&surface->events.request_configure, &window->configure, configure_window);
    listen_to(&surface->events.request_close, &window->close, request_close);
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

static void output_frame(struct wl_listener *listener, void *data) {
    (void)data;
    struct hgs_x11_session *session = wl_container_of(listener, session, frame);
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    struct xwindow *window;
    wl_list_for_each(window, &session->windows, link) {
        if (!window->mapped) continue;
        wlr_surface_send_frame_done(window->xsurface->surface, &now);
        if (!window->present_attempts) continue;
        if (present_window(window)) {
            window->present_attempts = 0;
            if (!window->presented) {
                OH_LOG_Print(LOG_APP, LOG_INFO, 0, "HGS_X11", "Presented first frame xid=%{public}u surface=%{public}llu",
                             window->xsurface->window_id, (unsigned long long)window->output.id);
                window->presented = true;
            }
        } else if (--window->present_attempts) {
            wlr_output_schedule_frame(session->output);
        } else if (!window->presented) {
            OH_LOG_Print(LOG_APP, LOG_WARN, 0, "HGS_X11", "No frame available for xid=%{public}u surface=%{public}llu",
                         window->xsurface->window_id, (unsigned long long)window->output.id);
        }
    }
}

static void xwayland_ready(struct wl_listener *listener, void *data) {
    (void)data;
    struct hgs_x11_session *session = wl_container_of(listener, session, xwayland_ready);
    xcb_connection_t *connection = wlr_xwayland_get_xwm_connection(session->xwayland);
    if (!connection || xcb_connection_has_error(connection)) return;
    session->resource_mask = xcb_get_setup(connection)->resource_id_mask;
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
    session->observer = settings->observer;
    session->context = settings->context;
    wl_list_init(&session->windows);
    session->display = wl_display_create();
    if (!session->display) goto failed;
    session->loop = wl_display_get_event_loop(session->display);
    session->wake_source = wl_event_loop_add_fd(session->loop, settings->wake_fd,
                                                WL_EVENT_READABLE, read_wake, session);
    if (!session->wake_source) goto failed;
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

void hgs_x11_destroy(struct hgs_x11_session *session) {
    if (!session) return;
    close_ready_channel(session);
    if (session->wake_source) wl_event_source_remove(session->wake_source);
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

void hgs_x11_bind_surface(struct hgs_x11_session *session, uint32_t xid, uint64_t generation,
                          uint64_t surface_id, bool attach) {
    struct xwindow *window = find_window(session, xid, generation);
    if (!window) return;
    if (!attach) {
        if (window->output.id != surface_id) return;
        hgs_cpu_surface_close(&window->output);
        window->present_attempts = 0;
        return;
    }
    if (window->output.id != surface_id) {
        if (!hgs_cpu_surface_open(&window->output, surface_id)) {
            OH_LOG_Print(LOG_APP, LOG_WARN, 0, "HGS_X11", "Could not bind surface=%{public}llu",
                         (unsigned long long)surface_id);
            return;
        }
        window->presented = false;
    }
    window->present_attempts = 8;
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
}

void hgs_x11_key(struct hgs_x11_session *session, uint32_t xid, uint64_t generation,
                 uint32_t code, bool pressed) {
    struct wlr_xwayland_surface *window = target(session, xid, generation);
    if (!window) return;
    focus_keyboard(session, window);
    struct wlr_keyboard_key_event event = {
        .time_msec = now_ms(), .keycode = code, .update_state = true,
        .state = pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED,
    };
    wlr_keyboard_notify_key(&session->keyboard, &event);
    wlr_seat_keyboard_notify_key(session->seat, event.time_msec, code, event.state);
    wlr_seat_keyboard_notify_modifiers(session->seat, &session->keyboard.modifiers);
}

void hgs_x11_pointer(struct hgs_x11_session *session, uint32_t xid, uint64_t generation,
                     double x, double y, uint32_t button, int action) {
    struct wlr_xwayland_surface *window = target(session, xid, generation);
    if (!window) return;
    uint32_t now = now_ms();
    wlr_seat_pointer_notify_enter(session->seat, window->surface, x, y);
    wlr_seat_pointer_notify_motion(session->seat, now, x, y);
    if (action == 1) focus_keyboard(session, window);
    if (action != 3 && button) {
        wlr_seat_pointer_notify_button(session->seat, now, button,
            action == 1 ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
    }
    wlr_seat_pointer_notify_frame(session->seat);
}

void hgs_x11_scroll(struct hgs_x11_session *session, uint32_t xid, uint64_t generation,
                    double x, double y, int axis, double value) {
    struct wlr_xwayland_surface *window = target(session, xid, generation);
    if (!window || value == 0) return;
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
