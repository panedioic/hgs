#include "input_router.h"
namespace hgs::service {
InputRouter::InputRouter(const WindowRegistry& registry, Dispatch dispatch)
    : registry_(registry), dispatch_(std::move(dispatch)) {}
bool InputRouter::Send(const WindowId& id, const Input& input) {
    std::lock_guard<std::mutex> lock(mutex_);
    WindowId target = id;
    if (input.kind == Input::Kind::Key) {
        auto key = keys_.find(input.key.evdevCode);
        if (key != keys_.end()) target = key->second;
        else if (!input.key.pressed) return false;
    } else if (input.kind == Input::Kind::Pointer) {
        auto button = buttons_.find(input.pointer.button);
        if (input.pointer.action == PointerAction::Release) {
            if (button == buttons_.end()) return false;
            target = button->second;
        } else if (!buttons_.empty()) target = buttons_.begin()->second;
    }
    auto window = registry_.Find(target);
    const bool ok = window && dispatch_(*window, input);
    if (input.kind == Input::Kind::Key) {
        if (!input.key.pressed) keys_.erase(input.key.evdevCode);
        else if (ok) keys_[input.key.evdevCode] = target;
    } else if (input.kind == Input::Kind::Pointer) {
        if (input.pointer.action == PointerAction::Release) buttons_.erase(input.pointer.button);
        else if (input.pointer.action == PointerAction::Press && ok) buttons_[input.pointer.button] = target;
    }
    return ok;
}
void InputRouter::Reset() {
    std::lock_guard<std::mutex> lock(mutex_); keys_.clear(); buttons_.clear();
}
}
