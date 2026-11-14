/* gtk_smoke.c — GTK3 冒烟客户端（HGS 合成器用）
 *
 * 目的：验证 HGS 内嵌 Wayland server 能承载一个真实 GUI 工具链。GTK3 起不来，
 * Electron 一类（桌面版 VS Code）一定起不来；GTK 起来了才谈得上后面的 ozone/sandbox。
 * 与 wayland-smoke（手写 wl_shm，验证合成器协议面）互补：本程序验证的是
 * GDK-wayland 后端 + Pango 文字 + Cairo 渲染 + 输入事件整条链路。
 *
 * 覆盖点（每个都对应 HGS 的一条通路）：
 *   1. 连接 wl_display + registry + seat          —— HGS server 生命周期
 *   2. xdg_toplevel + configure/ack               —— 窗口协商（wlsmoke 也测）
 *   3. GtkHeaderBar (CSD)                         —— 自绘装饰 + Pango 文字
 *   4. GtkButton 点击计数                          —— 指针输入：HGS native input →
 *                                                    seat → GDK 事件
 *   5. GtkEntry                                    —— 键盘输入：HGS native keymap →
 *                                                    wl_keyboard → GTK IM
 *   6. DrawingArea + tick callback 动画            —— frame clock/持续重绘
 *   7. 最小化/最大化/还原按钮                       —— xdg_toplevel set/unset_maximized、
 *                                                    set_minimized（与 wlsmoke 同源）
 *   8. 关闭按钮                                    —— 窗口销毁链路
 *
 * 编法与设备上怎么跑见 scripts/smoke/build_gtk_smoke.sh（依赖 BetterSH 的
 * weston-deps/glib/qemu-pcre2/gtk 三层交叉产物，脚本头有说明）。
 */
#include <gtk/gtk.h>
#include <math.h>
#include <stdio.h>

typedef struct {
    GtkWidget* win;
    GtkWidget* entry;
    GtkWidget* counter_label;
    GtkWidget* draw;
    GtkWidget* max_btn;
    int clicks;
    int frames;
    gboolean maximized;
    guint tick_id;
} Smoke;

static Smoke g_smoke = {0};

/* ── 动画：底色渐变条 + 帧数，证明 frame clock 在转 ─────────────────────── */
static gboolean on_tick(GtkWidget* widget, GdkFrameClock* clock, gpointer data)
{
    (void)clock; (void)data;
    g_smoke.frames++;
    gtk_widget_queue_draw(widget);
    return G_SOURCE_CONTINUE;
}

static gboolean draw_cb(GtkWidget* widget, cairo_t* cr, gpointer data)
{
    (void)data;
    guint w = gtk_widget_get_allocated_width(widget);
    guint h = gtk_widget_get_allocated_height(widget);
    guint64 now = g_get_monotonic_time();

    /* 底：缓慢横移的渐变条（cairot 用法与 frame 时钟的最小验证） */
    double shift = (now / 16000) % 400;  /* ~25fps 观感，别太闪 */
    int bars = 24;
    for (int i = 0; i < bars; i++) {
        double t = ((double)i / bars + shift / 400.0);
        t -= floor(t);
        double r = 0.15 + 0.55 * t;
        double gg = 0.75 - 0.4 * t;
        double b = 0.85 - 0.3 * t;
        cairo_set_source_rgb(cr, r, gg, b);
        cairo_rectangle(cr, w * i / (double)bars, 0, w / (double)bars + 1, h);
        cairo_fill(cr);
    }

    /* 前景：帧数与输入状态 */
    char line[128];
    snprintf(line, sizeof line, "frames=%d  clicks=%d", g_smoke.frames, g_smoke.clicks);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, h / 8.0);
    cairo_move_to(cr, 16, h / 2.0);
    cairo_show_text(cr, line);
    return FALSE;
}

/* ── 输入 ──────────────────────────────────────────────────────────────── */
static void on_count_clicked(GtkButton* b, gpointer data)
{
    (void)b; (void)data;
    g_smoke.clicks++;
    char buf[64];
    snprintf(buf, sizeof buf, "clicked ×%d", g_smoke.clicks);
    gtk_label_set_text(GTK_LABEL(g_smoke.counter_label), buf);
    g_print("gtksmoke: click #%d —— 指针输入链路 OK\n", g_smoke.clicks);
}

