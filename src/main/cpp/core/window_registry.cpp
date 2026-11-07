#include "window_registry.h"
#include <limits>
namespace hgs::service {
WindowRegistry::Key WindowRegistry::Identity(const WindowOrigin& o) {
    return {o.epoch, o.backendId, o.protocol, o.clientId, o.nativeId, o.objectGeneration};
}
void WindowRegistry::Begin(Epoch epoch) {
    std::lock_guard<std::mutex> lock(mutex_);
    epoch_ = epoch;
    windows_.clear(); identities_.clear();
    nextId_ = 0;
}
std::optional<WindowSnapshot> WindowRegistry::Create(WindowSnapshot w) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (epoch_ == 0 || w.origin.epoch != epoch_ || w.origin.backendId.empty() ||
        w.origin.clientId == 0 || w.origin.nativeId == 0 || w.origin.objectGeneration == 0 ||
        nextId_ == std::numeric_limits<uint64_t>::max()) return {};
    const auto key = Identity(w.origin);
    auto found = identities_.find(key);
    if (found != identities_.end()) return windows_.at(found->second);
    w.id = "e" + std::to_string(epoch_) + "-w" + std::to_string(++nextId_);
    identities_[key] = w.id;
    windows_[w.id] = w;
    return w;
}
std::optional<WindowSnapshot> WindowRegistry::Update(const WindowSnapshot& w) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = identities_.find(Identity(w.origin));
    if (it == identities_.end()) return {};
    auto updated = w;
    updated.id = it->second;
    windows_[updated.id] = updated;
    return updated;
}
std::optional<WindowSnapshot> WindowRegistry::Remove(const WindowOrigin& o) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = identities_.find(Identity(o));
    if (it == identities_.end()) return {};
    auto copy = windows_.at(it->second);
    windows_.erase(it->second); identities_.erase(it);
    return copy;
}
std::optional<WindowSnapshot> WindowRegistry::Find(const WindowId& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = windows_.find(id);
    return it == windows_.end() ? std::optional<WindowSnapshot>{} : it->second;
}
std::optional<WindowSnapshot> WindowRegistry::Find(const WindowOrigin& o) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = identities_.find(Identity(o));
    return it == identities_.end() ? std::optional<WindowSnapshot>{} : windows_.at(it->second);
}
std::vector<WindowSnapshot> WindowRegistry::List() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<WindowSnapshot> result;
    for (const auto& item : windows_) result.push_back(item.second);
    return result;
}
void WindowRegistry::Clear() { Begin(0); }
}
