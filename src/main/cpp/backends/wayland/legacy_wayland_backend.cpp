#include "legacy_wayland_backend.h"
#include "api/hgs_instance.h"
#include "api/hgs_endpoint.h"
#include "compositor/wayland_server.h"
#include "compositor/frame/surface_data.h"
#include <map>
#include <utility>

namespace hgs::service {
struct LegacyWaylandBackend::Impl {
    struct Client {
        wl_listener listener{}; // first field: recover the record from notify
        Impl* owner = nullptr;
        wl_client* native = nullptr;
        uint64_t id = 0;
    };
    Epoch epoch = 0;
    uint64_t nextClient = 0, nextGeneration = 0;
    Sink sink;
    std::map<wl_client*, std::unique_ptr<Client>> clients;
    std::map<uint32_t, WindowSnapshot> windows;
    static void ClientGone(wl_listener* listener, void*) {
        auto* record = reinterpret_cast<Client*>(listener);
        auto* owner = record->owner;
        auto* native = record->native;
        wl_list_remove(&record->listener.link);
        owner->clients.erase(native);
    }
    uint64_t Connection(wl_client* client) {
        auto it = clients.find(client);
        if (it != clients.end()) return it->second->id;
        auto c = std::make_unique<Client>();
        c->owner = this; c->native = client; c->id = ++nextClient;
        c->listener.notify = ClientGone;
        wl_client_add_destroy_listener(client, &c->listener);
        uint64_t id = c->id;
        clients.emplace(client, std::move(c));
        return id;
    }
    void Observe(uint32_t id, ToplevelEventType type, const char* detail) {
        auto old = windows.find(id);
        if (type == ToplevelEventType::Destroyed) {
            if (old == windows.end()) return;
            Event e; e.kind = EventKind::Destroyed; e.window = old->second;
            e.detail = detail ? detail : "{}";
            windows.erase(old);
            sink(std::move(e));
            return;
        }
        // v1 popup notifications address their parent; preserve them as owned
        // change snapshots without pretending that the popup has that identity.
        wl_resource* resource = WaylandServer::GetInstance()->GetToplevelManager().GetSurfaceForToplevel(id);
        if (!resource) return;
        auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(resource));
        if (!sd) return;
        if (old == windows.end()) {
            if (type != ToplevelEventType::Created) return;
            WindowSnapshot w;
            w.legacyId = id;
            w.origin = {epoch, "legacy-wayland", Protocol::Wayland,
                        Connection(wl_resource_get_client(resource)), wl_resource_get_id(resource), ++nextGeneration};
            old = windows.emplace(id, std::move(w)).first;
        }
        auto& w = old->second;
        w.clientPid = sd->clientPid; w.title = sd->title; w.appId = sd->appId;
        w.x = sd->committed.screenX; w.y = sd->committed.screenY; w.width = sd->w; w.height = sd->h;
        w.minWidth = sd->minWidth; w.minHeight = sd->minHeight;
        w.maxWidth = sd->maxWidth; w.maxHeight = sd->maxHeight;
        switch (type) {
            case ToplevelEventType::Minimized: w.minimized = true; w.visible = false; break;
            case ToplevelEventType::Maximized: w.maximized = true; break;
            case ToplevelEventType::Fullscreen: w.fullscreen = true; break;
            case ToplevelEventType::Restored: w.minimized = false; w.visible = true; break;
            case ToplevelEventType::Unmaximized: w.maximized = false; break;
            case ToplevelEventType::Unfullscreen: w.fullscreen = false; break;
            default: break;
        }
        Event e; e.kind = type == ToplevelEventType::Created ? EventKind::Created : EventKind::Changed;
        e.window = w;
        e.detail = std::string(ToplevelEventName(type)) + ":" + (detail ? detail : "{}");
        sink(std::move(e));
    }
};
LegacyWaylandBackend::LegacyWaylandBackend() : impl_(std::make_unique<Impl>()) {}
LegacyWaylandBackend::~LegacyWaylandBackend() = default;
bool LegacyWaylandBackend::Start(const Config& config, Epoch epoch, Sink sink, Endpoint& ep, Error& error) {
    impl_->epoch = epoch; impl_->sink = std::move(sink);
    auto* p = impl_.get();
    auto& legacy = HgsInstance::Instance();
    legacy.serviceObserver_ = [p](uint32_t id, ToplevelEventType type, const char* json) { p->Observe(id, type, json); };
    HgsConfig options;
    options.socketPath = config.runtimeDir + (config.runtimeDir == "/" ? "" : "/") + config.waylandSocket;
    options.output = config.output;
    options.composition = config.composition;
    options.inputBackend = config.inputBackend;
    options.setProcessEnvironment = config.setProcessEnvironment;
    options.features = config.legacyWaylandFeatures;
    if (!legacy.StartBackend(options)) {
        error = {ErrorCode::BackendFailed, "wayland-listener", legacy.LastError()};
        return false;
    }
    auto endpoint = SplitSocketPath(options.socketPath);
    ep.protocol = Protocol::Wayland; ep.backendId = Id();
    ep.runtimeDir = endpoint.directory; ep.address = endpoint.displayName; ep.socketPath = options.socketPath;
    auto* ws = WaylandServer::GetInstance();
    ep.outputWidth = ws->OutputWidth(); ep.outputHeight = ws->OutputHeight();
    ep.logicalOutputReady = ep.outputWidth > 0 && ep.outputHeight > 0;
    return true;
}
void LegacyWaylandBackend::Stop() {
    HgsInstance::Instance().StopBackend();
    HgsInstance::Instance().serviceObserver_ = {};
    for (auto& item : impl_->clients) wl_list_remove(&item.second->listener.link);
    impl_->clients.clear(); impl_->windows.clear(); impl_->sink = {};
}
bool LegacyWaylandBackend::Send(const WindowSnapshot& w, const Input& input) {
    if (w.origin.epoch != impl_->epoch || w.origin.backendId != Id()) return false;
    auto& legacy = HgsInstance::Instance();
    switch (input.kind) {
        case Input::Kind::Pointer: {
            auto e = input.pointer; e.hintToplevel = w.legacyId; legacy.SendPointerEvent(e); break;
        }
        case Input::Kind::Key: {
            auto e = input.key; e.toplevelId = w.legacyId; legacy.SendKeyEvent(e); break;
        }
        case Input::Kind::Scroll: {
            auto e = input.scroll; e.hintToplevel = w.legacyId; legacy.SendScrollEvent(e); break;
        }
    }
    return true;
}
}