static void on_entry_activate(GtkEntry* e, gpointer data)
{
    (void)data;
    const char* t = gtk_entry_get_text(e);
    g_print("gtksmoke: entry='%s' —— 键盘输入链路 OK\n", t);
    gtk_entry_set_text(e, "");
}

/* ── 窗口状态按钮：与 wlsmoke 走同一条 xdg_toplevel 请求路径 ────────────── */
static void on_minimize(GtkButton* b, gpointer data)
{
    (void)b; (void)data;
    gtk_window_iconify(GTK_WINDOW(g_smoke.win));
    g_print("gtksmoke: iconify —— set_minimized 已发\n");
}

static void on_maximize(GtkButton* b, gpointer data)
{
    (void)b; (void)data;
    if (g_smoke.maximized) {
        gtk_window_unmaximize(GTK_WINDOW(g_smoke.win));
        g_print("gtksmoke: unmaximize —— unset_maximized 已发\n");
    } else {
        gtk_window_maximize(GTK_WINDOW(g_smoke.win));
        g_print("gtksmoke: maximize —— set_maximized 已发\n");
    }
}

static gboolean on_win_state(GtkWidget* w, GdkEvent* e, gpointer data)
{
    (void)w; (void)data;
    GdkEventWindowState* st = (GdkEventWindowState*)e;
    if (st->changed_mask & GDK_WINDOW_STATE_MAXIMIZED) {
        g_smoke.maximized = (st->new_window_state & GDK_WINDOW_STATE_MAXIMIZED) != 0;
        gtk_button_set_label(GTK_BUTTON(g_smoke.max_btn),
                             g_smoke.maximized ? "还原" : "最大化");
        g_print("gtksmoke: state-event maximized=%d（configure 已回）\n",
                g_smoke.maximized);
    }
    return FALSE;
}

/* ── 装配 ──────────────────────────────────────────────────────────────── */
static void build_ui(void)
{
    Smoke* s = &g_smoke;

    s->win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(s->win), "gtksmoke");
    gtk_window_set_default_size(GTK_WINDOW(s->win), 480, 360);


    /* CSD 头栏：GTK wayland 后端自绘装饰 —— 验证文字渲染与按钮布局 */
    GtkWidget* header = gtk_header_bar_new();
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(header), TRUE);
    gtk_header_bar_set_title(GTK_HEADER_BAR(header), "gtksmoke");
    gtk_header_bar_set_has_subtitle(GTK_HEADER_BAR(header), FALSE);
    gtk_window_set_titlebar(GTK_WINDOW(s->win), header);

    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);

    s->draw = gtk_drawing_area_new();
    gtk_widget_set_size_request(s->draw, -1, 96);
    g_signal_connect(s->draw, "draw", G_CALLBACK(draw_cb), NULL);
    s->tick_id = gtk_widget_add_tick_callback(s->draw, on_tick, NULL, NULL);
    gtk_box_pack_start(GTK_BOX(box), s->draw, TRUE, TRUE, 0);

    s->counter_label = gtk_label_new("clicked ×0");
    gtk_box_pack_start(GTK_BOX(box), s->counter_label, FALSE, FALSE, 0);

    GtkWidget* count = gtk_button_new_with_label("点我计数（指针）");
    g_signal_connect(count, "clicked", G_CALLBACK(on_count_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(box), count, FALSE, FALSE, 0);

    s->entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(s->entry), "输入文字后回车（键盘）");
    g_signal_connect(s->entry, "activate", G_CALLBACK(on_entry_activate), NULL);
    gtk_box_pack_start(GTK_BOX(box), s->entry, FALSE, FALSE, 0);

    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget* bmin = gtk_button_new_with_label("最小化");
    g_signal_connect(bmin, "clicked", G_CALLBACK(on_minimize), NULL);
    s->max_btn = gtk_button_new_with_label("最大化");
    g_signal_connect(s->max_btn, "clicked", G_CALLBACK(on_maximize), NULL);
    GtkWidget* bclose = gtk_button_new_with_label("关闭");
    g_signal_connect(bclose, "clicked", G_CALLBACK(gtk_window_close), s->win);
    gtk_box_pack_start(GTK_BOX(row), bmin, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(row), s->max_btn, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(row), bclose, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);

    gtk_container_add(GTK_CONTAINER(s->win), box);
    g_signal_connect(s->win, "window-state-event", G_CALLBACK(on_win_state), NULL);
    gtk_widget_show_all(s->win);

    g_print("gtksmoke: 窗口已建 —— alloc=%dx%d gdk=%dx%d scale=%d\n",
            gtk_widget_get_allocated_width(s->win), gtk_widget_get_allocated_height(s->win),
            gdk_window_get_width(gtk_widget_get_window(s->win)),
            gdk_window_get_height(gtk_widget_get_window(s->win)),
            gdk_window_get_scale_factor(gtk_widget_get_window(s->win)));
}

