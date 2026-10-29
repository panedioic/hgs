#include "egl_renderer.h"

#include <GLES3/gl3.h>
#include <chrono>
#include <hilog/log.h>

#undef LOG_TAG
#define LOG_TAG "HGS_EGL"
#include <native_window/external_window.h>
#include <new>
#include <thread>

namespace {

// 进程级共享 display — 对齐 entry EglRenderer::GetSharedDisplay 的约定:
// 同进程所有渲染器共用一次 eglInitialize, display 随进程生命周期持有。
EGLDisplay GetSharedDisplay() {
    static EGLDisplay shared = [] {
        EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (d == EGL_NO_DISPLAY) {
            OH_LOG_ERROR(LOG_APP, "[HGS-EGL] eglGetDisplay failed");
            return EGL_NO_DISPLAY;
        }
        if (!eglInitialize(d, nullptr, nullptr)) {
            OH_LOG_ERROR(LOG_APP, "[HGS-EGL] eglInitialize failed");
            return EGL_NO_DISPLAY;
        }
        OH_LOG_INFO(LOG_APP, "[HGS-EGL] EGL initialized");
        return d;
    }();
    return shared;
}

} // namespace

namespace hgs {

EmptyEglRenderer *EmptyEglRenderer::Create(OHNativeWindow *window) {
    auto *r = new (std::nothrow) EmptyEglRenderer();
    if (r == nullptr || !r->Init(window)) {
        delete r;
        return nullptr;
    }
    return r;
}

bool EmptyEglRenderer::Init(OHNativeWindow *window) {
    display_ = GetSharedDisplay();
    if (display_ == EGL_NO_DISPLAY) {
        return false;
    }

    const EGLint configAttribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_NONE};
    EGLConfig config = nullptr;
    EGLint numConfigs = 0;
    if (!eglChooseConfig(display_, configAttribs, &config, 1, &numConfigs) || numConfigs < 1) {
        OH_LOG_ERROR(LOG_APP, "[HGS-EGL] eglChooseConfig failed");
        return false;
    }

    const EGLint contextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    context_ = eglCreateContext(display_, config, EGL_NO_CONTEXT, contextAttribs);
    if (context_ == EGL_NO_CONTEXT) {
        OH_LOG_ERROR(LOG_APP, "[HGS-EGL] eglCreateContext failed");
        return false;
    }

    // window 由 NativeXComponent OnSurfaceCreated 交付, 此处持有一个引用
    // 保证渲染线程期间有效; 析构时 Unreference 归还框架。
    if (window == nullptr) {
        OH_LOG_ERROR(LOG_APP, "[HGS-EGL] null window");
        return false;
    }
    window_ = window;
    OH_NativeWindow_NativeObjectReference(window_);

    surface_ = eglCreateWindowSurface(display_, config, (EGLNativeWindowType)window_, nullptr);
    if (surface_ == EGL_NO_SURFACE) {
        OH_LOG_ERROR(LOG_APP, "[HGS-EGL] eglCreateWindowSurface failed");
        return false;
    }

    running_ = true;
    thread_ = std::thread(&EmptyEglRenderer::RenderLoop, this);
    OH_LOG_INFO(LOG_APP, "[HGS-EGL] renderer started window=%{public}p", static_cast<void *>(window_));
    return true;
}

void EmptyEglRenderer::RenderLoop() {
    if (!eglMakeCurrent(display_, surface_, surface_, context_)) {
        OH_LOG_ERROR(LOG_APP, "[HGS-EGL] eglMakeCurrent failed");
        running_.store(false, std::memory_order_release);
        return;
    }
    eglSwapInterval(display_, 1);
    while (running_.load(std::memory_order_acquire)) {
        glClearColor(0.09f, 0.15f, 0.26f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        if (!eglSwapBuffers(display_, surface_)) {
            // surface 销毁 (窗口关闭) 时 swap 失败, 正常退出循环
            OH_LOG_INFO(LOG_APP, "[HGS-EGL] eglSwapBuffers failed, render loop exit");
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

EmptyEglRenderer::~EmptyEglRenderer() {
    running_.store(false, std::memory_order_release);
    if (thread_.joinable()) {
        thread_.join();
    }
    if (surface_ != EGL_NO_SURFACE) {
        eglDestroySurface(display_, surface_);
    }
    if (context_ != EGL_NO_CONTEXT) {
        eglDestroyContext(display_, context_);
    }
    if (window_ != nullptr) {
        OH_NativeWindow_NativeObjectUnreference(window_);
        window_ = nullptr;
    }
    // 共享 display 不在此销毁 (进程级持有, 对齐 entry 约定)
    OH_LOG_INFO(LOG_APP, "[HGS-EGL] renderer destroyed");
}

} // namespace hgs
