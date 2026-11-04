#pragma once
// ============================================================================
// native_input.h — OH_NativeXComponent 输入回调 → HgsInstance (阶段 1 N1)。
//
// 目标形态 (phase0 §14): ArkTS 侧不注册任何输入事件, 鼠标/键盘/滚轮/触摸
// 全部由 C++ 侧 OH_NativeXComponent 回调处理。N1 范围:
//   - mouse: PRESS/RELEASE/MOVE + hover (hover 只更新位置, 不产生按键)
//   - key: DOWN/UP (OHOS KeyCode → evdev, native_keymap)
//   - axis: 滚轮/轴 (ARKUI_UIINPUTEVENT_TYPE_AXIS)
//   - touch: 单击=左键 (Down/Move/Up/Cancel); 长按/双击/双指等手势留 N2
//   - focus/blur: 记录获焦窗口 (N1 仅诊断)
//
// 后端开关: 只有 HgsInstance::GetInputBackend()==Native 时才注入事件 —
// ArkTS 后端 (默认) 下回调注册也无副作用, 避免与 ArkUI 事件双发。
//
// 绑定: OH_NativeXComponent* → ToplevelId (bindXComponent 第二参数)。
// 首个 XComponent 由 libraryname:'hgs' 触发 Init 注册 surface 回调;
// onLoad 里 bindXComponent(event, id) 完成输入绑定。同进程后续 XComponent
// 同路径补挂。
// ============================================================================

#include <ace/xcomponent/native_interface_xcomponent.h>

#include "hgs_export.h"
#include "hgs_types.h"

namespace hgs {
namespace input {

// 绑定 XComponent 与 toplevel (幂等; 首次注册 mouse/key/axis/focus/blur 回调)。
// touch 回调由 graphics/egl_surface 的 OH_NativeXComponent_Callback 统一
// 注册 (同结构体含 surface 生命周期), 见 DispatchTouchEvent。
HGS_API void Bind(OH_NativeXComponent* comp, ToplevelId toplevelId);

// 解绑 (OnSurfaceDestroyed)。不注销 C 回调 (组件即将销毁), 只清绑定表。
HGS_API void Unbind(OH_NativeXComponent* comp);

// egl_surface 的 DispatchTouchEvent 入口。
HGS_API void DispatchTouchEvent(OH_NativeXComponent* comp, void* window);

} // namespace input
} // namespace hgs
