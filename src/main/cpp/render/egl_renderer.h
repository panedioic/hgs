#pragma once

#include <EGL/egl.h>
#include <atomic>
#include <cstdint>
#include <native_window/external_window.h>
#include <thread>

namespace hgs {

// 空 EGL 渲染器 — hgs 原生渲染能力的冒烟骨架。
// surface 生命周期由 NativeXComponent 回调驱动 (见 napi_init.cpp):
// OnSurfaceCreated 拿到 OHNativeWindow 后 Create, OnSurfaceDestroyed 析构。
// 以固定颜色循环 clear+swap, 验证 "HAR 内原生模块 → EGL → OHNativeWindow
// → 屏幕" 通路。结构对齐 entry EglRenderer (进程级共享 display + 独立
// 渲染线程), 后续 GL 部分迁移时以此文件为落点扩展。
class EmptyEglRenderer {
public:
    // 持有 window 的一个引用 (NativeObjectReference), 析构时释放。
    // 失败返回 nullptr (display/config/context 或窗口 surface 创建失败)
    static EmptyEglRenderer *Create(OHNativeWindow *window);

    ~EmptyEglRenderer();

    EmptyEglRenderer(const EmptyEglRenderer &) = delete;
    EmptyEglRenderer &operator=(const EmptyEglRenderer &) = delete;

private:
    EmptyEglRenderer() = default;

    bool Init(OHNativeWindow *window);
    void RenderLoop();

    std::thread thread_;
    std::atomic<bool> running_{false};
    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLContext context_ = EGL_NO_CONTEXT;
    EGLSurface surface_ = EGL_NO_SURFACE;
    OHNativeWindow *window_ = nullptr;
};

} // namespace hgs
