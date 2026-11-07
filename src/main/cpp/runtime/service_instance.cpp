#include "include/hgs/service.h"
#include "backends/wayland/legacy_wayland_backend.h"
#include "api/hgs_instance.h"
#include "compositor/wayland_server.h"
namespace hgs::service {
Runtime& DefaultRuntime() {
    // Construct dependencies before runtime so destruction drains the runtime
    // while the legacy server and facade are still alive.
    HgsInstance::Instance();
    WaylandServer::GetInstance();
    static Runtime runtime([] {
        std::vector<std::unique_ptr<Backend>> backends;
        backends.push_back(std::make_unique<LegacyWaylandBackend>());
        return backends;
    }());
    return runtime;
}
}
