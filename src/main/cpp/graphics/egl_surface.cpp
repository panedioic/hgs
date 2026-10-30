// hgs/src/main/cpp/graphics/egl_surface.cpp
// — Native 侧 Surface 生命周期函数实现 (占位, 见头文件说明)

#include "egl_surface.h"

#include <native_window/external_window.h>

namespace hgs {
namespace surface {

std::mutex gMutex;
// 以 OH_NativeXComponent* 为键的渲染器注册表 (每个 XComponent 实例一个)
std::unordered_map<OH_NativeXComponent *, hgs::EmptyEglRenderer *> gRenderers;

void OnSurfaceCreated(OH_NativeXComponent *comp, void *window) {
    uint64_t w = 0, h = 0;
    OH_NativeXComponent_GetXComponentSize(comp, window, &w, &h);
    auto *nw = static_cast<OHNativeWindow *>(window);
    if (nw == nullptr) {
        OH_LOG_ERROR(LOG_APP, "[HGS-NAPI] OnSurfaceCreated: null window");
        return;
    }
    // 把 ArkUI 给的尺寸写进 buffer geometry, 避免 eglQuery 尺寸与 surface 不一致
    int32_t rc = OH_NativeWindow_NativeWindowHandleOpt(
        nw, SET_BUFFER_GEOMETRY, static_cast<int32_t>(w), static_cast<int32_t>(h));
    if (rc != 0) {
        OH_LOG_WARN(LOG_APP, "[HGS-NAPI] SET_BUFFER_GEOMETRY failed ret=%{public}d", rc);
    }

    hgs::EmptyEglRenderer *r = hgs::EmptyEglRenderer::Create(nw);
    if (r == nullptr) {
        OH_LOG_ERROR(LOG_APP, "[HGS-NAPI] EmptyEglRenderer::Create failed");
        return;
    }
    std::lock_guard<std::mutex> lock(gMutex);
    gRenderers[comp] = r;
    OH_LOG_INFO(LOG_APP, "[HGS-NAPI] OnSurfaceCreated %{public}ux%{public}u renderer=%{public}p",
                static_cast<unsigned>(w), static_cast<unsigned>(h), static_cast<void *>(r));
}

void OnSurfaceChanged(OH_NativeXComponent *comp, void *window) {
    uint64_t w = 0, h = 0;
    OH_NativeXComponent_GetXComponentSize(comp, window, &w, &h);
    auto *nw = static_cast<OHNativeWindow *>(window);
    if (nw == nullptr) {
        return;
    }
    OH_NativeWindow_NativeWindowHandleOpt(
        nw, SET_BUFFER_GEOMETRY, static_cast<int32_t>(w), static_cast<int32_t>(h));
    OH_LOG_INFO(LOG_APP, "[HGS-NAPI] OnSurfaceChanged %{public}ux%{public}u",
                static_cast<unsigned>(w), static_cast<unsigned>(h));
}

void OnSurfaceDestroyed(OH_NativeXComponent *comp, void * /*window*/) {
    std::lock_guard<std::mutex> lock(gMutex);
    auto it = gRenderers.find(comp);
    if (it != gRenderers.end()) {
        delete it->second;
        gRenderers.erase(it);
        OH_LOG_INFO(LOG_APP, "[HGS-NAPI] OnSurfaceDestroyed renderer released");
    }
}

OH_NativeXComponent_Callback gXcompCb = {
    OnSurfaceCreated, OnSurfaceChanged, OnSurfaceDestroyed, nullptr,
};

napi_value BindXComponent(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 1) {
        return nullptr;
    }
    OH_NativeXComponent *nx = nullptr;
    if (napi_unwrap(env, argv[0], reinterpret_cast<void **>(&nx)) != napi_ok || nx == nullptr) {
        OH_LOG_ERROR(LOG_APP, "[HGS-NAPI] bindXComponent: napi_unwrap failed");
        return nullptr;
    }
    OH_NativeXComponent_RegisterCallback(nx, &gXcompCb);
    OH_LOG_INFO(LOG_APP, "[HGS-NAPI] bindXComponent wired");
    return nullptr;
}

} // namespace surface
} // namespace hgs
