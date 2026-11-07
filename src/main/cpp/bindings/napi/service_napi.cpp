#include "service_napi.h"
#include "include/hgs/service.h"
#include <charconv>
#include <cmath>
#include <mutex>
#include <memory>
#include <optional>

namespace hgs::service {
namespace {
const char* StateName(State s) {
    switch (s) { case State::Stopped: return "stopped"; case State::Starting: return "starting";
        case State::Ready: return "ready"; case State::Stopping: return "stopping"; case State::Failed: return "failed"; }
    return "failed";
}
const char* ErrorName(ErrorCode e) {
    switch (e) { case ErrorCode::None: return "none"; case ErrorCode::InvalidConfig: return "invalid-config";
        case ErrorCode::UnsupportedProtocol: return "unsupported-protocol"; case ErrorCode::ConfigConflict: return "config-conflict";
        case ErrorCode::BackendFailed: return "backend-failed"; case ErrorCode::NotReady: return "not-ready"; case ErrorCode::StaleWindow: return "stale-window"; }
    return "backend-failed";
}
napi_value Object(napi_env env) { napi_value o; napi_create_object(env, &o); return o; }
void Put(napi_env env, napi_value o, const char* key, const std::string& value) {
    napi_value v; napi_create_string_utf8(env, value.data(), value.size(), &v); napi_set_named_property(env, o, key, v);
}
void Put(napi_env env, napi_value o, const char* key, const char* value) { Put(env, o, key, std::string(value)); }
void Put(napi_env env, napi_value o, const char* key, bool value) {
    napi_value v; napi_get_boolean(env, value, &v); napi_set_named_property(env, o, key, v);
}
void Put(napi_env env, napi_value o, const char* key, double value) {
    napi_value v; napi_create_double(env, value, &v); napi_set_named_property(env, o, key, v);
}
void PutId(napi_env env, napi_value o, const char* key, uint64_t value) { Put(env, o, key, std::to_string(value)); }
napi_value String(napi_env env, const std::string& s) { napi_value v; napi_create_string_utf8(env, s.data(), s.size(), &v); return v; }
napi_value Boolean(napi_env env, bool b) { napi_value v; napi_get_boolean(env, b, &v); return v; }
napi_value Array(napi_env env, size_t size) { napi_value a; napi_create_array_with_length(env, size, &a); return a; }
bool IsObject(napi_env env, napi_value o) {
    napi_valuetype type; bool array = false, isNull = true; napi_value nullValue;
    if (napi_typeof(env, o, &type) != napi_ok || type != napi_object) return false;
    napi_get_null(env, &nullValue); napi_strict_equals(env, o, nullValue, &isNull); napi_is_array(env, o, &array);
    return !isNull && !array;
}
bool ReadString(napi_env env, napi_value v, std::string& out) {
    napi_valuetype t; size_t size = 0;
    if (napi_typeof(env, v, &t) != napi_ok || t != napi_string || napi_get_value_string_utf8(env, v, nullptr, 0, &size) != napi_ok) return false;
    std::string buffer(size + 1, '\0');
    if (napi_get_value_string_utf8(env, v, buffer.data(), buffer.size(), &size) != napi_ok) return false;
    buffer.resize(size); out = std::move(buffer); return true;
}
bool Has(napi_env env, napi_value o, const char* key) { bool has = false; napi_has_named_property(env, o, key, &has); return has; }
napi_value Property(napi_env env, napi_value o, const char* key) { napi_value v = nullptr; napi_get_named_property(env, o, key, &v); return v; }
bool OptionalString(napi_env env, napi_value o, const char* key, std::string& out) {
    return !Has(env, o, key) || ReadString(env, Property(env, o, key), out);
}
bool Number(napi_env env, napi_value v, double& out) {
    napi_valuetype t; return napi_typeof(env, v, &t) == napi_ok && t == napi_number &&
        napi_get_value_double(env, v, &out) == napi_ok && std::isfinite(out);
}
bool Bool(napi_env env, napi_value v, bool& out) {
    napi_valuetype t; return napi_typeof(env, v, &t) == napi_ok && t == napi_boolean && napi_get_value_bool(env, v, &out) == napi_ok;
}
napi_value Throw(napi_env env, const char* message) { napi_throw_type_error(env, "invalid-config", message); return nullptr; }
napi_value Throw(napi_env env, const Error& e) { napi_throw_error(env, ErrorName(e.code), (e.stage + ": " + e.message).c_str()); return nullptr; }
napi_value ErrorObject(napi_env env, const Error& e) {
    auto o = Object(env); Put(env, o, "code", ErrorName(e.code)); Put(env, o, "stage", e.stage); Put(env, o, "message", e.message); return o;
}
napi_value SnapshotObject(napi_env env, const Snapshot& s) {
    auto o = Object(env); Put(env, o, "state", StateName(s.state)); PutId(env, o, "epoch", s.epoch);
    auto caps = Object(env); Put(env, caps, "wayland", s.capabilities.wayland); Put(env, caps, "x11", s.capabilities.x11);
    Put(env, caps, "perWindow", s.capabilities.perWindow); Put(env, caps, "desktopRoot", s.capabilities.desktopRoot);
    Put(env, caps, "popupIdentity", s.capabilities.popupIdentity); Put(env, caps, "crossBackendFocus", s.capabilities.crossBackendFocus);
    napi_set_named_property(env, o, "capabilities", caps);
    auto endpoints = Array(env, s.endpoints.size());
    for (size_t i = 0; i < s.endpoints.size(); ++i) {
        const auto& e = s.endpoints[i]; auto ep = Object(env);
        Put(env, ep, "protocol", e.protocol == Protocol::Wayland ? "wayland" : "x11"); Put(env, ep, "backendId", e.backendId);
        Put(env, ep, "runtimeDir", e.runtimeDir); Put(env, ep, "address", e.address); Put(env, ep, "socketPath", e.socketPath);
        Put(env, ep, "authorityPath", e.authorityPath);
        Put(env, ep, "logicalOutputReady", e.logicalOutputReady); Put(env, ep, "outputWidth", static_cast<double>(e.outputWidth)); Put(env, ep, "outputHeight", static_cast<double>(e.outputHeight));
        napi_set_element(env, endpoints, i, ep);
    }
    napi_set_named_property(env, o, "endpoints", endpoints); napi_set_named_property(env, o, "lastError", ErrorObject(env, s.lastError)); return o;
}
napi_value WindowObject(napi_env env, const WindowSnapshot& w) {
    auto o = Object(env); Put(env, o, "id", w.id); PutId(env, o, "epoch", w.origin.epoch);
    Put(env, o, "protocol", w.origin.protocol == Protocol::Wayland ? "wayland" : "x11"); Put(env, o, "backendId", w.origin.backendId);
    PutId(env, o, "clientId", w.origin.clientId); PutId(env, o, "nativeId", w.origin.nativeId); PutId(env, o, "objectGeneration", w.origin.objectGeneration);
    PutId(env, o, "surfaceGeneration", w.surfaceGeneration);
    Put(env, o, "legacyId", static_cast<double>(w.legacyId)); Put(env, o, "clientPid", static_cast<double>(w.clientPid));
    Put(env, o, "title", w.title); Put(env, o, "appId", w.appId); Put(env, o, "role", w.role); Put(env, o, "parentId", w.parentId);
    Put(env, o, "x", static_cast<double>(w.x)); Put(env, o, "y", static_cast<double>(w.y));
    Put(env, o, "width", static_cast<double>(w.width)); Put(env, o, "height", static_cast<double>(w.height));
    Put(env, o, "minWidth", static_cast<double>(w.minWidth)); Put(env, o, "minHeight", static_cast<double>(w.minHeight));
    Put(env, o, "maxWidth", static_cast<double>(w.maxWidth)); Put(env, o, "maxHeight", static_cast<double>(w.maxHeight));
    Put(env, o, "visible", w.visible); Put(env, o, "minimized", w.minimized); Put(env, o, "maximized", w.maximized);
    Put(env, o, "fullscreen", w.fullscreen); Put(env, o, "modal", w.modal); return o;
}
const char* EventName(EventKind k) {
    switch (k) { case EventKind::StateChanged: return "state-changed"; case EventKind::Created: return "created";
        case EventKind::Changed: return "changed"; case EventKind::Destroyed: return "destroyed"; case EventKind::CloseRequested: return "close-requested";
        case EventKind::BackendDisconnected: return "backend-disconnected"; }
    return "changed";
}
napi_value EventObject(napi_env env, const Event& e) {
    auto o = Object(env); Put(env, o, "kind", EventName(e.kind)); PutId(env, o, "epoch", e.epoch); PutId(env, o, "serial", e.serial);
    Put(env, o, "state", StateName(e.state)); Put(env, o, "origin", e.origin); Put(env, o, "detail", e.detail);
    napi_set_named_property(env, o, "window", WindowObject(env, e.window)); return o;
}
napi_value StartResultObject(napi_env env, const StartResult& result) {
    auto value = Object(env);
    Put(env, value, "ok", result.ok);
    napi_set_named_property(env, value, "session", SnapshotObject(env, result.session));
    napi_set_named_property(env, value, "error", ErrorObject(env, result.error));
    return value;
}
struct ServiceWork {
    napi_async_work work = nullptr;
    napi_deferred deferred = nullptr;
    std::optional<Config> start;
    StartResult result;
};
napi_value QueueServiceWork(napi_env env, std::optional<Config> start) {
    auto job = std::make_unique<ServiceWork>();
    job->start = std::move(start);
    napi_value promise;
    if (napi_create_promise(env, &job->deferred, &promise) != napi_ok) return nullptr;
    const auto execute = [](napi_env, void* data) {
        auto& job = *static_cast<ServiceWork*>(data);
        if (job.start) job.result = DefaultRuntime().Start(*job.start);
        else DefaultRuntime().Stop();
    };
    const auto complete = [](napi_env env, napi_status status, void* data) {
        std::unique_ptr<ServiceWork> job(static_cast<ServiceWork*>(data));
        if (status == napi_ok) {
            const auto result = job->start ? StartResultObject(env, job->result) : Boolean(env, true);
            napi_resolve_deferred(env, job->deferred, result);
        } else {
            napi_value error;
            napi_create_error(env, nullptr, String(env, "HGS lifecycle work was cancelled"), &error);
            napi_reject_deferred(env, job->deferred, error);
        }
        napi_delete_async_work(env, job->work);
    };
    auto name = String(env, job->start ? "HgsServiceStart" : "HgsServiceStop");
    if (napi_create_async_work(env, nullptr, name, execute, complete, job.get(), &job->work) != napi_ok ||
        napi_queue_async_work(env, job->work) != napi_ok) {
        if (job->work) napi_delete_async_work(env, job->work);
        napi_value error;
        napi_create_error(env, nullptr, String(env, "could not queue HGS lifecycle work"), &error);
        napi_reject_deferred(env, job->deferred, error);
        return promise;
    }
    job.release();
    return promise;
}
napi_value StartCommon(napi_env env, napi_callback_info info, bool asynchronous) {
    size_t argc = 1; napi_value args[1]; napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc != 1 || !IsObject(env, args[0])) return Throw(env, "serviceStart requires an options object");
    Config config; auto o = args[0]; std::string protocols = "wayland", composition = "preserve", input = "auto";
    if (!OptionalString(env, o, "protocols", protocols) || !OptionalString(env, o, "runtimeDir", config.runtimeDir) ||
        !OptionalString(env, o, "composition", composition) || !OptionalString(env, o, "inputBackend", input)) return Throw(env, "invalid service option type");
    if (protocols == "wayland") config.protocols = Protocols::Wayland;
    else if (protocols == "x11") config.protocols = Protocols::X11;
    else if (protocols == "both") config.protocols = Protocols::Both;
    else return Throw(env, "protocols must be wayland, x11 or both");
    if (composition == "per-window") config.composition = CompositionMode::PerToplevel;
    else if (composition == "desktop-root") config.composition = CompositionMode::DesktopRoot;
    else if (composition != "preserve") return Throw(env, "invalid composition");
    if (input == "native") config.inputBackend = InputBackend::Native;
    else if (input == "arkts") config.inputBackend = InputBackend::ArkTS;
    else if (input != "auto") return Throw(env, "invalid inputBackend");
    if (Has(env, o, "setProcessEnvironment") && !Bool(env, Property(env, o, "setProcessEnvironment"), config.setProcessEnvironment)) return Throw(env, "invalid setProcessEnvironment");
    if (Has(env, o, "wayland")) {
        auto wl = Property(env, o, "wayland");
        if (!IsObject(env, wl) || !OptionalString(env, wl, "socketName", config.waylandSocket)) return Throw(env, "invalid wayland socketName");
    }
    if (Has(env, o, "x11")) {
        auto x11 = Property(env, o, "x11");
        if (!IsObject(env, x11) || !OptionalString(env, x11, "xkbRoot", config.x11.xkbRoot) ||
            !OptionalString(env, x11, "keymapPath", config.x11.keymapPath)) {
            return Throw(env, "invalid X11 resource paths");
        }
        if (Has(env, x11, "startupTimeoutMs")) {
            double timeout;
            if (!Number(env, Property(env, x11, "startupTimeoutMs"), timeout) ||
                timeout < 100 || timeout > 30000 || timeout != std::floor(timeout)) {
                return Throw(env, "X11 startupTimeoutMs must be an integer from 100 to 30000");
            }
            config.x11.startupTimeoutMs = static_cast<int32_t>(timeout);
        }
    }
    if (Has(env, o, "output")) {
        auto output = Property(env, o, "output"); if (!IsObject(env, output)) return Throw(env, "invalid output");
        for (const char* key : {"width", "height", "refreshMilliHz", "scale"}) {
            if (!Has(env, output, key)) continue;
            double n;
            if (!Number(env, Property(env, output, key), n)) return Throw(env, "output fields must be finite numbers");
            if (std::string(key) == "scale") config.output.scale = n;
            else {
                if (n < 0 || n > INT32_MAX || n != std::floor(n)) return Throw(env, "output dimensions must be nonnegative integers");
                if (std::string(key) == "width") config.output.width = n;
                else if (std::string(key) == "height") config.output.height = n;
                else config.output.refreshMilliHz = n;
            }
        }
    }
    if (asynchronous) return QueueServiceWork(env, std::move(config));
    return StartResultObject(env, DefaultRuntime().Start(config));
}
napi_value Start(napi_env env, napi_callback_info info) { return StartCommon(env, info, false); }
napi_value StartAsync(napi_env env, napi_callback_info info) { return StartCommon(env, info, true); }
napi_value StopAsync(napi_env env, napi_callback_info) { return QueueServiceWork(env, std::nullopt); }
napi_value Stop(napi_env env, napi_callback_info) { DefaultRuntime().Stop(); return Boolean(env, true); }
napi_value Inspect(napi_env env, napi_callback_info) { return SnapshotObject(env, DefaultRuntime().Inspect()); }
napi_value Windows(napi_env env, napi_callback_info info) {
    size_t argc = 1; napi_value args[1]; napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    std::string epoch;
    if (argc && (!ReadString(env, args[0], epoch) || epoch.empty())) return Throw(env, "session epoch must be a nonempty string");
    auto windows = DefaultRuntime().Windows(epoch); auto arr = Array(env, windows.size());
    for (size_t i = 0; i < windows.size(); ++i) napi_set_element(env, arr, i, WindowObject(env, windows[i]));
    return arr;
}
napi_value Environment(napi_env env, napi_callback_info info) {
    size_t argc = 2; napi_value args[2]; napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    std::string protocol, epoch;
    if (argc != 2 || !ReadString(env, args[0], protocol) || !ReadString(env, args[1], epoch) || epoch.empty()) return Throw(env, "protocol and session epoch strings required");
    if (protocol != "wayland" && protocol != "x11") return Throw(env, "invalid protocol");
    Error error; const auto e = DefaultRuntime().Environment(protocol == "wayland" ? Protocol::Wayland : Protocol::X11, error, epoch);
    if (error.code != ErrorCode::None) return Throw(env, error);
    auto o = Object(env); auto set = Object(env);
    for (const auto& entry : e.set) Put(env, set, entry.name.c_str(), entry.value);
    napi_set_named_property(env, o, "set", set);
    auto unset = Array(env, e.unset.size());
    for (size_t i = 0; i < e.unset.size(); ++i) napi_set_element(env, unset, i, String(env, e.unset[i]));
    napi_set_named_property(env, o, "unset", unset);
    napi_set_named_property(env, o, "inheritedFds", Array(env, 0)); return o;
}
napi_value BindSurface(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value args[4];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    std::string window, surface, epoch;
    bool attach;
    if (argc != 4 || !ReadString(env, args[0], window) || window.empty() ||
        !ReadString(env, args[1], surface) || !Bool(env, args[2], attach) ||
        !ReadString(env, args[3], epoch) || epoch.empty())
        return Throw(env, "window ID, surface ID, attach flag and session epoch required");
    uint64_t id = 0;
    const auto parsed = std::from_chars(surface.data(), surface.data() + surface.size(), id);
    if (parsed.ec != std::errc{} || parsed.ptr != surface.data() + surface.size() || !id)
        return Throw(env, "invalid OHOS surface ID");
    return Boolean(env, DefaultRuntime().BindSurface(window, id, attach, epoch));
}
napi_value Send(napi_env env, napi_callback_info info) {
    size_t argc = 3; napi_value args[3]; napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    std::string window, kind, epoch;
    if (argc < 2 || !ReadString(env, args[0], window) || !IsObject(env, args[1]) ||
        !Has(env, args[1], "kind") || !ReadString(env, Property(env, args[1], "kind"), kind)) return Throw(env, "window ID and typed input required");
    if (argc > 2 && (!ReadString(env, args[2], epoch) || epoch.empty())) return Throw(env, "session epoch must be a nonempty string");
    auto o = args[1]; Input input;
    auto read = [&](const char* name, double& n) { return Has(env, o, name) && Number(env, Property(env, o, name), n); };
    double n = 0;
    if (kind == "key") {
        input.kind = Input::Kind::Key;
        if (!read("evdevCode", n) || n < 0 || n > 0x2ff || n != std::floor(n) || !Has(env, o, "pressed") ||
            !Bool(env, Property(env, o, "pressed"), input.key.pressed)) return Throw(env, "invalid key event");
        input.key.evdevCode = n;
    } else if (kind == "pointer") {
        if (!read("action", n) || n < 1 || n > 3 || n != std::floor(n)) return Throw(env, "invalid pointer action");
        input.pointer.action = static_cast<PointerAction>(static_cast<int>(n));
        if (!read("x", input.pointer.x) || !read("y", input.pointer.y) || !read("button", n) || n < 0 || n > UINT32_MAX || n != std::floor(n)) return Throw(env, "invalid pointer event");
        input.pointer.button = n;
        if (Has(env, o, "rawDeltaX") && !read("rawDeltaX", input.pointer.rawDeltaX)) return Throw(env, "invalid rawDeltaX");
        if (Has(env, o, "rawDeltaY") && !read("rawDeltaY", input.pointer.rawDeltaY)) return Throw(env, "invalid rawDeltaY");
        if (Has(env, o, "fromMouse") && !Bool(env, Property(env, o, "fromMouse"), input.pointer.fromMouse)) return Throw(env, "invalid fromMouse");
    } else if (kind == "scroll") {
        input.kind = Input::Kind::Scroll;
        if (!read("axis", n) || (n != 0 && n != 1)) return Throw(env, "invalid scroll axis");
        input.scroll.axis = n;
        if (!read("value", input.scroll.value) || !read("x", input.scroll.x) || !read("y", input.scroll.y)) return Throw(env, "invalid scroll event");
        if (Has(env, o, "scrollStep")) {
            if (!read("scrollStep", n) || n < INT32_MIN || n > INT32_MAX || n != std::floor(n)) return Throw(env, "invalid scrollStep");
            input.scroll.scrollStep = n;
        }
    } else return Throw(env, "unknown input kind");
    return Boolean(env, DefaultRuntime().Send(window, input, epoch));
}
struct CallbackState { std::mutex mutex; napi_threadsafe_function callback = nullptr; uint64_t token = 0; } callbacks;
void Deliver(napi_env env, napi_value callback, void*, void* data) {
    std::unique_ptr<Event> event(static_cast<Event*>(data));
    if (!env || !callback) return;
    auto value = EventObject(env, *event); napi_value self; napi_get_undefined(env, &self);
    napi_call_function(env, self, callback, 1, &value, nullptr);
}
void ClearCallback(void*) {
    uint64_t token; napi_threadsafe_function old;
    { std::lock_guard<std::mutex> lock(callbacks.mutex); token = callbacks.token; old = callbacks.callback; callbacks.token = 0; callbacks.callback = nullptr; }
    if (token) DefaultRuntime().Unsubscribe(token);
    if (old) napi_release_threadsafe_function(old, napi_tsfn_abort);
}
napi_value SetCallback(napi_env env, napi_callback_info info) {
    size_t argc = 1; napi_value args[1]; napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    napi_valuetype t;
    if (argc != 1 || napi_typeof(env, args[0], &t) != napi_ok || t != napi_function) return Throw(env, "event listener must be a function");
    ClearCallback(nullptr);
    napi_threadsafe_function fn;
    if (napi_create_threadsafe_function(env, args[0], nullptr, String(env, "hgs-service-events"), 0, 1, nullptr, nullptr, nullptr, Deliver, &fn) != napi_ok) return Throw(env, "cannot create event callback");
    { std::lock_guard<std::mutex> lock(callbacks.mutex); callbacks.callback = fn; }
    auto token = DefaultRuntime().Subscribe([](const Event& event) {
        napi_threadsafe_function callback;
        {
            std::lock_guard<std::mutex> lock(callbacks.mutex); callback = callbacks.callback;
            if (!callback || napi_acquire_threadsafe_function(callback) != napi_ok) return;
        }
        auto copy = std::make_unique<Event>(event);
        if (napi_call_threadsafe_function(callback, copy.get(), napi_tsfn_nonblocking) == napi_ok) copy.release();
        napi_release_threadsafe_function(callback, napi_tsfn_release);
    });
    { std::lock_guard<std::mutex> lock(callbacks.mutex); callbacks.token = token; }
    return Boolean(env, true);
}
}
void RegisterServiceNapi(napi_env env, napi_value exports) {
    napi_property_descriptor properties[] = {
        {"serviceStart", nullptr, Start, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"serviceStop", nullptr, Stop, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"serviceStartAsync", nullptr, StartAsync, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"serviceStopAsync", nullptr, StopAsync, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"serviceInspect", nullptr, Inspect, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"serviceWindows", nullptr, Windows, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"serviceClientEnvironment", nullptr, Environment, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"serviceSendInput", nullptr, Send, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"serviceBindSurface", nullptr, BindSurface, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setServiceEventCallback", nullptr, SetCallback, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(properties) / sizeof(properties[0]), properties);
    napi_add_env_cleanup_hook(env, ClearCallback, nullptr);
}
}
