/* 独立 Wayland 客户端：验证默认 host 的建窗、键鼠、尺寸协商及关闭后重连。
 * 只依赖标准 Wayland/xdg-shell，不调用 HGS、Wine 或 shell 私有 API。
 * 第一个参数是日志路径；用于在真机上留存原始协议回调。 */
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <stdarg.h>

static struct {
    struct wl_display *display;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct xdg_wm_base *wm;
    struct wl_seat *seat;
    struct wl_pointer *pointer;
    struct wl_keyboard *keyboard;
    struct wl_surface *surface;
    struct xdg_surface *xdg;
    struct xdg_toplevel *top;
    int running, width, height, maximized, mx, my;
    FILE *log;
} app;
static void record(const char *format, ...) {
    va_list args; va_start(args, format); vfprintf(app.log, format, args); va_end(args);
    fputc('\n', app.log); fflush(app.log);
}
struct buffer { struct wl_buffer *object; void *map; size_t bytes; };
static void released(void *data, struct wl_buffer *object) {
    struct buffer *b = data; wl_buffer_destroy(object); munmap(b->map, b->bytes); free(b);
}
static const struct wl_buffer_listener buffer_listener = { released };
static void paint(void) {
    struct buffer *b = calloc(1, sizeof(*b));
    b->bytes = (size_t)app.width * app.height * 4;
    int fd = memfd_create("hgs-service-client", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, b->bytes) < 0) { record("ERROR shm"); exit(2); }
    b->map = mmap(NULL, b->bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (b->map == MAP_FAILED) { record("ERROR mmap"); exit(2); }
    struct wl_shm_pool *pool = wl_shm_create_pool(app.shm, fd, b->bytes);
    b->object = wl_shm_pool_create_buffer(pool, 0, app.width, app.height, app.width * 4, WL_SHM_FORMAT_XRGB8888);
    wl_buffer_add_listener(b->object, &buffer_listener, b);
    wl_shm_pool_destroy(pool); close(fd);
    uint32_t *pixels = b->map;
    for (int y = 0; y < app.height; ++y) for (int x = 0; x < app.width; ++x) {
        uint32_t color = y < 40 ? 0x203040 : ((x < app.width/2) ? 0x2060c0 : 0x20a060);
        if (y < 40 && x >= app.width - 80) color = x >= app.width - 40 ? 0xd03030 : 0x30c050;
        pixels[(size_t)y * app.width + x] = color;
    }
    wl_surface_attach(app.surface, b->object, 0, 0);
    wl_surface_damage(app.surface, 0, 0, app.width, app.height);
    wl_surface_commit(app.surface); record("FRAME %d %d", app.width, app.height);
}
static void configured(void *data, struct xdg_surface *surface, uint32_t serial) {
    (void)data; xdg_surface_ack_configure(surface, serial); paint();
}
static const struct xdg_surface_listener surface_listener = { configured };
static void top_configure(void *data, struct xdg_toplevel *top, int32_t w, int32_t h, struct wl_array *states) {
    (void)data; (void)top; (void)states;
    if (w > 0) app.width = w; if (h > 0) app.height = h;
    record("CONFIGURE %d %d", w, h);
}
static void top_close(void *data, struct xdg_toplevel *top) {
    (void)data; (void)top; record("CLOSE_REQUESTED"); app.running = 0;
}
static const struct xdg_toplevel_listener top_listener = { .configure = top_configure, .close = top_close };
static void ping(void *data, struct xdg_wm_base *wm, uint32_t serial) { (void)data; xdg_wm_base_pong(wm, serial); }
static const struct xdg_wm_base_listener wm_listener = { ping };
static void pointer_enter(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s, wl_fixed_t x, wl_fixed_t y) {
    (void)d; (void)p; (void)serial; (void)s; app.mx = wl_fixed_to_int(x); app.my = wl_fixed_to_int(y); record("POINTER_ENTER");
}
static void pointer_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s) {
    (void)d; (void)p; (void)serial; (void)s;
}
static void pointer_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y) {
    (void)d; (void)p; (void)t; app.mx = wl_fixed_to_int(x); app.my = wl_fixed_to_int(y);
}
static void pointer_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t t, uint32_t button, uint32_t state) {
    (void)d; (void)p; (void)serial; (void)t; record("BUTTON %u %u %d %d", button, state, app.mx, app.my);
    if (state != WL_POINTER_BUTTON_STATE_PRESSED || app.my >= 40) return;
    if (app.mx >= app.width - 40) { record("CLOSE_BUTTON"); app.running = 0; }
    else if (app.mx >= app.width - 80) {
        app.maximized = !app.maximized;
        if (app.maximized) xdg_toplevel_set_maximized(app.top); else xdg_toplevel_unset_maximized(app.top);
        record("MAXIMIZE_REQUEST %d", app.maximized);
    }
}
static void pointer_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t axis, wl_fixed_t value) {
    (void)d; (void)p; (void)t; record("AXIS %u %d", axis, wl_fixed_to_int(value));
}
/* seat v5 会发 frame/source/stop/discrete；每个已协商事件都必须提供回调。 */
static void pointer_frame(void *d, struct wl_pointer *p) { (void)d; (void)p; }
static void pointer_source(void *d, struct wl_pointer *p, uint32_t source) { (void)d; (void)p; (void)source; }
static void pointer_stop(void *d, struct wl_pointer *p, uint32_t time, uint32_t axis) { (void)d; (void)p; (void)time; (void)axis; }
static void pointer_discrete(void *d, struct wl_pointer *p, uint32_t axis, int32_t value) { (void)d; (void)p; (void)axis; (void)value; }
static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter, .leave = pointer_leave, .motion = pointer_motion, .button = pointer_button, .axis = pointer_axis,
    .frame = pointer_frame, .axis_source = pointer_source, .axis_stop = pointer_stop, .axis_discrete = pointer_discrete
};
static void keymap(void *d, struct wl_keyboard *k, uint32_t format, int32_t fd, uint32_t size) {
    (void)d; (void)k; record("KEYMAP %u %u", format, size); close(fd);
}
static void keyboard_enter(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s, struct wl_array *keys) {
    (void)d; (void)k; (void)serial; (void)s; (void)keys; record("KEYBOARD_ENTER");
}
static void keyboard_leave(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s) { (void)d; (void)k; (void)serial; (void)s; }
static void key(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t t, uint32_t code, uint32_t state) {
    (void)d; (void)k; (void)serial; (void)t; record("KEY %u %u", code, state);
}
static void modifiers(void *d, struct wl_keyboard *k, uint32_t s, uint32_t a, uint32_t b, uint32_t c, uint32_t g) {
    (void)d; (void)k; (void)s; (void)a; (void)b; (void)c; (void)g;
}
static void repeat(void *d, struct wl_keyboard *k, int32_t rate, int32_t delay) { (void)d; (void)k; (void)rate; (void)delay; }
static const struct wl_keyboard_listener keyboard_listener = { keymap, keyboard_enter, keyboard_leave, key, modifiers, repeat };
static void capabilities(void *d, struct wl_seat *seat, uint32_t caps) {
    (void)d;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !app.pointer) {
        app.pointer = wl_seat_get_pointer(seat); wl_pointer_add_listener(app.pointer, &pointer_listener, NULL);
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !app.keyboard) {
        app.keyboard = wl_seat_get_keyboard(seat); wl_keyboard_add_listener(app.keyboard, &keyboard_listener, NULL);
    }
}
static void seat_name(void *d, struct wl_seat *s, const char *name) { (void)d; (void)s; (void)name; }
static const struct wl_seat_listener seat_listener = { capabilities, seat_name };
static void global(void *d, struct wl_registry *r, uint32_t id, const char *name, uint32_t version) {
    (void)d;
    if (!strcmp(name, "wl_compositor")) app.compositor = wl_registry_bind(r, id, &wl_compositor_interface, version < 4 ? version : 4);
    else if (!strcmp(name, "wl_shm")) app.shm = wl_registry_bind(r, id, &wl_shm_interface, 1);
    else if (!strcmp(name, "xdg_wm_base")) { app.wm = wl_registry_bind(r, id, &xdg_wm_base_interface, 1); xdg_wm_base_add_listener(app.wm, &wm_listener, NULL); }
    else if (!strcmp(name, "wl_seat")) { app.seat = wl_registry_bind(r, id, &wl_seat_interface, version < 5 ? version : 5); wl_seat_add_listener(app.seat, &seat_listener, NULL); }
}
static void global_remove(void *d, struct wl_registry *r, uint32_t id) { (void)d; (void)r; (void)id; }
static const struct wl_registry_listener registry_listener = { global, global_remove };
int main(int argc, char **argv) {
    memset(&app, 0, sizeof(app)); app.width = 480; app.height = 320; app.running = 1;
    app.log = argc > 1 ? fopen(argv[1], "w") : stderr; if (!app.log) return 2;
    app.display = wl_display_connect(NULL); if (!app.display) { record("ERROR connect"); return 1; }
    struct wl_registry *registry = wl_display_get_registry(app.display); wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(app.display); wl_display_roundtrip(app.display);
    if (!app.compositor || !app.shm || !app.wm) { record("ERROR globals"); return 1; }
    app.surface = wl_compositor_create_surface(app.compositor);
    app.xdg = xdg_wm_base_get_xdg_surface(app.wm, app.surface); xdg_surface_add_listener(app.xdg, &surface_listener, NULL);
    app.top = xdg_surface_get_toplevel(app.xdg); xdg_toplevel_add_listener(app.top, &top_listener, NULL);
    xdg_toplevel_set_title(app.top, "P1 HGS client"); xdg_toplevel_set_app_id(app.top, "org.hgs.smoke");
    wl_surface_commit(app.surface); record("CONNECTED");
    while (app.running && wl_display_dispatch(app.display) >= 0) {}
    xdg_toplevel_destroy(app.top); xdg_surface_destroy(app.xdg); wl_surface_destroy(app.surface);
    wl_display_flush(app.display); wl_display_disconnect(app.display);
    record("EXIT"); if (app.log != stderr) fclose(app.log); return 0;
}
