#pragma once

#include "backends/backend.h"
#include <memory>

namespace hgs::service {
class XwaylandBackend final : public Backend {
public:
    XwaylandBackend();
    ~XwaylandBackend() override;
    Protocol GetProtocol() const override { return Protocol::X11; }
    std::string Id() const override { return "xwayland"; }
    bool Start(const Config& config, Epoch epoch, Sink sink, Endpoint& endpoint, Error& error) override;
    void Stop() override;
    bool Send(const WindowSnapshot& window, const Input& input) override;
    bool BindSurface(const WindowSnapshot& window, uint64_t surfaceId, bool attach) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
