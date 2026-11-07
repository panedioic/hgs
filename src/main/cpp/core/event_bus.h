#pragma once
#include <functional>
#include <atomic>
#include <map>
#include <mutex>
#include "include/hgs/service_types.h"
namespace hgs::service {
// Synchronous, serialized by the runtime's event producer. Callbacks run without
// this mutex, may unsubscribe, and must not synchronously call Start/Stop.
class EventBus {
public:
    using Listener = std::function<void(const Event&)>;
    uint64_t Subscribe(Listener listener);
    void Unsubscribe(uint64_t token);
    void Publish(const Event& event) const;
    uint64_t FailedDeliveries() const { return failures_.load(); }
private:
    mutable std::mutex mutex_;
    mutable std::atomic<uint64_t> failures_{0};
    uint64_t nextToken_ = 0;
    std::map<uint64_t, Listener> listeners_;
};
}
