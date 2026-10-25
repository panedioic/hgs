// hgs 原生入口 — NativeXComponent 模式 (参照 blender-harmonyos):
// XComponent(libraryname:'hgs') 触发模块 Init 时 exports 携带
// OH_NATIVE_XCOMPONENT_OBJ, 在此注册 surface 回调; 生命周期由
// OnSurfaceCreated/Changed/Destroyed 在原生层驱动, 不再暴露
// create/destroy 导出。
#include "napi/native_api.h"
#include "render/egl_renderer.h"

#include <ace/xcomponent/native_interface_xcomponent.h>
#include <cstdint>
#include <hilog/log.h>
#include <mutex>
#include <unordered_map>

namespace {

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
    // 对齐 blender 的 ohos_configure_native_window: 把 ArkUI 给的尺寸
    // 写进 buffer geometry, 避免 eglQuery 尺寸与 surface 不一致
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

// NAPI: bindXComponent(xcompObject) — 非 Init 触发的 XComponent (同进程
// 第二个窗口: 模块已加载, Init 不会再次执行) 在 onLoad 里显式补挂回调。
// 参照 blender 的 bindSubXComponent。
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

EXTERN_C_START
static napi_value Init(napi_env env, napi_value exports) {
    const napi_property_descriptor desc[] = {
        {"bindXComponent", nullptr, BindXComponent, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);

    // XComponent(libraryname:'hgs') 触发的加载: exports 携带原生对象
    napi_value xc;
    if (napi_get_named_property(env, exports, OH_NATIVE_XCOMPONENT_OBJ, &xc) == napi_ok) {
        OH_NativeXComponent *nx = nullptr;
        if (napi_unwrap(env, xc, reinterpret_cast<void **>(&nx)) == napi_ok && nx != nullptr) {
            OH_NativeXComponent_RegisterCallback(nx, &gXcompCb);
            char idBuf[128] = {0};
            uint64_t idLen = sizeof(idBuf);
            OH_NativeXComponent_GetXComponentId(nx, idBuf, &idLen);
            OH_LOG_INFO(LOG_APP, "[HGS-NAPI] XComponent callbacks registered id=%{public}s", idBuf);
        }
    }
    return exports;
}
EXTERN_C_END

static napi_module hgsModule = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,
    .nm_modname = "hgs",
    .nm_priv = nullptr,
    .reserved = {0},
};

extern "C" __attribute__((constructor)) void RegisterHgsModule(void) {
    napi_module_register(&hgsModule);
}

} // namespace
