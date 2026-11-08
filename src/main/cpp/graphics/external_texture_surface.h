#pragma once

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <atomic>

struct NativeWindow;
struct OH_NativeImage;

namespace hgs::service {
class ExternalTextureSurface {
public:
    enum class DrawResult { Idle, Presented, Retry };

    ~ExternalTextureSurface();
    bool Open(NativeWindow* output, int width, int height);
    NativeWindow* Producer() const { return producer_; }
    DrawResult Draw(bool flipY);

private:
    bool MakeCurrent() const;
    static void OnFrame(void* context);

    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLContext context_ = EGL_NO_CONTEXT;
    EGLSurface surface_ = EGL_NO_SURFACE;
    OH_NativeImage* image_ = nullptr;
    NativeWindow* producer_ = nullptr;
    GLuint program_ = 0, texture_ = 0, vertices_ = 0;
    GLint transformLocation_ = -1;
    std::atomic<bool> available_{false};
    bool listenerSet_ = false, needsDraw_ = false, hasImage_ = false;
    int drawnWidth_ = 0, drawnHeight_ = 0;
    float transform_[16]{};
};
}
