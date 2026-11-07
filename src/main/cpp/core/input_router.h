#pragma once
#include <functional>
#include <map>
#include <mutex>
#include "window_registry.h"
namespace hgs::service {
// Key/button releases follow their press owner. A stale ID never falls back to
// another backend. Backend dispatch only queues work, never re-enters this router.
class InputRouter {
public:
    using Dispatch = std::function<bool(const WindowSnapshot&, const Input&)>;
    InputRouter(const WindowRegistry& registry, Dispatch dispatch);
    bool Send(const WindowId& window, const Input& input);
    void Reset();
private:
    const WindowRegistry& registry_;
    Dispatch dispatch_;
    std::mutex mutex_;
    std::map<int32_t, WindowId> keys_;
    std::map<uint32_t, WindowId> buttons_;
};
}
