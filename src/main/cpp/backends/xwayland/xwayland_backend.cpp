#include "xwayland_backend.h"
#include "runtime/session.h"
#include "runtime/x11_endpoint.h"
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <sys/eventfd.h>
#include <thread>
#include <unistd.h>

namespace hgs::service {
struct XwaylandBackend::Impl {
    struct Command {
        uint32_t xid;
        uint64_t generation;
        Input input;
    };

    std::unique_ptr<X11Endpoint> endpoint;
    std::thread worker;
    std::mutex mutex;
    std::condition_variable startup;
    std::atomic<bool> stopping{false};
    bool started = false, startupFinished = false;
    Error error;
    std::deque<Command> commands;
    Epoch epoch = 0;
    Sink sink;
    int wake = -1;

    static void Observe(void* context, hgs_x11_event_kind kind, const hgs_x11_window* source) {
        const auto& self = *static_cast<Impl*>(context);
        Event event;
        switch (kind) {
            case HGS_X11_MAPPED: event.kind = EventKind::Created; break;
            case HGS_X11_UPDATED: event.kind = EventKind::Changed; break;
            case HGS_X11_UNMAPPED: event.kind = EventKind::Destroyed; break;
            case HGS_X11_CLOSE_REQUESTED: event.kind = EventKind::CloseRequested; break;
        }
        auto& w = event.window;
        w.origin = {self.epoch, "xwayland", Protocol::X11, source->client_id, source->xid, source->generation};
        w.clientPid = source->pid;
        w.title = source->title ? source->title : "";
        w.appId = source->app_id ? source->app_id : "";
        w.x = source->x;
        w.y = source->y;
        w.width = source->width;
        w.height = source->height;
        w.role = source->popup ? "popup" : "toplevel";
        w.modal = source->modal;
        w.minimized = source->minimized;
        w.maximized = source->maximized;
        w.fullscreen = source->fullscreen;
        w.visible = !source->minimized;
        self.sink(std::move(event));
    }

    void StartResult(bool success, Error failure = {}) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            started = success;
            startupFinished = true;
            error = std::move(failure);
        }
        startup.notify_one();
    }

    void Drain(hgs_x11_session* session) {
        std::deque<Command> pending;
        {
            std::lock_guard<std::mutex> lock(mutex);
            pending.swap(commands);
        }
        for (const auto& item : pending) {
            const auto& i = item.input;
            switch (i.kind) {
                case Input::Kind::Key:
                    hgs_x11_key(session, item.xid, item.generation, i.key.evdevCode, i.key.pressed);
                    break;
                case Input::Kind::Pointer:
                    hgs_x11_pointer(session, item.xid, item.generation, i.pointer.x, i.pointer.y,
                                    i.pointer.button, static_cast<int>(i.pointer.action));
                    break;
                case Input::Kind::Scroll:
                    hgs_x11_scroll(session, item.xid, item.generation, i.scroll.x, i.scroll.y,
                                   i.scroll.axis, i.scroll.value);
                    break;
            }
        }
    }

    void Run(Config config, int width, int height) {
        hgs_x11_settings settings{};
        settings.xkb_root = config.x11.xkbRoot.c_str();
        settings.session_dir = endpoint->Directory().c_str();
        settings.authority_path = endpoint->Authority().c_str();
        settings.x_socket = endpoint->Listener();
        settings.display_number = endpoint->DisplayNumber();
        settings.width = width;
        settings.height = height;
        settings.refresh_millihz = config.output.refreshMilliHz;
        settings.wake_fd = wake;
        settings.observer = Observe;
        settings.context = this;
        const char* stage = nullptr;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(config.x11.startupTimeoutMs);
        auto* session = hgs_x11_create(&settings, &stage);
        if (!session) {
            StartResult(false, {ErrorCode::BackendFailed, stage, "could not initialize Xwayland session"});
            return;
        }

        // 子进程已接管监听 fd。父进程不保留会使断连后仍可 connect 的副本。
        endpoint->CloseListener();
        while (!hgs_x11_ready(session) && !hgs_x11_child_lost(session) &&
               std::chrono::steady_clock::now() < deadline && !stopping.load()) {
            if (hgs_x11_dispatch(session, 25) < 0) break;
        }
        if (!hgs_x11_ready(session) || hgs_x11_child_lost(session)) {
            std::string message = hgs_x11_child_lost(session) ? "Xwayland exited before XWM was ready" :
                                                               "Xwayland XWM readiness timed out";
            const std::string diagnostic = hgs_x11_diagnostic(session);
            if (!diagnostic.empty()) message += "\n" + diagnostic;
            hgs_x11_destroy(session);
            StartResult(false, {ErrorCode::BackendFailed, "x11-ready", message});
            return;
        }

        StartResult(true);
        while (!stopping.load() && !hgs_x11_child_lost(session)) {
            Drain(session);
            if (hgs_x11_dispatch(session, -1) < 0) break;
        }
        hgs_x11_destroy(session);
        {
            std::lock_guard<std::mutex> lock(mutex);
            started = false;
            commands.clear();
        }
        if (!stopping.load()) {
            Event event;
            event.kind = EventKind::BackendDisconnected;
            event.detail = "Xwayland event loop or child exited";
            sink(std::move(event));
        }
    }

    void Wake() const {
        const uint64_t signal = 1;
        ssize_t count;
        do {
            count = write(wake, &signal, sizeof(signal));
        } while (count < 0 && errno == EINTR);
    }
};

