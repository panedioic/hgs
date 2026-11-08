#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct hgs_x11_session;
struct NativeWindow;

enum hgs_x11_external_result {
    HGS_X11_EXTERNAL_IDLE,
    HGS_X11_EXTERNAL_OWNED,
    HGS_X11_EXTERNAL_PRESENTED,
    HGS_X11_EXTERNAL_RELEASED
};
enum hgs_x11_control { HGS_X11_FOCUS, HGS_X11_BLUR, HGS_X11_RESIZE, HGS_X11_CLOSE };

enum hgs_x11_event_kind {
    HGS_X11_MAPPED,
    HGS_X11_UPDATED,
    HGS_X11_UNMAPPED,
    HGS_X11_CLOSE_REQUESTED
};

struct hgs_x11_window {
    uint32_t xid, pid;
    uint64_t client_id, generation;
    uint64_t surface_generation;
    uint32_t parent_xid;
    uint64_t parent_client_id, parent_generation;
    const char *title, *app_id;
    int32_t x, y, width, height;
    int32_t min_width, min_height, max_width, max_height;
    bool popup, modal, minimized, maximized, fullscreen;
    bool text_input_active;
    bool frame_presented;
};

typedef void (*hgs_x11_observer)(void *context, enum hgs_x11_event_kind kind,
                                 const struct hgs_x11_window *window);

struct hgs_x11_settings {
    const char *xkb_root, *session_dir, *authority_path;
    const char *ime_server_name;
    int x_socket, display_number, wake_fd;
    int width, height, refresh_millihz;
    hgs_x11_observer observer;
    enum hgs_x11_external_result (*present_external)(void *context, uint32_t xid, uint32_t pid,
                                                     uint64_t generation, struct NativeWindow *output);
    void (*close_external)(void *context, uint64_t generation, bool unmapped);
    void *external_context;
    void *context;
};

struct hgs_x11_session *hgs_x11_create(const struct hgs_x11_settings *settings, const char **stage);
int hgs_x11_dispatch(struct hgs_x11_session *session, int timeout_ms);
bool hgs_x11_ready(const struct hgs_x11_session *session);
const char *hgs_x11_diagnostic(const struct hgs_x11_session *session);
bool hgs_x11_child_lost(const struct hgs_x11_session *session);
bool hgs_x11_xim_failed(const struct hgs_x11_session *session);
void hgs_x11_destroy(struct hgs_x11_session *session);
void hgs_x11_bind_surface(struct hgs_x11_session *session, uint32_t xid, uint64_t generation,
                          uint64_t surface_id, bool attach);
void hgs_x11_commit_text(struct hgs_x11_session *session, uint32_t xid, uint64_t generation,
                         const char *text, size_t length);
void hgs_x11_control_window(struct hgs_x11_session *session, uint32_t xid, uint64_t generation,
                            enum hgs_x11_control action, int32_t width, int32_t height);
void hgs_x11_key(struct hgs_x11_session *session, uint32_t xid, uint64_t generation,
                 uint32_t code, bool pressed);
void hgs_x11_pointer(struct hgs_x11_session *session, uint32_t xid, uint64_t generation,
                     double x, double y, uint32_t button, int action, bool normalized);
void hgs_x11_scroll(struct hgs_x11_session *session, uint32_t xid, uint64_t generation,
                    double x, double y, int axis, double value, bool normalized);

#ifdef __cplusplus
}
#endif
