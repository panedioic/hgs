// thirdparty/wayland-smoke/wayland_smoke.c — Wayland 冒烟客户端（so 化，命令表 wlsmoke）
//
// 验证 HGS 合成器的基础窗口能力（替代旧 wldemo，画得更"像窗口"）：
//   1. 基础图形显示：wl_shm(XRGB8888) 动画 —— 渐变底色 + 反弹方块 + 内容边框，
//      帧 callback 驱动持续重绘（vsync 节奏）；
//   2. 客户端自绘标题栏：标题文字（内置 5x7 点阵字体，无字体库依赖）
//      + 最小化/最大化/关闭三个按钮，带 hover 高亮；
//   3. 按钮行为：关闭 → 退出进程；最大化 → xdg_toplevel set/unset_maximized；
//      最小化 → xdg_toplevel_set_minimized；标题栏按下 → xdg_toplevel_move；
//   4. 尺寸协商：configure/ack_configure + 双缓冲重分配（最大化/还原跟手）。
//
// 运行环境：BetterSH shell 内 `wlsmoke` 命令（procmgr dlopen 本 so 调 main），
// 连接经 WAYLAND_DISPLAY/XDG_RUNTIME_DIR 指向 HGS server。
//
// 零第三方依赖：不链 hilog/libm，日志走 stderr。

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#include "wayland-client.h"
#include "wayland-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001
#endif

#define DEF_W   480
#define DEF_H   320
#define TITLE_H 32      /* 标题栏高度(px) */
#define BTN_W   40      /* 每个装饰按钮宽度(右侧三连) */

/* 调色板 (0xRRGGBB; 写像素时转 BGR 字节序) */
#define COL_TITLE_BG    0x1B242Cu
#define COL_BTN_BG      0x202A34u
#define COL_BTN_HOVER   0x3A4A5Au
#define COL_TITLE_FG    0xE8EAEDu
#define COL_SEPARATOR   0x3C4650u
#define COL_CONTENT_A   0x103050u
#define COL_CONTENT_B   0x40A0D0u

/* ── 5x7 点阵字体(仅标题用到的字符; 列字节, bit0=顶行) ─────────────────── */
struct Glyph { char ch; unsigned char col[5]; };
static const struct Glyph kFont[] = {
    { 'a', { 0x20, 0x54, 0x54, 0x54, 0x78 } },
    { 'd', { 0x3E, 0x42, 0x42, 0x42, 0x3C } },
    { 'e', { 0x3C, 0x4A, 0x4A, 0x4A, 0x30 } },
    { 'k', { 0x7F, 0x08, 0x14, 0x22, 0x41 } },
    { 'l', { 0x7F, 0x40, 0x40, 0x40, 0x40 } },
    { 'm', { 0x7C, 0x04, 0x18, 0x04, 0x78 } },
    { 'n', { 0x7C, 0x08, 0x04, 0x04, 0x78 } },
    { 'o', { 0x38, 0x44, 0x44, 0x44, 0x38 } },
    { 's', { 0x48, 0x54, 0x54, 0x54, 0x20 } },
    { 'w', { 0x3C, 0x40, 0x30, 0x40, 0x3C } },
    { 'y', { 0x38, 0x40, 0x40, 0x22, 0x1C } },
    { '-', { 0x08, 0x08, 0x08, 0x08, 0x08 } },
    { ' ', { 0x00, 0x00, 0x00, 0x00, 0x00 } },
};

/* ── 全局状态 ──────────────────────────────────────────────────────────── */
struct Ctx {
    struct wl_display* dpy;
    struct wl_registry* reg;
    struct wl_compositor* comp;
    struct wl_shm* shm;
    struct wl_seat* seat;
    struct wl_pointer* ptr;
    struct xdg_wm_base* wm;

    struct wl_surface* surf;
    struct xdg_surface* xsurf;
    struct xdg_toplevel* top;

