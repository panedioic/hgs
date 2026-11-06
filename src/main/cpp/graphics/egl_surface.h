// hgs/src/main/cpp/graphics/egl_surface.h
//
// Native 侧 Surface 生命周期 (阶段 1 N1 起为原生路径的唯一入口):
// XComponent libraryname:'hgs' 触发 NativeXComponent 回调; OnSurfaceCreated
// 保存 OHNativeWindow*, bindXComponent(event, toplevelId) 后由
// PluginManager::CreateRendererFromWindow 建真实渲染器 (项目 SDK 无
// GetXComponentSurfaceId, 只能走 window 指针)。输入回调见 input/native_input。
#pragma once

#include <ace/xcomponent/native_interface_xcomponent.h>
#include <hilog/log.h>
#include <napi/native_api.h>

namespace hgs {
namespace surface {

void OnSurfaceCreated(OH_NativeXComponent *comp, void *window);

void OnSurfaceChanged(OH_NativeXComponent *comp, void *window);

void OnSurfaceDestroyed(OH_NativeXComponent *comp, void *window);

extern OH_NativeXComponent_Callback gXcompCb;

// NAPI: bindXComponent(xcompObject, toplevelId?) — 非 Init 触发的 XComponent
// (同进程第二个窗口: 模块已加载, Init 不会再次执行) 在 onLoad 里显式补挂
// 回调; 带 toplevelId 时同时绑定原生输入与 renderer。
napi_value BindXComponent(napi_env env, napi_callback_info info);

// regXComponentCallback 成功 unwrap 后记录本窗口的组件指针。多窗口时第二个
// 窗口的原生 OnSurfaceCreated 早于回调注册触发 (那时回调还没挂上, 事件丢),
// 于是 bindXComponent 等不到 surface — 只能直接绑"刚注册的这个组件"拿输入,
// 渲染器则由 ArkTS controller 的 surfaceId 兜底 (见 AttachSurfaceFallback)。
void NoteRegisteredComponent(OH_NativeXComponent *comp);

// NAPI: attachSurfaceFallback(toplevelId, surfaceId: bigint) — ArkTS controller
// onSurfaceCreated 调; 原生路径已建 renderer 时 no-op (返回 true)。
napi_value AttachSurfaceFallback(napi_env env, napi_callback_info info);

} // namespace surface
} // namespace hgs
