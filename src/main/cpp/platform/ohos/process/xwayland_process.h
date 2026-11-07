#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct hgs_xwayland_process {
    int32_t pid;
    int control_fd;
};

bool hgs_spawn_xwayland(const char *display, const char *runtime_dir, const char *xkb_root,
                       const char *authority_path, int listen_fd, int wayland_fd,
                       int xwm_fd, int ready_fd, struct hgs_xwayland_process *process);
void hgs_stop_xwayland(struct hgs_xwayland_process *process);

#ifdef __cplusplus
}
#endif