    struct wl_buffer* buf[2];
    uint8_t* px[2];
    size_t px_len;
    int cur;

    int w, h;
    int configured;
    int maximized;
    int running;
    int frames_done;

    /* 指针状态(表面坐标) */
    int entered;
    int mx, my;          /* 当前位置 */
    int hover;           /* 0=min 1=max 2=close -1=无 */
};

static struct Ctx g;

/* ── 小工具 ────────────────────────────────────────────────────────────── */
static int shm_fd_create(size_t size) {
#ifdef SYS_memfd_create
    int fd = (int)syscall(SYS_memfd_create, "wlsmoke", MFD_CLOEXEC);
#else
    int fd = -1;
#endif
    if (fd < 0) {
        char path[] = "/tmp/wlsmoke-XXXXXX";
        fd = mkstemp(path);
        if (fd >= 0) unlink(path);
    }
    if (fd < 0) return -1;
    if (ftruncate(fd, (off_t)size) != 0) { close(fd); return -1; }
    return fd;
}

static int in_rect(int px, int py, int x, int y, int w, int h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

/* 按钮矩形: 0=min 1=max 2=close, 从右往左排 */
static void btn_rect(int i, int* x, int* y, int* bw, int* bh) {
    *bw = BTN_W;
    *bh = TITLE_H;
    *y = 0;
    *x = g.w - (i + 1) * BTN_W;      /* i=2(close) 最靠右 */
}

static void put_px(uint8_t* base, int w, int x, int y, uint32_t rgb) {
    uint8_t* p = base + ((size_t)y * w + x) * 4;
    p[0] = (uint8_t)(rgb & 0xFF);         /* B */
    p[1] = (uint8_t)((rgb >> 8) & 0xFF);  /* G */
    p[2] = (uint8_t)((rgb >> 16) & 0xFF); /* R */
    p[3] = 0xFF;
}

static void fill_rect(uint8_t* base, int w, int h,
                      int x, int y, int rw, int rh, uint32_t rgb) {
    if (x < 0) { rw += x; x = 0; }
    if (y < 0) { rh += y; y = 0; }
    for (int yy = y; yy < y + rh && yy < h; yy++)
        for (int xx = x; xx < x + rw && xx < w; xx++)
            put_px(base, w, xx, yy, rgb);
}

static void draw_glyph(uint8_t* base, int w, int x, int y,
                       const struct Glyph* gl, uint32_t rgb, int scale) {
    for (int c = 0; c < 5; c++) {
        for (int r = 0; r < 7; r++) {
            if (gl->col[c] & (1u << r)) {
                for (int sy = 0; sy < scale; sy++)
                    for (int sx = 0; sx < scale; sx++)
                        put_px(base, w, x + c * scale + sx, y + r * scale + sy, rgb);
            }
        }
    }
}

static void draw_text(uint8_t* base, int w, int x, int y,
                      const char* s, uint32_t rgb, int scale) {
    for (; *s; s++) {
        const struct Glyph* gl = NULL;
        for (size_t i = 0; i < sizeof(kFont) / sizeof(kFont[0]); i++) {
            if (kFont[i].ch == *s) { gl = &kFont[i]; break; }
        }
        if (gl) draw_glyph(base, w, x, y, gl, rgb, scale);
        x += 6 * scale;   /* 5 列 + 1 列间距 */
    }
}

/* ── 绘制 ──────────────────────────────────────────────────────────────── */
static void draw_decorations(uint8_t* base) {
    const int w = g.w;
    /* 标题栏底 */
    fill_rect(base, w, g.h, 0, 0, w, TITLE_H, COL_TITLE_BG);
    /* 标题文字(2x 缩放 = 10x14), 垂直居中 */
    draw_text(base, w, 12, (TITLE_H - 14) / 2, "wayland-smoke",
              COL_TITLE_FG, 2);

    /* 三个按钮: hover 高亮 + 图标 */
    for (int i = 0; i < 3; i++) {
        int bx, by, bw, bh;
        btn_rect(i, &bx, &by, &bw, &bh);
        if (bx < 0) break;                       /* 窗口太窄, 按钮放不下 */
        fill_rect(base, w, g.h, bx, by, bw, bh,
                  g.hover == i ? COL_BTN_HOVER : COL_BTN_BG);
        if (i < 2)                                /* min/max 与相邻分隔线 */
            fill_rect(base, w, g.h, bx - 1, 0, 1, TITLE_H, COL_SEPARATOR);

        const int cx = bx + bw / 2;
        const int cy = bh / 2;
        if (i == 0) {                             /* 最小化: 底部横线 */
            fill_rect(base, w, g.h, cx - 7, cy + 4, 14, 2, COL_TITLE_FG);
        } else if (i == 1) {                      /* 最大化: 方框(最大化态画还原双框) */
            if (!g.maximized) {
                for (int k = 0; k < 12; k++) {
                    put_px(base, w, cx - 7 + k, cy - 6, COL_TITLE_FG);
                    put_px(base, w, cx - 7 + k, cy + 5, COL_TITLE_FG);
                }
                for (int k = 0; k < 12; k++) {
                    put_px(base, w, cx - 7, cy - 6 + k, COL_TITLE_FG);
                    put_px(base, w, cx + 6, cy - 6 + k, COL_TITLE_FG);
                }
            } else {                              /* 还原: 前后两个小框 */
                for (int k = 0; k < 8; k++) {
                    put_px(base, w, cx - 3 + k, cy - 6, COL_TITLE_FG);
                    put_px(base, w, cx - 3, cy - 6 + k, COL_TITLE_FG);
                }
                for (int k = 0; k < 9; k++) {
                    put_px(base, w, cx - 7 + k, cy - 2, COL_TITLE_FG);
                    put_px(base, w, cx - 7 + k, cy + 6, COL_TITLE_FG);
                    put_px(base, w, cx - 7, cy - 2 + k, COL_TITLE_FG);
                    put_px(base, w, cx + 1, cy - 2 + k, COL_TITLE_FG);
                }
            }
        } else {                                  /* 关闭: X 两条对角线 */
            for (int k = 0; k < 12; k++) {
                put_px(base, w, cx - 6 + k, cy - 6 + k, COL_TITLE_FG);
                put_px(base, w, cx - 6 + k, cy + 5 - k, COL_TITLE_FG);
            }
        }
    }
    /* 标题栏下沿分隔线 */
    fill_rect(base, w, g.h, 0, TITLE_H, w, 1, COL_SEPARATOR);
}

static void draw_content(uint8_t* base, uint32_t t_ms) {
    const int w = g.w, h = g.h;
    /* 渐变底(随时间流动) */
    const uint32_t phase = (t_ms / 8) % 64;
    for (int y = TITLE_H; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t v = (uint32_t)((x / 2 + y / 2 + phase) % 64);
            uint32_t rgb = COL_CONTENT_A
                + (v << 16) / 64 * 2          /* R 分量随 v 增大 */
                + (v << 8) / 64 * 6;          /* G 分量 */
            /* 粗条纹理, 便于观察滚动方向 */
            if ((x + y / 2 + (int)(t_ms / 8)) % 48 < 6) rgb = COL_CONTENT_B;
            put_px(base, w, x, y, rgb);
        }
    }
    /* 反弹方块(三角波, 无 libm) */
    const int side = (h - TITLE_H) / 6;
    const uint32_t range_x = (uint32_t)(w - side > 0 ? w - side : 1);
    const uint32_t range_y = (uint32_t)(h - side - TITLE_H > 0 ? h - side - TITLE_H : 1);
    uint32_t tx = (t_ms / 12) % (2 * range_x);
    uint32_t ty = (t_ms / 17) % (2 * range_y);
    int bx = (int)(tx >= range_x ? 2 * range_x - tx : tx);
    int by = (int)(ty >= range_y ? 2 * range_y - ty : ty) + TITLE_H;
    fill_rect(base, w, h, bx, by, side, side, 0x101010u);
    fill_rect(base, w, h, bx + 3, by + 3, side - 6, side - 6, 0xF0B429u);
    /* 内容区边框 */
    for (int x = 0; x < w; x++) {
        put_px(base, w, x, TITLE_H + 1, 0xFFFFFFu);
        put_px(base, w, x, h - 1, 0xFFFFFFu);
    }
    for (int y = TITLE_H; y < h; y++) {
        put_px(base, w, 0, y, 0xFFFFFFu);
        put_px(base, w, w - 1, y, 0xFFFFFFu);
    }
}