XwaylandBackend::XwaylandBackend() : impl_(std::make_unique<Impl>()) {}
XwaylandBackend::~XwaylandBackend() { Stop(); }

bool XwaylandBackend::Start(const Config& config, Epoch epoch, Sink sink, Endpoint& endpoint, Error& error) {
    auto& p = *impl_;
    if (config.composition == CompositionMode::DesktopRoot) {
        error = {ErrorCode::UnsupportedProtocol, "x11-composition", "X11 desktop-root presentation is not available"};
        return false;
    }
    if (config.output.width > 32767 || config.output.height > 32767 ||
        config.output.refreshMilliHz > 1000000) {
        error = {ErrorCode::InvalidConfig, "x11-output", "X11 output exceeds supported size or refresh range"};
        return false;
    }
    p.endpoint = std::make_unique<X11Endpoint>();
    if (!p.endpoint->Open(config, error)) return false;
    p.wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (p.wake < 0) {
        error = {ErrorCode::BackendFailed, "x11-thread", "could not create event loop wake descriptor"};
        return false;
    }
    p.epoch = epoch;
    p.sink = std::move(sink);
    p.stopping.store(false);
    p.startupFinished = false;
    const int width = config.output.width ? config.output.width : 1280;
    const int height = config.output.height ? config.output.height : 720;
    try {
        p.worker = std::thread([&p, config, width, height] { p.Run(config, width, height); });
    } catch (const std::system_error& failure) {
        error = {ErrorCode::BackendFailed, "x11-thread", failure.what()};
        return false;
    }
    std::unique_lock<std::mutex> lock(p.mutex);
    p.startup.wait(lock, [&p] { return p.startupFinished; });
    if (!p.started) {
        error = p.error.code == ErrorCode::None ? Error{ErrorCode::BackendFailed, "x11-ready", "Xwayland stopped during startup"} : p.error;
        return false;
    }
    endpoint = {Protocol::X11, Id(), p.endpoint->Directory(), p.endpoint->Display(),
                p.endpoint->Address(), true, width, height, p.endpoint->Authority()};
    return true;
}

void XwaylandBackend::Stop() {
    auto& p = *impl_;
    p.stopping.store(true);
    if (p.worker.joinable()) {
        p.Wake();
        p.worker.join();
    }
    if (p.wake >= 0) close(p.wake);
    p.wake = -1;
    p.endpoint.reset();
    p.sink = {};
}

bool XwaylandBackend::Send(const WindowSnapshot& window, const Input& input) {
    auto& p = *impl_;
    std::lock_guard<std::mutex> lock(p.mutex);
    if (!p.started || window.origin.epoch != p.epoch || p.commands.size() >= 256) return false;
    p.commands.push_back({static_cast<uint32_t>(window.origin.nativeId), window.origin.objectGeneration, input});
    p.Wake();
    return true;
}
}
