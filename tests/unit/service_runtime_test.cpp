#include "include/hgs/service.h"
#include "backends/backend.h"
#include "core/window_registry.h"
#include "core/event_bus.h"
#include "core/input_router.h"
#include <cassert>
#include <cstdio>
using namespace hgs;
using namespace hgs::service;
struct Fake final : Backend {
    Protocol protocol;
    int starts = 0, stops = 0;
    bool fail = false;
    Sink sink;
    Epoch epoch = 0;
    std::vector<WindowSnapshot> inputs;
    explicit Fake(Protocol p) : protocol(p) {}
    Protocol GetProtocol() const override { return protocol; }
    std::string Id() const override { return protocol == Protocol::Wayland ? "wl" : "x"; }
    bool Start(const Config&, Epoch e, Sink s, Endpoint& ep, Error& error) override {
        ++starts; epoch = e; sink = std::move(s);
        ep.protocol = protocol; ep.backendId = Id(); ep.runtimeDir = "/tmp/hgs";
        ep.address = protocol == Protocol::Wayland ? "wl-actual" : ":7";
        ep.logicalOutputReady = true;
        if (fail) { error = {ErrorCode::BackendFailed, "fake", "injected failure"}; return false; }
        return true;
    }
    void Stop() override { ++stops; }
    bool Send(const WindowSnapshot& w, const Input&) override { inputs.push_back(w); return true; }
    Event Create(uint64_t client, uint64_t native, uint64_t generation, std::string title) {
        Event e; e.kind = EventKind::Created;
        e.window.origin = {epoch, Id(), protocol, client, native, generation};
        e.window.clientPid = 42; e.window.title = std::move(title);
        sink(e); return e;
    }
};
int main() {
    {
        WindowRegistry registry;
        registry.Begin(1);
        WindowSnapshot w;
        w.origin = {1, "wl", Protocol::Wayland, 10, 5, 1};
        auto one = registry.Create(w); assert(one);
        w.origin.clientId = 11;
        auto two = registry.Create(w); assert(two && two->id != one->id);
        assert(registry.Create(w)->id == two->id);
        w.origin.objectGeneration = 2;
        auto three = registry.Create(w); assert(three && three->id != two->id);
        assert(registry.Remove(two->origin));
        assert(!registry.Find(two->id)); assert(registry.Find(three->id));
        registry.Begin(2); assert(!registry.Create(w)); assert(!registry.Find(one->id));
    }
    auto wl = std::make_unique<Fake>(Protocol::Wayland); auto* backend = wl.get();
    std::vector<std::unique_ptr<Backend>> backends; backends.push_back(std::move(wl));
    Runtime runtime(std::move(backends));
    Config config; config.runtimeDir = "/tmp/hgs";
    Error error;
    assert(runtime.Environment(Protocol::Wayland, error).set.empty()); assert(error.code == ErrorCode::NotReady);
    auto unsupported = config; unsupported.protocols = Protocols::Both;
    assert(runtime.Start(unsupported).error.code == ErrorCode::UnsupportedProtocol); assert(backend->starts == 0);
    auto invalid = config; invalid.output.width = 1;
    assert(runtime.Start(invalid).error.code == ErrorCode::InvalidConfig && backend->starts == 0);
    auto result = runtime.Start(config); assert(result.ok && result.session.state == State::Ready);
    auto epoch = result.session.epoch;
    assert(runtime.Start(config).ok && backend->starts == 1);
    auto conflict = config; conflict.inputBackend = InputBackend::Native;
    assert(runtime.Start(conflict).error.code == ErrorCode::ConfigConflict);
    assert(runtime.Inspect().state == State::Ready);
    auto env = runtime.Environment(Protocol::Wayland, error);
    assert(error.code == ErrorCode::None && env.set[1].value == "wl-actual");
    assert(env.unset[0] == "DISPLAY" && env.unset[1] == "WAYLAND_SOCKET");
    assert(!config.setProcessEnvironment);
    std::vector<Event> received;
    auto subscription = runtime.Subscribe([&](const Event& e) { received.push_back(e); assert(runtime.Inspect().epoch == e.epoch); });
    auto event = backend->Create(1, 9, 1, "owned title");
    event.window.title = "changed local storage";
    assert(received.back().window.title == "owned title");
    auto firstId = received.back().window.id;
    auto staleSink = backend->sink;
    auto other = backend->Create(2, 9, 1, "same PID, different connection");
    auto secondId = received.back().window.id; assert(firstId != secondId);
    Input input; input.kind = Input::Kind::Key; input.key.evdevCode = 30; input.key.pressed = true;
    auto invalidInput = input; invalidInput.key.evdevCode = -1;
    assert(!runtime.Send(firstId, invalidInput));
    assert(runtime.Send(firstId, input));
    input.key.pressed = false;
    assert(runtime.Send(secondId, input)); assert(backend->inputs.back().id == firstId);
    assert(!runtime.Send("unknown", input));
    runtime.Stop(); assert(runtime.Inspect().state == State::Stopped && runtime.Inspect().endpoints.empty());
    assert(runtime.Windows().empty()); assert(!runtime.Send(firstId, input));
    auto count = received.size(); runtime.Stop(); assert(count == received.size());
    assert(runtime.Environment(Protocol::Wayland, error).set.empty());
    assert(runtime.Start(config).ok && runtime.Inspect().epoch > epoch);
    assert(runtime.Environment(Protocol::Wayland, error, std::to_string(epoch)).set.empty());
    assert(error.code == ErrorCode::NotReady);
    count = received.size(); staleSink(other); assert(received.size() == count && runtime.Windows().empty());
    backend->Create(1, 9, 1, "new session"); assert(received.back().window.id != firstId);
    assert(runtime.Windows(std::to_string(epoch)).empty());
    assert(!runtime.Send(received.back().window.id, input, std::to_string(epoch)));
    runtime.Unsubscribe(subscription);
    runtime.Stop();
    {
        auto a = std::make_unique<Fake>(Protocol::Wayland); auto* ap = a.get();
        auto b = std::make_unique<Fake>(Protocol::X11); auto* bp = b.get(); b->fail = true;
        std::vector<std::unique_ptr<Backend>> both; both.push_back(std::move(a)); both.push_back(std::move(b));
        Runtime atomic(std::move(both));
        config.protocols = Protocols::Both;
        assert(!atomic.Start(config).ok);
        assert(ap->stops == 1 && bp->stops == 1 && atomic.Inspect().endpoints.empty());
        bp->fail = false; assert(atomic.Start(config).ok);
        ap->Create(1, 8, 1, "wayland"); bp->Create(1, 8, 1, "x11");
        assert(atomic.Windows().size() == 2);
        for (const auto& w : atomic.Windows()) {
            input.key.pressed = true; input.key.evdevCode = w.origin.protocol == Protocol::Wayland ? 30 : 31;
            assert(atomic.Send(w.id, input));
        }
        assert(ap->inputs.size() == 1 && bp->inputs.size() == 1);
    }
    {
        EventBus bus; int calls = 0; uint64_t token = 0;
        token = bus.Subscribe([&](const Event&) { ++calls; bus.Unsubscribe(token); });
        bus.Publish({}); bus.Publish({}); assert(calls == 1);
        bus.Subscribe([](const Event&) { throw 1; });
        bus.Subscribe([&](const Event&) { ++calls; });
        bus.Publish({}); assert(calls == 2 && bus.FailedDeliveries() == 1);
    }
    puts("service_runtime_test: identity, epoch, rollback, environment, input ownership and event lifetime PASS");
}
