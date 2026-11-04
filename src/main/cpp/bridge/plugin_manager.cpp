#include "plugin_manager.h"
#include "compositor/wayland_server.h"
#include "hgs_instance.h"  // P1.1: surface NAPI 经门面 (generation 表)
#include <native_window/external_window.h>
#include <napi/native_api.h>
#include <algorithm>

#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_Plugin"
#include <hilog/log.h>

PluginManager* PluginManager::GetInstance() {
    static PluginManager s;
    return &s;
}

uint32_t PluginManager::DequeuePendingToplevel() {
    if (pendingToplevelQueue_.empty()) return 0;
    uint32_t id = pendingToplevelQueue_.front();
    pendingToplevelQueue_.pop_front();
    return id;
}

// 清除队列中指定 id 的残坑: 窗口在 loadContent 完成前被销毁 (destroy-while-
// creating, aboutToAppear 未执行 → id 无人消费), 不删会让后续每个页面出队
// 拿到这个死 id → 渲染器挂错 toplevel 取不到帧 → 黑屏 (1.8 实测)。id 未在
// 队列 (页面已出队) 时是 no-op, 无条件调用安全。
void PluginManager::CancelPendingToplevel(uint32_t id) {
    const size_t before = pendingToplevelQueue_.size();
    pendingToplevelQueue_.erase(
        std::remove(pendingToplevelQueue_.begin(), pendingToplevelQueue_.end(), id),
        pendingToplevelQueue_.end());
    if (pendingToplevelQueue_.size() != before) {
        OH_LOG_WARN(LOG_APP, "[MW-Life] CancelPendingToplevel id=%{public}u (%{public}zu→%{public}zu)",
                    id, before, pendingToplevelQueue_.size());
    }
}

// 返回值 (阶段 0 P0.3): true = native window 创建 + EglRenderer::Init 成功。
// 旧调用方 (hgs::surface::CreateRendererNAPI) 忽略返回值, 行为不变;
// HgsInstance::AttachSurface 依据返回值分配 generation。
bool PluginManager::CreateRenderer(uint32_t toplevelId, int64_t surfaceId) {
    OH_LOG_INFO(LOG_APP, "[MW-Life] CreateRenderer tl=%{public}u count=%{public}zu→%{public}zu",
                toplevelId, toplevelRenderers_.size(), toplevelRenderers_.size() + 1);
    OH_LOG_INFO(LOG_APP, "[MW-Create] toplevel=%{public}u surfaceId=%{public}ld existRender=%{public}zu",
                toplevelId, surfaceId, toplevelRenderers_.size());

    // 防御: 如果已有旧 renderer, 先 shutdown
    auto old = toplevelRenderers_.find(toplevelId);
    if (old != toplevelRenderers_.end()) {
        OH_LOG_WARN(LOG_APP, "[MW-Create] WARN toplevel #%{public}u already has renderer, shutting down old", toplevelId);
        old->second->Shutdown();
        toplevelRenderers_.erase(old);
    }

    OHNativeWindow* win = nullptr;
    int ret = OH_NativeWindow_CreateNativeWindowFromSurfaceId((uint64_t)surfaceId, &win);
    if (ret != 0 || !win) {
        OH_LOG_ERROR(LOG_APP, "[MW-Create] ERR toplevel #%{public}u CreateNativeWindowFromSurfaceId FAILED ret=%{public}d",
                     toplevelId, ret);
        return false;
    }
    OH_LOG_INFO(LOG_APP, "[MW-Create] native window created %{public}p for toplevel #%{public}u", win, toplevelId);

    // 构造注入 frame compositor (重构第 6A 步): 渲染器取帧/ZC 状态机经
    // DesktopCompositor 直连 — 替代旧 WaylandServer 门面 26 处转发
    // (TakeToplevelFrame/GetZeroCopyLayerInfo/ActivateZcSurface 等)。
    // compositor 生命周期长于一切 renderer (WaylandServer 单例成员, 装配出口
    // 见 wayland_server.h GetDesktopCompositor 注释), 无后置装配竞态。
    auto r = std::make_unique<EglRenderer>(
        WaylandServer::GetInstance()->GetDesktopCompositor());
    // 初始尺寸 1x1, 真正尺寸由 ResizeRenderer (onSurfaceChanged) 设置
    r->SetToplevelId(toplevelId);
    if (r->Init(win, 1, 1)) {
        toplevelRenderers_[toplevelId] = std::move(r);
        OH_LOG_INFO(LOG_APP, "[MW-Create] OK toplevel #%{public}u renderer created (total=%{public}zu)",
                    toplevelId, toplevelRenderers_.size());
        return true;
    }
    OH_LOG_ERROR(LOG_APP, "[MW-Create] ERR toplevel #%{public}u EglRenderer::Init FAILED", toplevelId);
    // 销毁刚才创建的 native window
    OH_NativeWindow_DestroyNativeWindow(win);
    return false;
}