/* 入口: 不走 GtkApplication —— 它依赖 D-Bus/session 总线, 在 app 沙箱里没有,
 * 实测会在 gdk seat 断言后 SIGSEGV。这里用最朴素的 gtk_init + 主循环,
 * 与 BetterSH 原 gtkprobe 同形态。 */
/* 冒烟程序自己把 GTK 需要的两套运行时数据找好, 免得靠调用者的环境:
 *   XKB_CONFIG_ROOT  —— xkb-data 的 xkb 目录(不是它的上一级)
 *   FONTCONFIG_FILE  —— 会话生成的 fonts.conf
 * 依次尝试 BSH_SYSROOT(<资源根>), $HOME 下的解包位置。 */
static void setup_runtime_env(void)
{
    const char* sysroot = g_getenv("BSH_SYSROOT");
    if (sysroot == NULL || *sysroot == '\0') sysroot = g_get_home_dir();

    if (g_getenv("XKB_CONFIG_ROOT") == NULL) {
        char* cands[2];
        cands[0] = g_build_filename(sysroot, "usr/share/X11/xkb", NULL);
        cands[1] = g_build_filename(g_get_home_dir(), "usr/share/X11/xkb", NULL);
        for (int i = 0; i < 2; i++) {
            if (g_file_test(cands[i], G_FILE_TEST_IS_DIR)) {
                g_setenv("XKB_CONFIG_ROOT", cands[i], TRUE);
                g_print("gtksmoke: XKB_CONFIG_ROOT=%s\n", cands[i]);
                break;
            }
        }
        g_free(cands[0]); g_free(cands[1]);
    }
    if (g_getenv("FONTCONFIG_FILE") == NULL) {
        char* conf = g_build_filename(sysroot, "fonts.conf", NULL);
        if (g_file_test(conf, G_FILE_TEST_IS_REGULAR)) {
            g_setenv("FONTCONFIG_FILE", conf, TRUE);
            g_setenv("FONTCONFIG_PATH", sysroot, TRUE);
            g_print("gtksmoke: FONTCONFIG_FILE=%s\n", conf);
        }
        g_free(conf);
    }
}

int smoke_main(void)
{
    g_setenv("GDK_BACKEND", "wayland", TRUE);
    setup_runtime_env();
    if (!gtk_init_check(NULL, NULL)) {
        g_printerr("gtksmoke: gtk_init 失败 (WAYLAND_DISPLAY=%s XDG_RUNTIME_DIR=%s)\n",
                   g_getenv("WAYLAND_DISPLAY") ? g_getenv("WAYLAND_DISPLAY") : "(未设)",
                   g_getenv("XDG_RUNTIME_DIR") ? g_getenv("XDG_RUNTIME_DIR") : "(未设)");
        return 1;
    }
    g_print("gtksmoke: gtk_init OK —— GTK %u.%u.%u, GDK 后端=%s\n",
            gtk_get_major_version(), gtk_get_minor_version(), gtk_get_micro_version(),
            G_OBJECT_TYPE_NAME(gdk_display_get_default()));
    build_ui();
    gtk_main();
    g_print("gtksmoke: 退出\n");
    return 0;
}

int main(int argc, char** argv)
{
    (void)argc; (void)argv;
    return smoke_main();
}
