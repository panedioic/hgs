#pragma once
// ============================================================================
// native_keymap.h — OHOS KeyCode → Linux evdev scancode 映射 (阶段 1 N1)。
//
// 移植自 hgs/src/main/ets/common/KeyMap.ets (OH2EVDEV, 逐项对账):
//   OHOS keycode 范围 ~2000-2122, evdev 范围 1-255。
//   Wine keyboard_handle_key() 接收 raw evdev code (不需 +8)。
//
// 纯函数 (零平台依赖) — host_tests/native_keymap_test.cpp 直连编译,
// 用 KeyMap.ets 的条目做黄金值对账。
// ============================================================================

#include <cstdint>

#include "hgs_export.h"

namespace hgs {
namespace input {

// 返回 0 = 未知/无映射 (调用方丢弃并打一次 WARN)。
HGS_API int32_t OhosKeyCodeToEvdev(int32_t ohosKeyCode);

// 是否修饰键 (ALT/SHIFT/CTRL/META/CAPS_LOCK) — 阶段 N1 仅用于诊断计数;
// modifier 状态仍由 Wine 从 key 事件自行推导。
HGS_API bool IsModifierKeyCode(int32_t ohosKeyCode);

} // namespace input
} // namespace hgs