void PluginManager::SetRendererStretchFill(uint32_t toplevelId, bool on) {
    auto rit = toplevelRenderers_.find(toplevelId);
    if (rit != toplevelRenderers_.end() && rit->second->IsValid()) {
        rit->second->SetStretchFill(on);
    }
}

void PluginManager::ResizeRenderer(uint32_t toplevelId, int w, int h) {
    OH_LOG_INFO(LOG_APP, "[MW-Resize] toplevel=%{public}u size=%{public}dx%{public}d", toplevelId, w, h);

    auto rit = toplevelRenderers_.find(toplevelId);
    if (rit != toplevelRenderers_.end() && rit->second->IsValid()) {
        rit->second->SetSize(w, h);
        OH_LOG_INFO(LOG_APP, "[MW-Resize] toplevel #%{public}u renderer resized OK", toplevelId);

        // surface 尺寸变化后强制重绘: 即使没有新帧, 也需用当前帧在
        // 新 surface 上重新 letterbox, 避免旧 viewport 残留导致黑边
        WaylandServer::GetInstance()->ForceToplevelRedraw(toplevelId);

        // 通知 ArkTS surface 的物理像素尺寸, 用于动态计算标题栏高度
        WaylandServer::GetInstance()->PostToplevelEvent(
            toplevelId, ToplevelEventType::Surface,
            ToplevelEventBus::JsonSurface(w, h));
    } else {
        OH_LOG_WARN(LOG_APP, "[MW-Resize] toplevel #%{public}u renderer NOT found or invalid", toplevelId);
    }
}

void PluginManager::DestroyToplevel(uint32_t toplevelId) {
    OH_LOG_INFO(LOG_APP, "[MW-Life] DestroyRenderer tl=%{public}u count=%{public}zu→%{public}zu",
                toplevelId, toplevelRenderers_.size(), toplevelRenderers_.size() > 0 ? toplevelRenderers_.size() - 1 : 0);
    auto it = toplevelRenderers_.find(toplevelId);
    if (it != toplevelRenderers_.end()) {
        it->second->Shutdown();
        toplevelRenderers_.erase(it);
        OH_LOG_INFO(LOG_APP, "[MW-Destroy] toplevel #%{public}u renderer destroyed (remaining: %{public}zu)",
                    toplevelId, toplevelRenderers_.size());
    } else {
        OH_LOG_WARN(LOG_APP, "[MW-Destroy] toplevel #%{public}u NOT found (remaining: %{public}zu)",
                    toplevelId, toplevelRenderers_.size());
    }
}

EglRenderer* PluginManager::GetRendererForToplevel(uint32_t tid) {
    auto rit = toplevelRenderers_.find(tid);
    if (rit == toplevelRenderers_.end()) return nullptr;
    return rit->second.get();
}

EglRenderer* PluginManager::GetAnyRenderer() {
    if (toplevelRenderers_.empty()) return nullptr;
    return toplevelRenderers_.begin()->second.get();
}

