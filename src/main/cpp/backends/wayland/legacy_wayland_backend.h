#pragma once
#include <memory>
#include "backends/backend.h"
namespace hgs::service {
class LegacyWaylandBackend final : public Backend {
public:
    LegacyWaylandBackend();
    ~LegacyWaylandBackend() override;
    Protocol GetProtocol() const override { return Protocol::Wayland; }
    std::string Id() const override { return "legacy-wayland"; }
    bool Start(const Config&, Epoch, Sink, Endpoint&, Error&) override;
    void Stop() override;
    bool Send(const WindowSnapshot&, const Input&) override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
