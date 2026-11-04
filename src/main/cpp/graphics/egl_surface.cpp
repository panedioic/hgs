// hgs/src/main/cpp/graphics/egl_surface.cpp
// — Native 侧 Surface 生命周期 + 原生 renderer/输入绑定 (阶段 1 N1 / W2b)
//
// 项目 SDK 没有 OH_NativeXComponent_GetXComponentSurfaceId: native 路径只能
// 用 OnSurfaceCreated 的 OHNativeWindow* 直接建渲染器。生命周期:
//   OnSurfaceCreated  保存 window → 认领 pending id → 建渲染器 + 输入绑定
//   bindXComponent(id) 记录"待绑定 id" → 若 surface 已存在则当场认领
//   OnSurfaceChanged  门面 ResizeSurface (SetSize + ForceRedraw + surface 事件)
//   OnSurfaceDestroyed 解绑输入/窗口 → DetachSurface (只释放 surface)
// ArkTS controller 路径不经本文件 (走 attachSurface → PluginManager)。
//
// ⚠ bindXComponent 的参数是 toplevelId 而非 XComponent 对象: 本 SDK 上 onLoad
// 的事件对象 napi_unwrap 拿不到 OH_NativeXComponent (实测 unwrap failed),
// 而 surface 回调天然携带 comp 指针 — 所以改为"页面传 id、回调认领"。
// 单页面单 XComponent (HgsWindowPage) 场景下无歧义; 同屏多 XComponent 同时
// 创建时按"未绑定的最新 surface 先认领"近似处理。

#include "egl_surface.h"

#include <mutex>
#include <unordered_map>

#include <native_window/external_window.h>

#include "bridge/plugin_manager.h"
#include "hgs_instance.h"
#include "input/native_input.h"

// ⚠ 必须定义 LOG_TAG: SDK hilog/log.h 对未定义者默认 NULL, OH_LOG_Print 遇
// NULL tag 静默丢日志 (本文件所有日志曾因此不可见, 排查半天)
#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_DOMAIN 0x0000
#define LOG_TAG "HGS_NAPI"

namespace hgs {
namespace surface {

namespace {

std::mutex gMutex;
// 以 OH_NativeXComponent* 为键的原生绑定表 (每个 XComponent 实例一份)
std::unordered_map<OH_NativeXComponent *, OHNativeWindow *> gWindows;
std::unordered_map<OH_NativeXComponent *, uint32_t> gToplevelIds;
// bindXComponent(id) 记下的待绑定 toplevelId; 由下一个未绑定的 surface 认领
uint32_t gPendingBindId = 0;

// comp 绑定 toplevel: 写表 + 原生输入绑定 (调用方持 gMutex 外)
bool BindComp(OH_NativeXComponent *comp, uint32_t tl) {
    if (comp == nullptr || tl == 0) return false;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        auto tit = gToplevelIds.find(comp);
        if (tit != gToplevelIds.end() && tit->second == tl) return true;  // 已绑定
        gToplevelIds[comp] = tl;
    }
    hgs::input::Bind(comp, tl);
    OH_LOG_INFO(LOG_APP, "[HGS-NAPI] comp bound tl=%{public}u", tl);
    return true;
}

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

// 认领 pending id: gWindows 中找一个尚未绑定 toplevel 的 comp 绑上 (幂等)。
void ClaimPendingBind() {
    uint32_t pending = 0;
    OH_NativeXComponent *target = nullptr;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        pending = gPendingBindId;
        if (pending == 0) return;
        for (auto &kv : gWindows) {
            if (gToplevelIds.find(kv.first) == gToplevelIds.end()) {
                target = kv.first;
                break;
            }
        }
    }
    if (target != nullptr && BindComp(target, pending)) {
        std::lock_guard<std::mutex> lock(gMutex);
        gPendingBindId = 0;
        OH_LOG_INFO(LOG_APP, "[HGS-NAPI] pending id=%{public}u claimed", pending);
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
    // 认领 pending id (bindXComponent 先到) → 绑输入 + 建渲染器
    ClaimPendingBind();
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
    } else {
        // onLoad 的绑定可能晚于首个 changed, 再试一次认领
        ClaimPendingBind();
        TryAttachNativeRenderer(comp);
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

// NAPI: bindXComponent(toplevelId) — 页面 onLoad 传入本窗口的 toplevelId;
// 已存在的未绑定 surface 当场认领 (绑输入 + 建渲染器), 否则留给下一个
// OnSurfaceCreated。
napi_value BindXComponent(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 1) {
        OH_LOG_ERROR(LOG_APP, "[HGS-NAPI] bindXComponent: 需要 toplevelId");
        return nullptr;
    }
    uint32_t toplevelId = 0;
    napi_get_value_uint32(env, argv[0], &toplevelId);
    if (toplevelId == 0) {
        OH_LOG_WARN(LOG_APP, "[HGS-NAPI] bindXComponent: toplevelId=0 忽略");
        return nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(gMutex);
        gPendingBindId = toplevelId;
    }
    OH_LOG_INFO(LOG_APP, "[HGS-NAPI] bindXComponent pending tl=%{public}u", toplevelId);
    ClaimPendingBind();
    // 认领成功时 ClaimPendingBind 内部已绑输入; 这里补建渲染器。
    // ⚠ TryAttachNativeRenderer 内部取 gMutex — 必须在锁外调用 (先前持锁调用
    //   导致 UI 线程死锁, 渲染器永远挂不上)
    OH_NativeXComponent *target = nullptr;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        for (auto &kv : gToplevelIds) {
            if (kv.second == toplevelId) {
                target = kv.first;
                break;
            }
        }
    }
    if (target != nullptr) {
        TryAttachNativeRenderer(target);
    }
    return nullptr;
}

} // namespace surface
} // namespace hgs