void PluginManager::MoveRendererToToplevel(uint32_t oldId, uint32_t newId) {
    OH_LOG_INFO(LOG_APP, "[MW-Life] MoveRenderer tl %{public}u→%{public}u", oldId, newId);
    if (oldId == newId) { OH_LOG_WARN(LOG_APP, "[MW-Life] MoveRenderer SKIP: old==new"); return; }
    auto it = toplevelRenderers_.find(oldId);
    if (it == toplevelRenderers_.end()) {
        OH_LOG_WARN(LOG_APP, "[MW-Life] MoveRenderer old tl=%{public}u NOT FOUND", oldId);
        return;
    }
    OH_LOG_INFO(LOG_APP, "[MW-Plug] MoveRenderer tl #%{public}u -> #%{public}u", oldId, newId);
    toplevelRenderers_[newId] = std::move(it->second);
    toplevelRenderers_.erase(it);
}

// ========================================================================
//  NAPI Export
// ========================================================================

// todo: 未来放入 egl_surface -> 改为 native 实现。目前暂时放在这里
// 阶段 1 (P1.1): canonical 入口 attachSurface/resizeSurface/detachSurface
// 经 HgsInstance 门面 (generation 表); 旧名 createRenderer/resizeRenderer/
// destroyRenderer 转发并打一次性 [HGS-DEPRECATED]。
namespace hgs {
namespace surface {

// -- NAPI: attachSurface -- (canonical; (toplevelId, surfaceId:bigint) → generation)
// 失败返回 0。Native XComponent 路径 (inputBackend=Native) 不经此入口 —
// OnSurfaceCreated 直接持 OHNativeWindow*, 由 HGS 内部绑定 (phase0 §14.5)。
napi_value AttachSurfaceNAPI(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 2) {
        OH_LOG_ERROR(LOG_APP, "[MW-NAPI] attachSurface: need 2 args (toplevelId, surfaceId)");
        napi_value r;
        napi_create_uint32(env, 0, &r);
        return r;
    }
    uint32_t tid = 0;
    napi_get_value_uint32(env, args[0], &tid);
    int64_t surfaceId = 0;
    bool lossless = true;
    napi_status s = napi_get_value_bigint_int64(env, args[1], &surfaceId, &lossless);
    if (s != napi_ok) {
        OH_LOG_ERROR(LOG_APP, "[MW-NAPI] attachSurface: BIGINT parse failed status=%{public}d", s);
        napi_value r;
        napi_create_uint32(env, 0, &r);
        return r;
    }
    OH_LOG_INFO(LOG_APP, "[MW-NAPI] attachSurface tl=%{public}u surfaceId=%{public}ld", tid, surfaceId);
    const uint32_t gen = HgsInstance::Instance().AttachSurface(tid, static_cast<uint64_t>(surfaceId));
    if (gen == 0) {
        OH_LOG_ERROR(LOG_APP, "[MW-NAPI] attachSurface FAILED (see getLastError)");
    }
    napi_value r;
    napi_create_uint32(env, gen, &r);
    return r;
}

// -- NAPI: resizeSurface -- (canonical; generation 缺省 = 0 通配)
napi_value ResizeSurfaceNAPI(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value args[4];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 3) {
        OH_LOG_ERROR(LOG_APP, "[MW-NAPI] resizeSurface: need 3 args (toplevelId, w, h[, generation])");
        napi_value r;
        napi_get_boolean(env, false, &r);
        return r;
    }
    uint32_t tid = 0, gen = 0;
    napi_get_value_uint32(env, args[0], &tid);
    int32_t w = 0, h = 0;
    napi_get_value_int32(env, args[1], &w);
    napi_get_value_int32(env, args[2], &h);
    if (argc >= 4) napi_get_value_uint32(env, args[3], &gen);
    OH_LOG_INFO(LOG_APP, "[MW-NAPI] resizeSurface tl=%{public}u %{public}dx%{public}d gen=%{public}u", tid, w, h, gen);
    const bool ok = HgsInstance::Instance().ResizeSurface(tid, w, h, gen);
    napi_value r;
    napi_get_boolean(env, ok, &r);
    return r;
}

