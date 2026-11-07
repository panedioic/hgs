#include "include/hgs/service.h"
#include "backends/backend.h"
#include "core/event_bus.h"
#include "core/window_registry.h"
#include "core/input_router.h"
#include "api/hgs_endpoint.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <tuple>

namespace hgs::service {
namespace {
bool SameConfig(const Config& a, const Config& b) {
    const auto features = [](const FeatureConfig& f) {
        return std::make_tuple(f.shm, f.subsurface, f.viewporter, f.xdgPopup, f.dataDevice,
                              f.relativePointer, f.textInput, f.externalSurface, f.winehuaToplevel);
    };
    const auto values = [](const Config& c) {
        const auto& w = c;
        return std::make_tuple(c.protocols, c.runtimeDir, c.waylandSocket,
            w.output.width, w.output.height, w.output.refreshMilliHz, w.output.scale,
            w.composition, w.inputBackend, w.setProcessEnvironment);
    };
    return values(a) == values(b) && features(a.legacyWaylandFeatures) == features(b.legacyWaylandFeatures);
}
bool Normalize(Config& c) {
    if (c.protocols != Protocols::Wayland && c.protocols != Protocols::X11 && c.protocols != Protocols::Both) return false;
    auto& w = c;
    if (c.runtimeDir.empty() || c.runtimeDir.front() != '/' || c.runtimeDir.find('\0') != std::string::npos) return false;
    while (c.runtimeDir.size() > 1 && c.runtimeDir.back() == '/') c.runtimeDir.pop_back();
    if (c.protocols != Protocols::X11) {
        if (c.waylandSocket.empty() || c.waylandSocket.find('/') != std::string::npos) return false;
        const auto path = c.runtimeDir + (c.runtimeDir == "/" ? "" : "/") + c.waylandSocket;
        auto ep = SplitSocketPath(path);
        if (!ep.valid) return false;
        c.runtimeDir = ep.directory;
        c.waylandSocket = ep.displayName;
    }
    const auto& o = w.output;
    return ((o.width == 0 && o.height == 0) || (o.width > 0 && o.height > 0)) &&
           std::isfinite(o.scale) && o.scale >= 0 && o.refreshMilliHz > 0 &&
           static_cast<unsigned>(w.composition) <= static_cast<unsigned>(CompositionMode::DesktopRoot) &&
           static_cast<unsigned>(w.inputBackend) <= static_cast<unsigned>(InputBackend::Native);
}
}
struct Runtime::Impl {
    mutable std::mutex mutex;
    std::mutex operations;
    std::mutex delivery;
    Snapshot snapshot;
    Config config;
    std::vector<std::unique_ptr<Backend>> backends;
    std::vector<Backend*> active;
    WindowRegistry registry;
    EventBus events;
    uint64_t nextSerial = 0;
    InputRouter input;
    explicit Impl(std::vector<std::unique_ptr<Backend>> b)
        : backends(std::move(b)), input(registry, [this](const WindowSnapshot& w, const Input& i) {
            for (auto* backend : active) if (backend->Id() == w.origin.backendId && backend->GetProtocol() == w.origin.protocol) return backend->Send(w, i);
            return false;
        }) {
        for (const auto& backend : backends) {
            if (backend->GetProtocol() == Protocol::Wayland) snapshot.capabilities.wayland = true;
            else snapshot.capabilities.x11 = true;
        }
    }
    Event Stamp(Event e) {
        std::lock_guard<std::mutex> lock(mutex);
        e.epoch = snapshot.epoch;
        e.serial = ++nextSerial;
        e.state = snapshot.state;
        return e;
    }
    void StateEvent() { std::lock_guard<std::mutex> serial(delivery); Event event; event.origin = "runtime"; events.Publish(Stamp(std::move(event))); }
    void Observe(Epoch epoch, const std::string& backendId, Protocol protocol, Event event) {
        std::lock_guard<std::mutex> serial(delivery);
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (epoch != snapshot.epoch ||
                (snapshot.state != State::Starting && snapshot.state != State::Ready &&
                 !(snapshot.state == State::Stopping && event.kind == EventKind::Destroyed))) return;
            auto& origin = event.window.origin;
            // A backend cannot publish another backend's identity.
            if (origin.epoch != epoch || origin.backendId != backendId || origin.protocol != protocol) return;
            std::optional<WindowSnapshot> window;
            switch (event.kind) {
                case EventKind::Created: window = registry.Create(event.window); break;
                case EventKind::Changed: window = registry.Update(event.window); break;
                case EventKind::Destroyed: window = registry.Remove(origin); break;
                case EventKind::CloseRequested: window = registry.Find(origin); break;
                default: return;
            }
            if (!window) return;
            event.window = *window;
            event.epoch = epoch;
            event.state = snapshot.state;
            event.serial = ++nextSerial;
        }
        events.Publish(event);
    }
    void ForgetWindows() {
        for (const auto& w : registry.List()) {
            if (!registry.Remove(w.origin)) continue;
            Event e; e.kind = EventKind::Destroyed; e.window = w; e.origin = "runtime";
            std::lock_guard<std::mutex> serial(delivery);
            events.Publish(Stamp(std::move(e)));
        }
        registry.Clear();
    }
};
Runtime::Runtime(std::vector<std::unique_ptr<Backend>> b) : impl_(std::make_unique<Impl>(std::move(b))) {}
Runtime::~Runtime() { Stop(); }
StartResult Runtime::Start(const Config& requested) {
    auto& p = *impl_;
    std::lock_guard<std::mutex> operation(p.operations);
    Config c = requested;
    const bool valid = Normalize(c);
    std::vector<Backend*> selected;
    Error error;
    {
        std::lock_guard<std::mutex> lock(p.mutex);
        if (p.snapshot.state == State::Ready) {
            if (valid && SameConfig(c, p.config)) return {true, p.snapshot, {}};
            return {false, p.snapshot, {ErrorCode::ConfigConflict, "config", "stop the active runtime before changing configuration"}};
        }
        if (!valid) error = {ErrorCode::InvalidConfig, "config", "invalid endpoint, output, protocol or input configuration"};
        else {
            for (auto protocol : {Protocol::Wayland, Protocol::X11}) {
                if ((c.protocols == Protocols::Wayland && protocol == Protocol::X11) ||
                    (c.protocols == Protocols::X11 && protocol == Protocol::Wayland)) continue;
                auto it = std::find_if(p.backends.begin(), p.backends.end(), [protocol](const auto& b) {
                    return b->GetProtocol() == protocol;
                });
                if (it == p.backends.end()) { error = {ErrorCode::UnsupportedProtocol, "capabilities", "requested protocol was not built into HGS"}; break; }
                selected.push_back(it->get());
            }
        }
        if (error.code == ErrorCode::None && p.snapshot.epoch == std::numeric_limits<Epoch>::max())
            error = {ErrorCode::BackendFailed, "epoch", "runtime epoch exhausted"};
        p.snapshot.lastError = error;
        p.snapshot.endpoints.clear();
        p.snapshot.state = error.code == ErrorCode::None ? State::Starting : State::Failed;
        if (error.code == ErrorCode::None) { ++p.snapshot.epoch; p.config = c; p.registry.Begin(p.snapshot.epoch); }
    }
    p.StateEvent();
    if (error.code != ErrorCode::None) return {false, Inspect(), error};
    std::vector<Endpoint> endpoints;
    const Epoch epoch = Inspect().epoch;
    for (auto* backend : selected) {
        // active is written only under the control mutex; Send holds that mutex.
        { std::lock_guard<std::mutex> lock(p.mutex); p.active.push_back(backend); }
        Endpoint endpoint;
        auto sink = [&p, epoch, id = backend->Id(), protocol = backend->GetProtocol()](Event e) { p.Observe(epoch, id, protocol, std::move(e)); };
        bool ok = backend->Start(c, epoch, std::move(sink), endpoint, error);
        if (!ok || !endpoint.logicalOutputReady || endpoint.address.empty() ||
            endpoint.protocol != backend->GetProtocol() || endpoint.backendId != backend->Id()) {
            if (error.code == ErrorCode::None) error = {ErrorCode::BackendFailed, "endpoint", "backend did not publish a ready endpoint and logical output"};
            { std::lock_guard<std::mutex> lock(p.mutex); p.snapshot.state = State::Stopping; }
            p.StateEvent();
            for (auto it = p.active.rbegin(); it != p.active.rend(); ++it) (*it)->Stop();
            p.input.Reset(); p.ForgetWindows();
            {
                std::lock_guard<std::mutex> lock(p.mutex);
                p.active.clear(); p.snapshot.state = State::Failed; p.snapshot.lastError = error;
            }
            p.StateEvent();
            return {false, Inspect(), error};
        }
        endpoints.push_back(std::move(endpoint));
    }
    {
        std::lock_guard<std::mutex> lock(p.mutex);
        p.snapshot.endpoints = std::move(endpoints); p.snapshot.state = State::Ready;
    }
    p.StateEvent();
    return {true, Inspect(), {}};
}
void Runtime::Stop() {
    auto& p = *impl_;
    std::lock_guard<std::mutex> operation(p.operations);
    {
        std::lock_guard<std::mutex> lock(p.mutex);
        if (p.snapshot.state == State::Stopped) return;
        p.snapshot.state = State::Stopping;
    }
    p.StateEvent();
    for (auto it = p.active.rbegin(); it != p.active.rend(); ++it) (*it)->Stop();
    p.input.Reset(); p.ForgetWindows();
    {
        std::lock_guard<std::mutex> lock(p.mutex);
        p.active.clear(); p.snapshot.endpoints.clear(); p.snapshot.state = State::Stopped;
    }
    p.StateEvent();
}
Snapshot Runtime::Inspect() const { std::lock_guard<std::mutex> lock(impl_->mutex); return impl_->snapshot; }
ClientEnvironment Runtime::Environment(Protocol protocol, Error& error, const std::string& expectedEpoch) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    error = {};
    if (!expectedEpoch.empty() && expectedEpoch != std::to_string(impl_->snapshot.epoch)) {
        error = {ErrorCode::NotReady, "epoch", "session is stale"}; return {};
    }
    if (impl_->snapshot.state == State::Ready) for (const auto& ep : impl_->snapshot.endpoints) {
        if (ep.protocol != protocol) continue;
        if (protocol == Protocol::Wayland) return {{{"XDG_RUNTIME_DIR", ep.runtimeDir}, {"WAYLAND_DISPLAY", ep.address}}, {"DISPLAY", "WAYLAND_SOCKET"}, {}};
        // X11 backend adds actual authentication / XIM variables in P2.
        return {{{"DISPLAY", ep.address}}, {"WAYLAND_DISPLAY", "WAYLAND_SOCKET", "XAUTHORITY", "XMODIFIERS"}, {}};
    }
    error = {ErrorCode::NotReady, "environment", "requested protocol has no ready endpoint"};
    return {};
}
std::vector<WindowSnapshot> Runtime::Windows(const std::string& expectedEpoch) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!expectedEpoch.empty() && (expectedEpoch != std::to_string(impl_->snapshot.epoch) || impl_->snapshot.state != State::Ready)) return {};
    return impl_->registry.List();
}
bool Runtime::Send(const WindowId& window, const Input& input, const std::string& expectedEpoch) {
    const auto finite = [](double value) { return std::isfinite(value); };
    switch (input.kind) {
        case Input::Kind::Key:
            if (input.key.evdevCode < 0 || input.key.evdevCode > 0x2ff) return false;
            break;
        case Input::Kind::Pointer:
            if (input.pointer.action < PointerAction::Press || input.pointer.action > PointerAction::Move ||
                !finite(input.pointer.x) || !finite(input.pointer.y) || !finite(input.pointer.rawDeltaX) || !finite(input.pointer.rawDeltaY)) return false;
            break;
        case Input::Kind::Scroll:
            if (input.scroll.axis < 0 || input.scroll.axis > 1 || !finite(input.scroll.value) || !finite(input.scroll.x) || !finite(input.scroll.y)) return false;
            break;
        default: return false;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->snapshot.state == State::Ready && (expectedEpoch.empty() || expectedEpoch == std::to_string(impl_->snapshot.epoch)) && impl_->input.Send(window, input);
}
uint64_t Runtime::Subscribe(std::function<void(const Event&)> listener) { return impl_->events.Subscribe(std::move(listener)); }
void Runtime::Unsubscribe(uint64_t token) { impl_->events.Unsubscribe(token); }
}
