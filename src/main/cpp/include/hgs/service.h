#pragma once
#include <memory>
#include "service_types.h"
#include <functional>
namespace hgs::service {
class Backend;
class HGS_API Runtime {
public:
    explicit Runtime(std::vector<std::unique_ptr<Backend>> backends);
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    StartResult Start(const Config& config);
    void Stop();
    Snapshot Inspect() const;
    ClientEnvironment Environment(Protocol protocol, Error& error, const std::string& expectedEpoch = {}) const;
    std::vector<WindowSnapshot> Windows(const std::string& expectedEpoch = {}) const;
    bool Send(const WindowId& window, const Input& input, const std::string& expectedEpoch = {});
    // Queues an OHOS Surface binding on backends that support it. Detach only
    // removes a binding with the same surface ID.
    bool BindSurface(const WindowId& window, uint64_t surfaceId, bool attach, const std::string& expectedEpoch);
    uint64_t Subscribe(std::function<void(const Event&)> listener);
    void Unsubscribe(uint64_t token);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// One runtime per process. v1 startServer uses the same instance.
HGS_API Runtime& DefaultRuntime();
}