// -- NAPI: detachSurface -- (canonical; 只释放 surface/renderer, 不触碰
//    toplevel 与 pointer lock; generation 缺省 = 0 通配)
napi_value DetachSurfaceNAPI(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    uint32_t tid = 0, gen = 0;
    if (argc >= 1) napi_get_value_uint32(env, args[0], &tid);
    if (argc >= 2) napi_get_value_uint32(env, args[1], &gen);
    OH_LOG_INFO(LOG_APP, "[MW-NAPI] detachSurface tl=%{public}u gen=%{public}u", tid, gen);
    const bool ok = HgsInstance::Instance().DetachSurface(tid, gen);
    napi_value r;
    napi_get_boolean(env, ok, &r);
    return r;
}

// -- NAPI: createRenderer -- @deprecated → attachSurface (返回值忽略 generation)
napi_value CreateRendererNAPI(napi_env env, napi_callback_info info) {
    static bool s_dep = false;
    if (!s_dep) {
        s_dep = true;
        OH_LOG_WARN(LOG_APP, "[HGS-DEPRECATED] createRenderer → attachSurface (阶段 3 删除)");
    }
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 2) {
        OH_LOG_ERROR(LOG_APP, "[MW-NAPI] createRenderer: need 2 args (toplevelId, surfaceId)");
        return nullptr;
    }
    uint32_t tid = 0;
    napi_get_value_uint32(env, args[0], &tid);
    int64_t surfaceId = 0;
    bool lossless = true;
    napi_status s = napi_get_value_bigint_int64(env, args[1], &surfaceId, &lossless);
    if (s != napi_ok) {
        OH_LOG_ERROR(LOG_APP, "[MW-NAPI] createRenderer: BIGINT parse failed status=%{public}d", s);
        return nullptr;
    }
    OH_LOG_INFO(LOG_APP, "[MW-NAPI] createRenderer tl=%{public}u surfaceId=%{public}ld", tid, surfaceId);
    HgsInstance::Instance().AttachSurface(tid, static_cast<uint64_t>(surfaceId));  // generation 忽略
    return nullptr;
}

// -- NAPI: resizeRenderer -- @deprecated → resizeSurface(gen=0)
napi_value ResizeRendererNAPI(napi_env env, napi_callback_info info) {
    static bool s_dep = false;
    if (!s_dep) {
        s_dep = true;
        OH_LOG_WARN(LOG_APP, "[HGS-DEPRECATED] resizeRenderer → resizeSurface (阶段 3 删除)");
    }
    size_t argc = 3;
    napi_value args[3];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 3) {
        OH_LOG_ERROR(LOG_APP, "[MW-NAPI] resizeRenderer: need 3 args (toplevelId, w, h)");
        return nullptr;
    }
    uint32_t tid = 0;
    napi_get_value_uint32(env, args[0], &tid);
    int32_t w = 0, h = 0;
    napi_get_value_int32(env, args[1], &w);
    napi_get_value_int32(env, args[2], &h);
    OH_LOG_INFO(LOG_APP, "[MW-NAPI] resizeRenderer tl=%{public}u %{public}dx%{public}d", tid, w, h);
    HgsInstance::Instance().ResizeSurface(tid, w, h, 0);
    return nullptr;
}

// -- NAPI: destroyRenderer -- @deprecated → detachSurface(gen=0)
napi_value DestroyRendererNAPI(napi_env env, napi_callback_info info) {
    static bool s_dep = false;
    if (!s_dep) {
        s_dep = true;
        OH_LOG_WARN(LOG_APP, "[HGS-DEPRECATED] destroyRenderer → detachSurface (阶段 3 删除)");
    }
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    uint32_t tid = 0;
    if (argc >= 1) {
        napi_get_value_uint32(env, args[0], &tid);
    }
    OH_LOG_INFO(LOG_APP, "[MW-NAPI] destroyRenderer tl=%{public}u", tid);
    HgsInstance::Instance().DetachSurface(tid, 0);
    return nullptr;
}

} // namespace surface
} // namespace hgs

