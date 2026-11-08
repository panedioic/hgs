#include "xwayland_backend.h"
#include "external_frame_bridge.h"
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
#include <variant>

namespace hgs::service {
struct XwaylandBackend::Impl {
    struct SurfaceBinding { uint64_t id; bool attach; };
    struct Control { hgs_x11_control action; int32_t width, height; };
    using Operation = std::variant<Input, SurfaceBinding, std::string, Control>;
    struct Command {
        uint32_t xid;
        uint64_t generation;
        Operation operation;
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
    ExternalFrameProvider* frameProvider = nullptr;

    static hgs_x11_external_result PresentExternal(void* context, uint32_t xid, uint32_t pid,
                                                   uint64_t generation, NativeWindow* output) {
        return static_cast<X11ExternalFrameBridge*>(context)->Present(xid, pid, generation, output);
    }
    static void CloseExternal(void* context, uint64_t generation, bool unmapped) {
        static_cast<X11ExternalFrameBridge*>(context)->Close(generation, unmapped);
    }

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
        w.minWidth = source->min_width;
        w.minHeight = source->min_height;
        w.maxWidth = source->max_width;
        w.maxHeight = source->max_height;
        if (source->parent_xid)
            event.parentOrigin = {self.epoch, "xwayland", Protocol::X11,
                                  source->parent_client_id, source->parent_xid, source->parent_generation};
        w.role = source->popup ? "popup" : "toplevel";
        w.modal = source->modal;
        w.minimized = source->minimized;
        w.maximized = source->maximized;
        w.fullscreen = source->fullscreen;
        w.visible = !source->minimized;
        w.textInputActive = source->text_input_active;
        w.surfaceGeneration = source->surface_generation;
        w.framePresented = source->frame_presented;
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
            if (const auto* surface = std::get_if<SurfaceBinding>(&item.operation)) {
                hgs_x11_bind_surface(session, item.xid, item.generation, surface->id, surface->attach);
                continue;
            }
            if (const auto* text = std::get_if<std::string>(&item.operation)) {
                hgs_x11_commit_text(session, item.xid, item.generation, text->data(), text->size());
                continue;
            }
            if (const auto* control = std::get_if<Control>(&item.operation)) {
                hgs_x11_control_window(session, item.xid, item.generation,
                    control->action, control->width, control->height);
                continue;
            }
            const auto& i = std::get<Input>(item.operation);
            switch (i.kind) {
                case Input::Kind::Key:
                    hgs_x11_key(session, item.xid, item.generation, i.key.evdevCode, i.key.pressed);
                    break;
                case Input::Kind::Pointer:
                    hgs_x11_pointer(session, item.xid, item.generation, i.pointer.x, i.pointer.y,
                                    i.pointer.button, static_cast<int>(i.pointer.action), i.normalizedPosition);
                    break;
                case Input::Kind::Scroll:
                    hgs_x11_scroll(session, item.xid, item.generation, i.scroll.x, i.scroll.y,
                                   i.scroll.axis, i.scroll.value, i.normalizedPosition);
                    break;
            }
        }
    }

    void Run(Config config, int width, int height) {
        std::unique_ptr<X11ExternalFrameBridge> externalFrames;
        if (frameProvider) {
            externalFrames = std::make_unique<X11ExternalFrameBridge>(
                *frameProvider, 1000000000000ull / config.output.refreshMilliHz);
        }
        hgs_x11_settings settings{};
        settings.xkb_root = config.x11.xkbRoot.c_str();
        settings.session_dir = endpoint->Directory().c_str();
        settings.authority_path = endpoint->Authority().c_str();
        settings.ime_server_name = config.x11.imeServerName.c_str();
        settings.x_socket = endpoint->Listener();
        settings.display_number = endpoint->DisplayNumber();
        settings.width = width;
        settings.height = height;
        settings.refresh_millihz = config.output.refreshMilliHz;
        settings.wake_fd = wake;
        settings.observer = Observe;
        if (externalFrames) {
            settings.present_external = PresentExternal;
            settings.close_external = CloseExternal;
            settings.external_context = externalFrames.get();
        }
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
        while (!hgs_x11_ready(session) && !hgs_x11_child_lost(session) && !hgs_x11_xim_failed(session) &&
               std::chrono::steady_clock::now() < deadline && !stopping.load()) {
            if (hgs_x11_dispatch(session, 25) < 0) break;
        }
        if (!hgs_x11_ready(session) || hgs_x11_child_lost(session)) {
            std::string message = hgs_x11_child_lost(session) ? "Xwayland exited before XWM was ready" :
                                                               "Xwayland XWM readiness timed out";
            const bool ximFailed = hgs_x11_xim_failed(session);
            if (ximFailed) message = "XIM helper exited before registering its server";
            const std::string diagnostic = hgs_x11_diagnostic(session);
            if (!diagnostic.empty()) message += "\n" + diagnostic;
            hgs_x11_destroy(session);
            StartResult(false, {ErrorCode::BackendFailed, ximFailed ? "xim-ready" : "x11-ready", message});
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

    bool Enqueue(const WindowSnapshot& window, Operation operation) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!started || stopping.load() || window.origin.epoch != epoch || commands.size() >= 256) return false;
        commands.push_back({static_cast<uint32_t>(window.origin.nativeId), window.origin.objectGeneration,
                            std::move(operation)});
        Wake();
        return true;
    }
};

XwaylandBackend::XwaylandBackend() : impl_(std::make_unique<Impl>()) {}
XwaylandBackend::~XwaylandBackend() { Stop(); }

bool XwaylandBackend::Start(const Config& config, Epoch epoch, Sink sink, Endpoint& endpoint, Error& error) {
    auto& p = *impl_;
    if (!config.x11.imeServerName.empty() && !SupportsXim()) {
        error = {ErrorCode::UnsupportedProtocol, "xim-build", "XIM support is not built"};
        return false;
    }
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
                p.endpoint->Address(), true, width, height, p.endpoint->Authority(), config.x11.imeServerName};
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
    return impl_->Enqueue(window, input);
}

bool XwaylandBackend::BindSurface(const WindowSnapshot& window, uint64_t surfaceId, bool attach) {
    return impl_->Enqueue(window, Impl::SurfaceBinding{surfaceId, attach});
}

bool XwaylandBackend::SupportsXim() const {
#ifdef HGS_BUILD_XIM
    return true;
#else
    return false;
#endif
}

bool XwaylandBackend::CommitText(const WindowSnapshot& window, std::string text) {
    return SupportsXim() && impl_->Enqueue(window, std::move(text));
}

bool XwaylandBackend::ControlWindow(const WindowSnapshot& window, const WindowControl& control) {
    constexpr hgs_x11_control actions[] = {HGS_X11_FOCUS, HGS_X11_BLUR, HGS_X11_RESIZE, HGS_X11_CLOSE};
    const auto index = static_cast<size_t>(control.kind);
    if (index >= sizeof(actions) / sizeof(actions[0])) return false;
    return impl_->Enqueue(window, Impl::Control{actions[index], control.width, control.height});
}

void XwaylandBackend::SetExternalFrameProvider(ExternalFrameProvider* provider) {
    impl_->frameProvider = provider;
}
}
