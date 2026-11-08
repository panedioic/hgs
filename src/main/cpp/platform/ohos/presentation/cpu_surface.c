#include "cpu_surface.h"

#include <native_buffer/native_buffer.h>
#include <native_window/external_window.h>
#include <poll.h>
#include <unistd.h>

bool hgs_cpu_surface_reset(struct hgs_cpu_surface *surface) {
    OHNativeWindow *window = surface->window;
    uint64_t usage = 0;
    return window &&
        OH_NativeWindow_NativeWindowHandleOpt(window, GET_USAGE, &usage) == 0 &&
        OH_NativeWindow_NativeWindowHandleOpt(window, SET_USAGE, usage | NATIVEBUFFER_USAGE_CPU_WRITE) == 0 &&
        OH_NativeWindow_NativeWindowHandleOpt(window, SET_FORMAT, NATIVEBUFFER_PIXEL_FMT_RGBA_8888) == 0 &&
        OH_NativeWindow_NativeWindowHandleOpt(window, SET_TIMEOUT, 0) == 0 &&
        OH_NativeWindow_NativeWindowSetScalingModeV2(window, OH_SCALING_MODE_SCALE_TO_WINDOW_V2) == 0;
}

bool hgs_cpu_surface_open(struct hgs_cpu_surface *surface, uint64_t id) {
    struct hgs_cpu_surface output = {.id = id};
    if (OH_NativeWindow_CreateNativeWindowFromSurfaceId(id, &output.window) != 0 || !output.window)
        return false;
    if (!hgs_cpu_surface_reset(&output)) {
        hgs_cpu_surface_close(&output);
        return false;
    }
    hgs_cpu_surface_close(surface);
    *surface = output;
    return true;
}

void hgs_cpu_surface_close(struct hgs_cpu_surface *surface) {
    if (surface->window) OH_NativeWindow_DestroyNativeWindow(surface->window);
    *surface = (struct hgs_cpu_surface){0};
}

static bool copy_image(OHNativeWindowBuffer *buffer, pixman_image_t *image, int width, int height) {
    OH_NativeBuffer *native = NULL;
    if (OH_NativeBuffer_FromNativeWindowBuffer(buffer, &native) != 0 || !native) return false;
    OH_NativeBuffer_Config config = {0};
    OH_NativeBuffer_GetConfig(native, &config);
    if (config.width != width || config.height != height || config.stride < width * 4 ||
        config.format != NATIVEBUFFER_PIXEL_FMT_RGBA_8888) return false;
    void *data = NULL;
    if (OH_NativeBuffer_Map(native, &data) != 0) return false;
    pixman_image_t *destination = data ? pixman_image_create_bits_no_clear(
        PIXMAN_a8b8g8r8, width, height, data, config.stride) : NULL;
    const bool copied = destination != NULL;
    if (copied) {
        pixman_image_composite32(PIXMAN_OP_SRC, image, NULL, destination,
                                 0, 0, 0, 0, 0, 0, width, height);
        pixman_image_unref(destination);
    }
    return OH_NativeBuffer_Unmap(native) == 0 && copied;
}

bool hgs_cpu_surface_present(struct hgs_cpu_surface *surface, pixman_image_t *image) {
    const int width = pixman_image_get_width(image), height = pixman_image_get_height(image);
    if (width <= 0 || height <= 0 || (uint64_t)width * height > 4096u * 4096u) return false;
    OHNativeWindow *window = surface->window;
    if (OH_NativeWindow_NativeWindowHandleOpt(window, SET_BUFFER_GEOMETRY, width, height) != 0) return false;

    OHNativeWindowBuffer *buffer = NULL;
    int fence = -1;
    const int result = OH_NativeWindow_NativeWindowRequestBuffer(window, &buffer, &fence);
    struct pollfd release = {.fd = fence, .events = POLLIN};
    // Never block the Xwayland event loop on a slow Surface consumer.
    const bool ready = fence < 0 || (poll(&release, 1, 0) == 1 && (release.revents & POLLIN));
    if (fence >= 0) close(fence);
    if (result != 0 || !buffer) return false;
    if (ready && copy_image(buffer, image, width, height) &&
        OH_NativeWindow_NativeWindowFlushBuffer(window, buffer, -1, (Region){0}) == 0) return true;
    OH_NativeWindow_NativeWindowAbortBuffer(window, buffer);
    return false;
}
