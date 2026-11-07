#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "types.h"

namespace hgs::service {
using WindowId = std::string;
using Epoch = uint64_t;
enum class Protocol { Wayland, X11 };
enum class Protocols { Wayland, X11, Both };
enum class State { Stopped, Starting, Ready, Stopping, Failed };
enum class ErrorCode { None, InvalidConfig, UnsupportedProtocol, ConfigConflict, BackendFailed, NotReady, StaleWindow };
enum class EventKind { StateChanged, Created, Changed, Destroyed, CloseRequested };

struct Error {
    ErrorCode code = ErrorCode::None;
    std::string stage;
    std::string message;
};
struct Config {
    Protocols protocols = Protocols::Wayland;
    std::string runtimeDir;
    std::string waylandSocket = "wayland-hgs-0";
    OutputConfig output;
    CompositionMode composition = CompositionMode::Unspecified;
    InputBackend inputBackend = InputBackend::Auto;
    bool setProcessEnvironment = false;
    // 仅供 v1 配置兼容；公共服务的输出、输入与生命周期不属于某个后端。
    FeatureConfig legacyWaylandFeatures;
};
struct Endpoint {
    Protocol protocol = Protocol::Wayland;
    std::string backendId;
    std::string runtimeDir;
    std::string address;
    std::string socketPath;
    bool logicalOutputReady = false;
    int32_t outputWidth = 0, outputHeight = 0;
};
struct Capabilities {
    bool wayland = false;
    bool x11 = false;
    bool perWindow = true;
    bool desktopRoot = true;
    bool popupIdentity = false;  // v1 popup events are still attached to their parent
    bool crossBackendFocus = false; // P4, not implied by the routing contract
};
struct EnvironmentVariable { std::string name, value; };
struct InheritedFd { std::string name; int fd = -1; };
struct ClientEnvironment {
    std::vector<EnvironmentVariable> set;
    std::vector<std::string> unset;
    std::vector<InheritedFd> inheritedFds;
};
struct Snapshot {
    State state = State::Stopped;
    Epoch epoch = 0;
    Capabilities capabilities;
    std::vector<Endpoint> endpoints;
    Error lastError;
};
struct StartResult { bool ok = false; Snapshot session; Error error; };

// Protocol-native identity never crosses into a numeric ArkTS WindowId.
// clientId identifies a connection, not a PID. objectGeneration handles reuse.
struct WindowOrigin {
    Epoch epoch = 0;
    std::string backendId;
    Protocol protocol = Protocol::Wayland;
    uint64_t clientId = 0;
    uint64_t nativeId = 0;
    uint64_t objectGeneration = 0;
};
struct WindowSnapshot {
    WindowId id;
    WindowOrigin origin;
    uint32_t legacyId = 0;
    uint32_t clientPid = 0;
    std::string title, appId;
    WindowId parentId;
    std::string role = "toplevel";
    int32_t x = 0, y = 0, width = 0, height = 0;
    int32_t minWidth = 0, minHeight = 0, maxWidth = 0, maxHeight = 0;
    bool visible = true, minimized = false, maximized = false, fullscreen = false, modal = false;
    uint64_t surfaceGeneration = 0;
};
// All fields own their storage. Subscribers may copy events beyond the callback.
struct Event {
    EventKind kind = EventKind::StateChanged;
    Epoch epoch = 0;
    uint64_t serial = 0;
    std::string origin = "backend";
    std::string detail;
    State state = State::Stopped;
    WindowSnapshot window;
};
struct Input {
    enum class Kind { Pointer, Key, Scroll };
    Kind kind = Kind::Pointer;
    PointerEvent pointer;
    KeyEvent key;
    ScrollEvent scroll;
};
} // namespace hgs::service
