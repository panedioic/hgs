// hgs/src/main/cpp/graphics/egl_surface.cpp
// — Native 侧 Surface 生命周期 + 原生 renderer/输入绑定 (阶段 1 N1)
//
// 项目 SDK 没有 OH_NativeXComponent_GetXComponentSurfaceId: native 路径只能
// 用 OnSurfaceCreated 的 OHNativeWindow* 直接建渲染器。生命周期:
//   OnSurfaceCreated  保存 window → 若已 bind 则 CreateRendererFromWindow
//   bindXComponent    注册 surface 回调 + 保存 toplevelId → 补建 renderer
//   OnSurfaceChanged  门面 ResizeSurface (SetSize + ForceRedraw + surface 事件)
//   OnSurfaceDestroyed 解绑输入/窗口 → DetachSurface (只释放 surface)
// ArkTS controller 路径不经本文件 (走 attachSurface → PluginManager)。

#include "egl_surface.h"

#include <mutex>
#include <unordered_map>

#include <native_window/external_window.h>

#include "bridge/plugin_manager.h"
#include "hgs_instance.h"
#include "input/native_input.h"

namespace hgs {
namespace surface {

namespace {

std::mutex gMutex;
// 以 OH_NativeXComponent* 为键的原生绑定表 (每个 XComponent 实例一份)
std::unordered_map<OH_NativeXComponent *, OHNativeWindow *> gWindows;
std::unordered_map<OH_NativeXComponent *, uint32_t> gToplevelIds;

// 有 window + toplevelId 时建真实渲染器 (幂等; 已建则跳过)。
void TryAttachNativeRenderer(OH_NativeXComponent *comp) {
    OHNativeWindow *nw = nullptr;
    uint32_t tl = 0;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        auto wit = gWindows.find(comp);
        auto tit = gToplevelIds.find(comp);
        if (wit == gWindows.end() || tit == gToplevelIds.end() || tit->second == 0) return;
        nw = wit->second;
        tl = tit->second;
    }
    // OnSurfaceCreated 与 bindXComponent 两个入口都可能触发, 已有 renderer
    // 时直接返回 (CreateRendererFromWindow 会先 shutdown 旧的, 重复调用会
    // 白重建一次 EGL context)
    if (PluginManager::GetInstance()->GetRendererForToplevel(tl) != nullptr) return;
    if (PluginManager::GetInstance()->CreateRendererFromWindow(tl, nw)) {
        OH_LOG_INFO(LOG_APP, "[HGS-NAPI] native renderer attached tl=%{public}u", tl);
    } else {
        OH_LOG_ERROR(LOG_APP, "[HGS-NAPI] native renderer attach FAILED tl=%{public}u", tl);
    }
}

} // namespace

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
    {
        std::lock_guard<std::mutex> lock(gMutex);
        gWindows[comp] = nw;
    }
    // 若 bindXComponent 先到 (onLoad 早于 surface created), 此处补建
    TryAttachNativeRenderer(comp);
    OH_LOG_INFO(LOG_APP, "[HGS-NAPI] OnSurfaceCreated %{public}ux%{public}u",
                static_cast<unsigned>(w), static_cast<unsigned>(h));
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
    uint32_t tl = 0;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        auto tit = gToplevelIds.find(comp);
        if (tit != gToplevelIds.end()) tl = tit->second;
    }
    if (tl != 0) {
        // 门面转发 = 旧 resizeRenderer 语义 (SetSize + ForceRedraw + surface 事件)
        HgsInstance::Instance().ResizeSurface(tl, static_cast<int32_t>(w),
                                             static_cast<int32_t>(h), 0);
    }
    OH_LOG_INFO(LOG_APP, "[HGS-NAPI] OnSurfaceChanged %{public}ux%{public}u",
                static_cast<unsigned>(w), static_cast<unsigned>(h));
}

void OnSurfaceDestroyed(OH_NativeXComponent *comp, void * /*window*/) {
    hgs::input::Unbind(comp);  // 清原生输入绑定 (surface 即将销毁)
    uint32_t toplevelId = 0;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        auto tit = gToplevelIds.find(comp);
        if (tit != gToplevelIds.end()) {
            toplevelId = tit->second;
            gToplevelIds.erase(tit);
        }
        gWindows.erase(comp);
    }
    if (toplevelId != 0) {
        HgsInstance::Instance().DetachSurface(toplevelId, 0);
    }
    OH_LOG_INFO(LOG_APP, "[HGS-NAPI] OnSurfaceDestroyed tl=%{public}u", toplevelId);
}

OH_NativeXComponent_Callback gXcompCb = {
    OnSurfaceCreated, OnSurfaceChanged, OnSurfaceDestroyed,
    hgs::input::DispatchTouchEvent,  // 触摸回调 (backend=Native 时注入)
};

napi_value BindXComponent(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2];
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
    // 可选第二参数 toplevelId → 原生输入绑定 + renderer 挂载。
    // 缺省 0 = 仅注册 surface 回调 (兼容旧调用), 不建渲染器。
    uint32_t toplevelId = 0;
    if (argc >= 2) {
        napi_get_value_uint32(env, argv[1], &toplevelId);
    }
    if (toplevelId != 0) {
        {
            std::lock_guard<std::mutex> lock(gMutex);
            gToplevelIds[nx] = toplevelId;
        }
        hgs::input::Bind(nx, toplevelId);
        TryAttachNativeRenderer(nx);
    }
    OH_LOG_INFO(LOG_APP, "[HGS-NAPI] bindXComponent wired tl=%{public}u", toplevelId);
    return nullptr;
}

} // namespace surface
} // namespace hgs
