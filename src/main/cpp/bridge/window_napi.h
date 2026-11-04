#pragma once
// ============================================================================
// window_napi.h — 窗口管理器 NAPI 导出 (阶段 2 W2a)。
//
// 通道:
//   setWindowCallback(cb)         core → JS: WindowEvent (TSFN, UI 线程)
//   notifyWindowGeometry/Focus/State/Closed  JS → core: host 回执
//   getWindow/listWindows/getWindowZOrder    查询快照
//
// 事件驱动 host (默认 ArkTS) 消费 WindowEvent: Created 建窗口, TitleChanged
// 设标题, GeometryChanged 调整, StateChanged 最大化/还原, RaiseRequested
// 带到前台, CloseRequested 关闭。IWindowHost 留给原生 host (W2c)。
// ============================================================================

#include <napi/native_api.h>

namespace hgs {
namespace window_napi {

napi_value SetWindowCallbackNAPI(napi_env env, napi_callback_info info);
napi_value NotifyWindowGeometryNAPI(napi_env env, napi_callback_info info);
napi_value NotifyWindowFocusNAPI(napi_env env, napi_callback_info info);
napi_value NotifyWindowStateNAPI(napi_env env, napi_callback_info info);
napi_value NotifyWindowClosedNAPI(napi_env env, napi_callback_info info);
napi_value GetWindowNAPI(napi_env env, napi_callback_info info);
napi_value ListWindowsNAPI(napi_env env, napi_callback_info info);
napi_value GetWindowZOrderNAPI(napi_env env, napi_callback_info info);

} // namespace window_napi
} // namespace hgs
