#include "event_bus.h"
#include <limits>
namespace hgs::service {
uint64_t EventBus::Subscribe(Listener listener) {
    if (!listener) return 0;
    std::lock_guard<std::mutex> lock(mutex_);
    if (nextToken_ == std::numeric_limits<uint64_t>::max()) return 0;
    listeners_[++nextToken_] = std::move(listener);
    return nextToken_;
}
void EventBus::Unsubscribe(uint64_t token) {
    std::lock_guard<std::mutex> lock(mutex_); listeners_.erase(token);
}
void EventBus::Publish(const Event& event) const {
    std::vector<Listener> snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& pair : listeners_) snapshot.push_back(pair.second);
    }
    for (const auto& listener : snapshot) {
        try { listener(event); } catch (...) { ++failures_; }
    }
}
}
