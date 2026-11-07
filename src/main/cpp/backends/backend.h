#pragma once
#include <functional>
#include "include/hgs/service_types.h"
namespace hgs::service {
class Backend {
public:
    using Sink = std::function<void(Event)>;
    virtual ~Backend() = default;
    virtual Protocol GetProtocol() const = 0;
    virtual std::string Id() const = 0;
    // Start returns only after a listening endpoint and logical output exist.
    virtual bool Start(const Config& config, Epoch epoch, Sink sink, Endpoint& endpoint, Error& error) = 0;
    // Stop joins the loop, drains callbacks and cleans owned endpoints.
    virtual void Stop() = 0;
    virtual bool Send(const WindowSnapshot& window, const Input& input) = 0;
    virtual bool BindSurface(const WindowSnapshot&, uint64_t, bool) { return false; }
};
}