static void render_frame(uint32_t t_ms) {
    uint8_t* base = g.px[g.cur];
    if (!base || g.w <= 0 || g.h <= 0) return;
    draw_content(base, t_ms);
    draw_decorations(base);
}

/* ── 缓冲管理(双缓冲, resize 时重建) ───────────────────────────────────── */
static void create_buffers(void) {
    for (int i = 0; i < 2; i++) {
        if (g.px[i]) { munmap(g.px[i], g.px_len); g.px[i] = NULL; }
        if (g.buf[i]) { wl_buffer_destroy(g.buf[i]); g.buf[i] = NULL; }
    }
    size_t stride = (size_t)g.w * 4;
    g.px_len = stride * (size_t)g.h;
    for (int i = 0; i < 2; i++) {
        int fd = shm_fd_create(g.px_len);
        if (fd < 0) { fprintf(stderr, "wlsmoke: shm fd failed\n"); return; }
        struct wl_shm_pool* pool = wl_shm_create_pool(g.shm, fd, (int32_t)g.px_len);
        g.buf[i] = wl_shm_pool_create_buffer(pool, 0, g.w, g.h,
                                             (int32_t)stride,
                                             WL_SHM_FORMAT_XRGB8888);
        wl_shm_pool_destroy(pool);
        g.px[i] = mmap(NULL, g.px_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
        if (g.px[i] == MAP_FAILED) { g.px[i] = NULL; return; }
    }
    g.cur = 0;
}

/* ── 帧 callback: vsync 驱动的持续重绘 ─────────────────────────────────── */
static const struct wl_callback_listener kFrameListener;

static void frame_done(void* data, struct wl_callback* cb, uint32_t timestamp) {
    wl_callback_destroy(cb);
    struct Ctx* c = data;
    if (!c->running || !c->px[0]) return;
    /* 节流到 ~30fps: 不限帧率会打满 CPU 且日志/合成器全被冲爆 */
    static struct timespec last = {0, 0};
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    long elapsed_ms = (ts.tv_sec - last.tv_sec) * 1000L
                    + (ts.tv_nsec - last.tv_nsec) / 1000000L;
    if (elapsed_ms < 33) return;   /* 下一帧 callback 再试 */
    last = ts;
    /* 只连续动画 3 帧, 之后停止 commit (保持存活不洪泛 hilog/合成器日志) */
    if (++c->frames_done > 3) return;
    c->cur ^= 1;   /* 交替缓冲, 不写服务端可能还在读的那张 */
    render_frame((uint32_t)(ts.tv_sec * 1000u + (uint32_t)(ts.tv_nsec / 1000000u)));
    wl_surface_attach(c->surf, c->buf[c->cur], 0, 0);
    wl_surface_damage(c->surf, 0, 0, c->w, c->h);
    struct wl_callback* ncb = wl_surface_frame(c->surf);
    wl_callback_add_listener(ncb, &kFrameListener, c);
    wl_surface_commit(c->surf);
}
static const struct wl_callback_listener kFrameListener = { frame_done };

/* ── xdg_surface / xdg_toplevel ────────────────────────────────────────── */
static void xsurf_configure(void* data, struct xdg_surface* xs, uint32_t serial) {
    struct Ctx* c = data;
    xdg_surface_ack_configure(xs, serial);
    if (!c->configured) {
        c->configured = 1;
        create_buffers();
        if (!c->px[0]) return;
        render_frame(0);
        wl_surface_attach(c->surf, c->buf[0], 0, 0);
        wl_surface_damage(c->surf, 0, 0, c->w, c->h);
        struct wl_callback* cb = wl_surface_frame(c->surf);
        wl_callback_add_listener(cb, &kFrameListener, c);
        wl_surface_commit(c->surf);
        fprintf(stderr, "wlsmoke: window up %dx%d\n", c->w, c->h);
        fflush(stderr);
    }
}
static const struct xdg_surface_listener kXSurfListener = { xsurf_configure };

static void top_configure(void* data, struct xdg_toplevel* top, int32_t w, int32_t h,
                          struct wl_array* states) {
    (void)top;
    struct Ctx* c = data;
    c->maximized = 0;
    uint32_t* st;
    wl_array_for_each(st, states) {
        if (*st == XDG_TOPLEVEL_STATE_MAXIMIZED) c->maximized = 1;
    }
    if (w > 0 && h > 0 && (w != c->w || h != c->h)) {
        c->w = w;
        c->h = h;
        if (c->configured) create_buffers();   /* 下一帧 callback 用新尺寸 */
    }
}
static void top_close(void* data, struct xdg_toplevel* top) {
    (void)top;
    ((struct Ctx*)data)->running = 0;
}
static void top_configure_bounds(void* data, struct xdg_toplevel* top,
                                 int32_t min_w, int32_t min_h) {
    (void)data; (void)top; (void)min_w; (void)min_h;
}
static void top_wm_capabilities(void* data, struct xdg_toplevel* top,
                                struct wl_array* caps) {
    (void)data; (void)top; (void)caps;
}

static const struct xdg_toplevel_listener kTopListener = {
    top_configure, top_close, top_configure_bounds, top_wm_capabilities,
};

static void wm_ping(void* data, struct xdg_wm_base* wm, uint32_t serial) {
    (void)data;
    xdg_wm_base_pong(wm, serial);
}
static const struct xdg_wm_base_listener kWmListener = { wm_ping };

/* ── pointer: hover 高亮 + 按钮动作 + 标题栏拖动 ────────────────────────── */
static void ptr_enter(void* data, struct wl_pointer* ptr, uint32_t serial,
                      struct wl_surface* surf, wl_fixed_t sx, wl_fixed_t sy) {
    (void)ptr; (void)serial; (void)surf;
    struct Ctx* c = data;
    c->entered = 1;
    c->mx = wl_fixed_to_int(sx);
    c->my = wl_fixed_to_int(sy);
}
static void ptr_leave(void* data, struct wl_pointer* ptr, uint32_t serial,
                      struct wl_surface* surf) {
    (void)ptr; (void)serial; (void)surf;
    struct Ctx* c = data;
    c->entered = 0;
    c->hover = -1;
}
static void ptr_motion(void* data, struct wl_pointer* ptr, uint32_t time,
                       wl_fixed_t sx, wl_fixed_t sy) {
    (void)ptr; (void)time;
    struct Ctx* c = data;
    c->mx = wl_fixed_to_int(sx);
    c->my = wl_fixed_to_int(sy);
    c->hover = -1;
    if (c->entered && c->my < TITLE_H) {
        for (int i = 0; i < 3; i++) {
            int bx, by, bw, bh;
            btn_rect(i, &bx, &by, &bw, &bh);
            if (bx >= 0 && in_rect(c->mx, c->my, bx, by, bw, bh)) {
                c->hover = i;
                break;
            }
        }
    }
}
static void ptr_button(void* data, struct wl_pointer* ptr, uint32_t serial,
                       uint32_t time, uint32_t button, uint32_t state) {
    (void)ptr; (void)time;
    struct Ctx* c = data;
    if (state != WL_POINTER_BUTTON_STATE_PRESSED || button != 0x110) return;
    if (c->entered && c->my < TITLE_H) {
        for (int i = 0; i < 3; i++) {
            int bx, by, bw, bh;
            btn_rect(i, &bx, &by, &bw, &bh);
            if (bx < 0 || !in_rect(c->mx, c->my, bx, by, bw, bh)) continue;
            if (i == 2) {
                fprintf(stderr, "wlsmoke: close requested\n");
                c->running = 0;
            } else if (i == 1) {
                if (c->maximized) xdg_toplevel_unset_maximized(c->top);
                else xdg_toplevel_set_maximized(c->top);
            } else {
                xdg_toplevel_set_minimized(c->top);
            }
            return;
        }
        /* 标题栏空白区按下 → 交给 compositor 交互式移动 */
        if (g.seat) xdg_toplevel_move(c->top, g.seat, serial);
    }
}
static void ptr_axis(void* data, struct wl_pointer* ptr, uint32_t time,
                     uint32_t axis, wl_fixed_t value) {
    (void)data; (void)ptr; (void)time; (void)axis; (void)value;
}

/* v5+ 附加事件: compositor 可能发, 监听器留 NULL 会被 libwayland abort
 * ("listener function for opcode N is NULL") — 全部给空实现 */
static void ptr_frame(void* data, struct wl_pointer* ptr) {
    (void)data; (void)ptr;
}
static void ptr_axis_source(void* data, struct wl_pointer* ptr, uint32_t src) {
    (void)data; (void)ptr; (void)src;
}
static void ptr_axis_stop(void* data, struct wl_pointer* ptr, uint32_t time, uint32_t axis) {
    (void)data; (void)ptr; (void)time; (void)axis;
}
static void ptr_axis_discrete(void* data, struct wl_pointer* ptr, uint32_t axis, int32_t steps) {
    (void)data; (void)ptr; (void)axis; (void)steps;
}
static void ptr_axis_value120(void* data, struct wl_pointer* ptr, uint32_t axis, int32_t v120) {
    (void)data; (void)ptr; (void)axis; (void)v120;
}
static void ptr_axis_relative_direction(void* data, struct wl_pointer* ptr, uint32_t axis, uint32_t dir) {
    (void)data; (void)ptr; (void)axis; (void)dir;
}

/* 指派初始化: 版本演进新增字段自动置零, 不触发 -Wmissing-field-initializers */
static const struct wl_pointer_listener kPtrListener = {
    .enter = ptr_enter,
    .leave = ptr_leave,
    .motion = ptr_motion,
    .button = ptr_button,
    .axis = ptr_axis,
    .frame = ptr_frame,
    .axis_source = ptr_axis_source,
    .axis_stop = ptr_axis_stop,
    .axis_discrete = ptr_axis_discrete,
    .axis_value120 = ptr_axis_value120,
    .axis_relative_direction = ptr_axis_relative_direction,
};

/* ── seat / registry ───────────────────────────────────────────────────── */
static void seat_caps(void* data, struct wl_seat* seat, uint32_t caps) {
    struct Ctx* c = data;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !c->ptr) {
        c->ptr = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(c->ptr, &kPtrListener, c);
    }
}
static void seat_name(void* data, struct wl_seat* seat, const char* name) {
    (void)data; (void)seat; (void)name;
}

