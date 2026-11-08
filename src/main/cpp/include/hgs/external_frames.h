#pragma once

#include "service_types.h"

struct NativeWindow;

namespace hgs::service {
struct ExternalFrameTarget {
    uint64_t token = 0;
    Protocol protocol = Protocol::X11;
    uint64_t nativeId = 0;
    uint32_t clientPid = 0;
    uint32_t width = 0, height = 0;
    bool attached = false;
    bool vulkan = false;
};

// Calls run on the presentation worker. Unbind must revoke readiness and
// release the provider's reference before the consumer is destroyed.
class ExternalFrameProvider {
public:
    virtual ~ExternalFrameProvider() = default;
    virtual bool Query(std::vector<ExternalFrameTarget>& targets) = 0;
    virtual bool Bind(const ExternalFrameTarget& target, NativeWindow* producer, uint64_t periodNs) = 0;
    virtual void Unbind(const ExternalFrameTarget& target) = 0;
};
}

// Register or clear while stopped. The caller retains the provider until it
// clears the registration after the runtime has stopped.
extern "C" HGS_API bool Hgs_setExternalFrameProvider(hgs::service::ExternalFrameProvider* provider);
