// api/hgs_window_policy.cpp — 默认浮动窗口策略 (阶段 2 W1)。
// 全部透传: PerToplevel 模式下 OHOS 系统窗口管理器负责最终叠放/移动/缩放,
// core 只维护逻辑顺序与焦点 (phase0 §15.4)。

#include "hgs_window_policy.h"

namespace hgs {

void FloatingWindowPolicy::OnCreated(WindowSpec& spec) {
    (void)spec;
}

void FloatingWindowPolicy::OnRaise(WindowId id) {
    (void)id;
}

void FloatingWindowPolicy::OnFocus(WindowId id) {
    (void)id;
}

void FloatingWindowPolicy::OnStateRequest(WindowId id, WindowState state) {
    (void)id;
    (void)state;
}

void FloatingWindowPolicy::OnGeometryRequest(WindowId id, const WindowGeometry& geometry) {
    (void)id;
    (void)geometry;
}

bool FloatingWindowPolicy::OnCloseRequest(WindowId id) {
    (void)id;
    return true;
}

} // namespace hgs