static const struct wl_seat_listener kSeatListener = { seat_caps, seat_name };

static void reg_global(void* data, struct wl_registry* reg, uint32_t name,
                       const char* iface, uint32_t ver) {
    struct Ctx* c = data;
    if (strcmp(iface, wl_compositor_interface.name) == 0) {
        c->comp = wl_registry_bind(reg, name, &wl_compositor_interface,
                                   ver < 4 ? ver : 4);
    } else if (strcmp(iface, wl_shm_interface.name) == 0) {
        c->shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    } else if (strcmp(iface, xdg_wm_base_interface.name) == 0) {
        c->wm = wl_registry_bind(reg, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(c->wm, &kWmListener, c);
    } else if (strcmp(iface, wl_seat_interface.name) == 0) {
        c->seat = wl_registry_bind(reg, name, &wl_seat_interface, ver < 5 ? ver : 5);
        wl_seat_add_listener(c->seat, &kSeatListener, c);
    }
}
static void reg_remove(void* data, struct wl_registry* reg, uint32_t name) {
    (void)data; (void)reg; (void)name;
}
static const struct wl_registry_listener kRegListener = { reg_global, reg_remove };

/* ── 信号 ──────────────────────────────────────────────────────────────── */
static void on_signal(int sig) { (void)sig; g.running = 0; }

/* ── 入口 (命令表 wlsmoke → dlopen + main) ─────────────────────────────── */
int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    memset(&g, 0, sizeof g);
    g.w = DEF_W;
    g.h = DEF_H;
    g.running = 1;
    g.hover = -1;

    g.dpy = wl_display_connect(NULL);
    if (!g.dpy) {
        fprintf(stderr, "wlsmoke: connect failed (WAYLAND_DISPLAY=%s)\n",
                getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "(unset)");
        return 1;
    }

    g.reg = wl_display_get_registry(g.dpy);
    wl_registry_add_listener(g.reg, &kRegListener, &g);
    wl_display_roundtrip(g.dpy);
    wl_display_roundtrip(g.dpy);   /* 第二轮取 seat capabilities */
    if (!g.comp || !g.shm || !g.wm) {
        fprintf(stderr, "wlsmoke: missing globals comp=%p shm=%p wm=%p\n",
                (void*)g.comp, (void*)g.shm, (void*)g.wm);
        return 1;
    }

    g.surf = wl_compositor_create_surface(g.comp);
    g.xsurf = xdg_wm_base_get_xdg_surface(g.wm, g.surf);
    xdg_surface_add_listener(g.xsurf, &kXSurfListener, &g);
    g.top = xdg_surface_get_toplevel(g.xsurf);
    xdg_toplevel_add_listener(g.top, &kTopListener, &g);
    xdg_toplevel_set_title(g.top, "wayland-smoke");
    xdg_toplevel_set_app_id(g.top, "bettersh.wlsmoke");
    wl_surface_commit(g.surf);
    wl_display_roundtrip(g.dpy);

    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGPIPE, SIG_IGN);   /* compositor 先断开时别被 SIGPIPE 杀掉 */

    while (g.running) {
        if (wl_display_dispatch(g.dpy) < 0) {
            int e = wl_display_get_error(g.dpy);
            fprintf(stderr, "wlsmoke: dispatch failed: %s (%d)\n", strerror(e), e);
            break;
        }
    }

    for (int i = 0; i < 2; i++) {
        if (g.px[i]) munmap(g.px[i], g.px_len);
        if (g.buf[i]) wl_buffer_destroy(g.buf[i]);
    }
    if (g.ptr) wl_pointer_destroy(g.ptr);
    if (g.seat) wl_seat_destroy(g.seat);
    if (g.top) xdg_toplevel_destroy(g.top);
    if (g.xsurf) xdg_surface_destroy(g.xsurf);
    if (g.surf) wl_surface_destroy(g.surf);
    if (g.wm) xdg_wm_base_destroy(g.wm);
    if (g.shm) wl_shm_destroy(g.shm);
    if (g.comp) wl_compositor_destroy(g.comp);
    wl_registry_destroy(g.reg);
    wl_display_disconnect(g.dpy);
    fprintf(stderr, "wlsmoke: bye\n");
    return 0;
}
