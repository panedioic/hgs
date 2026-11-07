#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <pixman.h>

struct hgs_cpu_surface {
    struct NativeWindow *window;
    uint64_t id;
};

bool hgs_cpu_surface_open(struct hgs_cpu_surface *surface, uint64_t id);
void hgs_cpu_surface_close(struct hgs_cpu_surface *surface);
bool hgs_cpu_surface_present(struct hgs_cpu_surface *surface, pixman_image_t *image);
