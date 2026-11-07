#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct hgs_x11_session;

enum hgs_x11_event_kind {
    HGS_X11_MAPPED,
    HGS_X11_UPDATED,
    HGS_X11_UNMAPPED,
    HGS_X11_CLOSE_REQUESTED
};

struct hgs_x11_window {
    uint32_t xid, pid;
    uint64_t client_id, generation;
    const char *title, *app_id;
    int32_t x, y, width, height;
    bool popup, modal, minimized, maximized, fullscreen;
};

typedef void (*hgs_x11_observer)(void *context, enum hgs_x11_event_kind kind,
                                 const struct hgs_x11_window *window);

struct hgs_x11_settings {
    const char *xkb_root, *session_dir, *authority_path;
    int x_socket, display_number, wake_fd;
    int width, height, refresh_millihz;
    hgs_x11_observer observer;
    void *context;
};

struct hgs_x11_session *hgs_x11_create(const struct hgs_x11_settings *settings, const char **stage);
int hgs_x11_dispatch(struct hgs_x11_session *session, int timeout_ms);
bool hgs_x11_ready(const struct hgs_x11_session *session);
const char *hgs_x11_diagnostic(const struct hgs_x11_session *session);
bool hgs_x11_child_lost(const struct hgs_x11_session *session);
void hgs_x11_destroy(struct hgs_x11_session *session);
void hgs_x11_key(struct hgs_x11_session *session, uint32_t xid, uint64_t generation,
                 uint32_t code, bool pressed);
void hgs_x11_pointer(struct hgs_x11_session *session, uint32_t xid, uint64_t generation,
                     double x, double y, uint32_t button, int action);
void hgs_x11_scroll(struct hgs_x11_session *session, uint32_t xid, uint64_t generation,
                    double x, double y, int axis, double value);

#ifdef __cplusplus
}
#endif
