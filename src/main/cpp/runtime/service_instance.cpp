#include "include/hgs/service.h"
#include "include/hgs/external_frames.h"
#include "backends/wayland/legacy_wayland_backend.h"
#ifdef HGS_BUILD_X11
#include "backends/xwayland/xwayland_backend.h"
#endif
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
#ifdef HGS_BUILD_X11
        backends.push_back(std::make_unique<XwaylandBackend>());
#endif
        return backends;
    }());
    return runtime;
}
}

extern "C" HGS_API bool Hgs_setExternalFrameProvider(hgs::service::ExternalFrameProvider* provider) {
    return hgs::service::DefaultRuntime().SetExternalFrameProvider(provider);
}
