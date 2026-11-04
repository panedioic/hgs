// input/native_input.cpp — 原生输入回调 → HgsInstance (阶段 1 N1)
//
// 回调线程 = UI 线程; 只做字段转换 + HgsInstance::Send* (InputManager 入队,
// Wayland 线程注入), 不直接碰 wl_*_send_* (沿用 input_manager.h:15-32 线程模型)。
//
// 坐标单位: OH_NativeXComponent_MouseEvent.x/y / TouchEvent.x/y 的注释是
// "relative to the left edge of the element", 未写明 vp/px — N1 先原样透传
// 并在首事件打一次诊断日志 (x/y/screenX/screenY + 组件像素尺寸), 由真机
// 实测决定是否需要 vp2px (phase0 §14.4)。

#include "native_input.h"

#include <arkui/ui_input_event.h>

#include <cmath>
#include <mutex>
#include <unordered_map>

#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_DOMAIN 0x0000
#define LOG_TAG "HGS_NativeInput"
#include <hilog/log.h>

#include "hgs_instance.h"
#include "native_keymap.h"
#include "native_mousemap.h"

namespace hgs {
namespace input {

namespace {

struct Binding {
    ToplevelId toplevelId = 0;
    bool callbacksRegistered = false;
    bool touchDown = false;       // N1: 主触摸点按下状态 (Up/Cancel 配对)
    bool calibrationLogged = false;
    bool unknownKeyLogged = false;
};

std::mutex gMutex;
std::unordered_map<OH_NativeXComponent*, Binding> gBindings;

// 取绑定快照; 未绑定返回 toplevelId=0。
Binding Snapshot(OH_NativeXComponent* comp) {
    std::lock_guard<std::mutex> lk(gMutex);
    const auto it = gBindings.find(comp);
    if (it == gBindings.end()) return Binding{};
    return it->second;
}

void Update(OH_NativeXComponent* comp, const Binding& b) {
    std::lock_guard<std::mutex> lk(gMutex);
    gBindings[comp] = b;
}

// 后端开关: 仅 Native 模式注入 (ArkTS 模式下回调注册但静默, 避免双发)。
bool Enabled() {
    return HgsInstance::Instance().GetInputBackend() == InputBackend::Native;
}

void LogCalibrationOnce(OH_NativeXComponent* comp, void* window, Binding& b,
                        const char* source, float x, float y,
                        float screenX, float screenY) {
    if (b.calibrationLogged) return;
    b.calibrationLogged = true;
    uint64_t w = 0, h = 0;
    OH_NativeXComponent_GetXComponentSize(comp, window, &w, &h);
    OH_LOG_INFO(LOG_APP,
                "[NAPI-INPUT] %{public}s calibration: x=%{public}.1f y=%{public}.1f "
                "screen=(%{public}.1f,%{public}.1f) surface=%{public}ux%{public}u "
                "(单位待真机实测 — phase0 §14.4)",
                source, x, y, screenX, screenY,
                static_cast<unsigned>(w), static_cast<unsigned>(h));
}

// ── mouse callback ─────────────────────────────────────────────────────────
void DispatchMouseEvent(OH_NativeXComponent* comp, void* window) {
    if (!comp || !window || !Enabled()) return;
    Binding b = Snapshot(comp);
    if (b.toplevelId == 0) return;

    OH_NativeXComponent_MouseEvent ev;
    if (OH_NativeXComponent_GetMouseEvent(comp, window, &ev) !=
        OH_NATIVEXCOMPONENT_RESULT_SUCCESS) {
        return;
    }
    LogCalibrationOnce(comp, window, b, "mouse", ev.x, ev.y, ev.screenX, ev.screenY);

    const int32_t button = OhosMouseButtonToEvdev(static_cast<int32_t>(ev.button));
    PointerEvent pe;
    pe.hintToplevel = b.toplevelId;
    pe.x = ev.x;
    pe.y = ev.y;
    pe.button = static_cast<uint32_t>(button);
    pe.fromMouse = true;

    switch (ev.action) {
        case OH_NATIVEXCOMPONENT_MOUSE_PRESS:
            if (button == 0) return;
            pe.action = PointerAction::Press;
            break;
        case OH_NATIVEXCOMPONENT_MOUSE_RELEASE:
            if (button == 0) return;
            pe.action = PointerAction::Release;
            break;
        case OH_NATIVEXCOMPONENT_MOUSE_MOVE:
            pe.action = PointerAction::Move;
            pe.button = 0;
            break;
        default:
            return;  // NONE/CANCEL: N1 忽略
    }
    HgsInstance::Instance().SendPointerEvent(pe);
}

void DispatchHoverEvent(OH_NativeXComponent* comp, bool isHover) {
    (void)comp;
    (void)isHover;  // N1: hover 不进输入队列 (phase0 §14.4)
}

OH_NativeXComponent_MouseEvent_Callback gMouseCb = {
    DispatchMouseEvent,
    DispatchHoverEvent,
};

// ── key callback ───────────────────────────────────────────────────────────
void DispatchKeyEvent(OH_NativeXComponent* comp, void* window) {
    (void)window;
    if (!comp || !Enabled()) return;
    Binding b = Snapshot(comp);
    if (b.toplevelId == 0) return;

    OH_NativeXComponent_KeyEvent* keyEvent = nullptr;
    if (OH_NativeXComponent_GetKeyEvent(comp, &keyEvent) !=
            OH_NATIVEXCOMPONENT_RESULT_SUCCESS ||
        keyEvent == nullptr) {
        return;
    }
    OH_NativeXComponent_KeyAction action = OH_NATIVEXCOMPONENT_KEY_ACTION_UNKNOWN;
    OH_NativeXComponent_KeyCode code = KEY_UNKNOWN;
    OH_NativeXComponent_GetKeyEventAction(keyEvent, &action);
    OH_NativeXComponent_GetKeyEventCode(keyEvent, &code);
    if (action != OH_NATIVEXCOMPONENT_KEY_ACTION_DOWN &&
        action != OH_NATIVEXCOMPONENT_KEY_ACTION_UP) {
        return;
    }

    const int32_t evdev = OhosKeyCodeToEvdev(static_cast<int32_t>(code));
    if (evdev == 0) {
        if (!b.unknownKeyLogged) {
            b.unknownKeyLogged = true;
            OH_LOG_WARN(LOG_APP, "[NAPI-INPUT] unmapped OHOS keycode=%{public}d dropped",
                        static_cast<int>(code));
        }
        Update(comp, b);
        return;
    }

    KeyEvent ke;
    ke.toplevelId = b.toplevelId;
    ke.evdevCode = evdev;
    ke.pressed = action == OH_NATIVEXCOMPONENT_KEY_ACTION_DOWN;
    HgsInstance::Instance().SendKeyEvent(ke);
}

// ── axis callback (滚轮; ArkUI_UIInputEvent) ───────────────────────────────
void DispatchAxisEvent(OH_NativeXComponent* comp, ArkUI_UIInputEvent* event,
                       ArkUI_UIInputEvent_Type type) {
    (void)type;
    if (!comp || !event || !Enabled()) return;
    Binding b = Snapshot(comp);
    if (b.toplevelId == 0) return;

    const double v = OH_ArkUI_AxisEvent_GetVerticalAxisValue(event);
    const double h = OH_ArkUI_AxisEvent_GetHorizontalAxisValue(event);
    const int32_t step = OH_ArkUI_AxisEvent_GetScrollStep(event);
    const double x = OH_ArkUI_PointerEvent_GetX(event);
    const double y = OH_ArkUI_PointerEvent_GetY(event);

    ScrollEvent se;
    se.hintToplevel = b.toplevelId;
    se.axis = (std::abs(v) >= std::abs(h)) ? 0 : 1;
    se.value = (se.axis == 0) ? v : h;
    se.scrollStep = step;
    se.x = x;
    se.y = y;
    if (se.value == 0.0 && step == 0) return;  // 无滚动的轴事件 (手势开始/结束)
    HgsInstance::Instance().SendScrollEvent(se);
}

// ── focus/blur (N1: 仅记录) ─────────────────────────────────────────────────
void DispatchFocusEvent(OH_NativeXComponent* comp, void* window) {
    (void)window;
    if (!comp) return;
    const Binding b = Snapshot(comp);
    OH_LOG_INFO(LOG_APP, "[NAPI-INPUT] focus tl=%{public}u", b.toplevelId);
}

void DispatchBlurEvent(OH_NativeXComponent* comp, void* window) {
    (void)window;
    if (!comp) return;
    const Binding b = Snapshot(comp);
    OH_LOG_INFO(LOG_APP, "[NAPI-INPUT] blur tl=%{public}u", b.toplevelId);
}

} // namespace

void Bind(OH_NativeXComponent* comp, ToplevelId toplevelId) {
    if (!comp) return;
    bool needRegister = false;
    {
        std::lock_guard<std::mutex> lk(gMutex);
        Binding& b = gBindings[comp];
        b.toplevelId = toplevelId;
        if (!b.callbacksRegistered) {
            b.callbacksRegistered = true;
            needRegister = true;
        }
    }
    if (!needRegister) return;

    // touch 由 egl_surface 的 OH_NativeXComponent_Callback.DispatchTouchEvent 承担,
    // 本函数只注册 mouse/key/axis/focus/blur (不覆盖 surface 回调结构体)。
    OH_NativeXComponent_RegisterMouseEventCallback(comp, &gMouseCb);
    OH_NativeXComponent_RegisterKeyEventCallback(comp, DispatchKeyEvent);
    OH_NativeXComponent_RegisterFocusEventCallback(comp, DispatchFocusEvent);
    OH_NativeXComponent_RegisterBlurEventCallback(comp, DispatchBlurEvent);
    const int32_t rc = OH_NativeXComponent_RegisterUIInputEventCallback(
        comp, DispatchAxisEvent, ARKUI_UIINPUTEVENT_TYPE_AXIS);
    OH_LOG_INFO(LOG_APP,
                "[NAPI-INPUT] bind comp=%{public}p tl=%{public}u axisRc=%{public}d",
                static_cast<void*>(comp), toplevelId, rc);
}

void Unbind(OH_NativeXComponent* comp) {
    if (!comp) return;
    std::lock_guard<std::mutex> lk(gMutex);
    gBindings.erase(comp);
}

void DispatchTouchEvent(OH_NativeXComponent* comp, void* window) {
    if (!comp || !window || !Enabled()) return;
    Binding b = Snapshot(comp);
    if (b.toplevelId == 0) return;

    OH_NativeXComponent_TouchEvent ev;
    if (OH_NativeXComponent_GetTouchEvent(comp, window, &ev) !=
        OH_NATIVEXCOMPONENT_RESULT_SUCCESS) {
        return;
    }
    LogCalibrationOnce(comp, window, b, "touch", ev.x, ev.y, ev.screenX, ev.screenY);

    PointerEvent pe;
    pe.hintToplevel = b.toplevelId;
    pe.x = ev.x;
    pe.y = ev.y;
    pe.button = 0x110;  // BTN_LEFT
    pe.fromMouse = false;

    switch (ev.type) {
        case OH_NATIVEXCOMPONENT_DOWN:
            if (b.touchDown) return;  // N1: 只跟踪主触摸点
            b.touchDown = true;
            Update(comp, b);
            pe.action = PointerAction::Press;
            break;
        case OH_NATIVEXCOMPONENT_MOVE:
            if (!b.touchDown) return;
            pe.action = PointerAction::Move;
            pe.button = 0;
            break;
        case OH_NATIVEXCOMPONENT_UP:
        case OH_NATIVEXCOMPONENT_CANCEL:
            if (!b.touchDown) return;
            b.touchDown = false;
            Update(comp, b);
            pe.action = PointerAction::Release;
            break;
        default:
            return;
    }
    HgsInstance::Instance().SendPointerEvent(pe);
}

} // namespace input
} // namespace hgs
