#pragma once
#include <map>
#include <mutex>
#include <optional>
#include <tuple>
#include "include/hgs/service_types.h"
namespace hgs::service {
class WindowRegistry {
public:
    void Begin(Epoch epoch);
    std::optional<WindowSnapshot> Create(WindowSnapshot window);
    std::optional<WindowSnapshot> Update(const WindowSnapshot& window);
    std::optional<WindowSnapshot> Remove(const WindowOrigin& origin);
    std::optional<WindowSnapshot> Find(const WindowId& id) const;
    std::optional<WindowSnapshot> Find(const WindowOrigin& origin) const;
    std::vector<WindowSnapshot> List() const;
    void Clear();
private:
    using Key = std::tuple<Epoch, std::string, Protocol, uint64_t, uint64_t, uint64_t>;
    static Key Identity(const WindowOrigin& origin);
    mutable std::mutex mutex_;
    Epoch epoch_ = 0;
    uint64_t nextId_ = 0;
    std::map<WindowId, WindowSnapshot> windows_;
    std::map<Key, WindowId> identities_;
};
}
