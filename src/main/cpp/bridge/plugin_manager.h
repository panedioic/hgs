#pragma once
#include "graphics/egl_renderer.h"
#include <unordered_map>
#include <memory>
#include <cstdint>
#include <deque>

#include "napi/native_api.h"  // 尾部 hgs::surface 的 NAPI 导出声明

// surfaceId 驱动的 XComponent 管理
//
// 每个 XComponent 通过自定义 Controller 回调拿到的 surfaceId 是唯一的,
// 不再共享 libraryname → exports 对象, 从根本上消除多窗口 XComponent 冲突。
//
// 架构:
//   WineWindow.ets (XComponentController.onSurfaceCreated)
//     → NAPI createRenderer(toplevelId, surfaceId)
//     → PluginManager::CreateRenderer → OH_NativeWindow_CreateNativeWindowFromSurfaceId
//     → EglRenderer::Init(nativeWindow, 1, 1) → 共享 EGLDisplay + 独立 EGLContext
//   WineWindow.ets (XComponentController.onSurfaceChanged)
//     → NAPI resizeRenderer(toplevelId, w, h)
//     → PluginManager::ResizeRenderer → EglRenderer::SetSize
//   WineWindow.ets (XComponentController.onSurfaceDestroyed)
//     → NAPI destroyRenderer(toplevelId) → DestroyToplevel
//   WaylandServer::NotifyToplevelResize (拖拽缩放标记随 configure 同步)
//     → PluginManager::SetRendererStretchFill → EglRenderer::SetStretchFill
//
// 键盘事件仅通过 Stack.onKeyEvent → NAPI sendKeyEvent → InputManager 路径,
// 不再使用 OH_NativeXComponent_RegisterKeyEventCallback。
class PluginManager {
public:
    static PluginManager* GetInstance();

    // surfaceId 驱动的渲染器生命周期
    // 返回 true = native window + EglRenderer::Init 成功 (阶段 0 P0.3)。
    // 旧调用方 (CreateRendererNAPI) 忽略返回值, 行为不变;
    // HgsInstance::AttachSurface 依据返回值分配 generation。
    bool CreateRenderer(uint32_t toplevelId, int64_t surfaceId);
    void ResizeRenderer(uint32_t toplevelId, int w, int h);
    // 拖拽缩放中: 目标渲染器整帧拉伸填满 (EglRenderer::SetStretchFill)
    void SetRendererStretchFill(uint32_t toplevelId, bool on);
    void DestroyToplevel(uint32_t toplevelId);

    // pending toplevelId 队列: Ability 在 loadContent 前入队
    // WineWindow.aboutToAppear 同步出队 (FIFO); 出队方窗口被销毁时
    // 用 CancelPendingToplevel 清除残坑, 防止后续页面出队错位
    // (getCurrentToplevelId 拿到别人的 id → 渲染器挂错 toplevel → 黑屏,
    // 1.8 实测: show/hide 快速交替的 destroy-while-creating 页面无 aboutToAppear)
    void SetPendingToplevel(uint32_t id) { pendingToplevelQueue_.push_back(id); }
    uint32_t DequeuePendingToplevel();
    void CancelPendingToplevel(uint32_t id);

    // 辅助: toplevelId -> EglRenderer 查找 (InputManager 坐标转换使用)
    EglRenderer* GetRendererForToplevel(uint32_t tid);
    // Desktop 合成模式: 取当前登记的唯一 renderer（输入坐标映射兜底）。
    // RootCompositing 下所有 renderer 都渲染桌面根，letterbox 与登记 id
    // 无关；桌面根重建/前台窗口"提升"导致按 id 查不到 renderer 时用它
    // 仍能得到正确的 viewport 映射。
    EglRenderer* GetAnyRenderer();
    // Desktop 模式: root 切换时更新渲染器的 toplevel 映射
    void MoveRendererToToplevel(uint32_t oldId, uint32_t newId);
    size_t GetRendererCount() const { return toplevelRenderers_.size(); }

private:
    PluginManager() = default;

    // 每个 toplevel 一个独立 EGLContext 渲染器
    std::unordered_map<uint32_t, std::unique_ptr<EglRenderer>> toplevelRenderers_;

    // pending queue: Ability 入队, WineWindow.aboutToAppear 出队 (FIFO);
    // deque 支持 CancelPendingToplevel 的定点删除 (queue 底层不可迭代)
    std::deque<uint32_t> pendingToplevelQueue_;
};

// todo: 未来放入 egl_surface -> 改为 native 实现。目前暂时放在这里
namespace hgs {
namespace surface {

// 阶段 1 (P1.1) canonical surface 生命周期 (经 HgsInstance 门面):
// attachSurface 返回 generation (0 = 失败); resize/detach 的 generation
// 缺省 = 0 通配。旧名 createRenderer/resizeRenderer/destroyRenderer
// 转发并打一次性 [HGS-DEPRECATED] (定义暂在本文件 .cpp, todo 迁入 egl_surface)
napi_value AttachSurfaceNAPI(napi_env env, napi_callback_info info);
napi_value ResizeSurfaceNAPI(napi_env env, napi_callback_info info);
napi_value DetachSurfaceNAPI(napi_env env, napi_callback_info info);

// @deprecated (阶段 1 起转发 attach/resize/detach)
napi_value CreateRendererNAPI(napi_env env, napi_callback_info info);
napi_value ResizeRendererNAPI(napi_env env, napi_callback_info info);
napi_value DestroyRendererNAPI(napi_env env, napi_callback_info info);

} // namespace surface
} // namespace hgs
