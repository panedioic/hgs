#pragma once
// ============================================================================
// native_mousemap.h — ArkUI/OHOS MouseButton → Linux evdev button (阶段 1 N1)。
// 移植自 hgs/src/main/ets/common/MouseMap.ets:
//   Left=1 Right=2 Middle=4 Back=8 Forward=16
//   → BTN_LEFT=0x110 BTN_RIGHT=0x111 BTN_MIDDLE=0x112
//     BTN_BACK=0x116 BTN_FORWARD=0x115
// 纯内联函数 (零平台依赖), host test 可直连。
// ============================================================================

#include <cstdint>

#include "hgs_export.h"

namespace hgs {
namespace input {

inline int32_t OhosMouseButtonToEvdev(int32_t button) {
    switch (button) {
        case 1:  return 0x110;  // BTN_LEFT
        case 2:  return 0x111;  // BTN_RIGHT
        case 4:  return 0x112;  // BTN_MIDDLE
        case 8:  return 0x116;  // BTN_BACK
        case 16: return 0x115;  // BTN_FORWARD
        default: return 0;
    }
}

} // namespace input
} // namespace hgs
