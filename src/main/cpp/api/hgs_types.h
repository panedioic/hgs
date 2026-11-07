#pragma once
// ============================================================================
// hgs_types.h — HGS 公共契约基础类型 (阶段 0, P0.2)。
//
// 出处: docs/architecture/hgs-shared-runtime-phase0.md §4.2 与
//       docs/architecture/hgs-exported-api.md (导出面规划)。
//
// 零依赖约定: 仅标准库头 — 不得引入 napi/hilog/wayland/EGL/native_window。
// 平台依赖只允许出现在实现文件 (api/hgs_instance.cpp 等)。
//
// 版本: HGS_API_VERSION = major<<16 | minor。新增导出 = minor+1;
// 删改签名/语义 = major+1 (hgs-exported-api.md §2.2)。
// ============================================================================

#include <cstdint>
#include <string>
#include <vector>

#include "hgs_export.h"

namespace hgs {

// HGS 公共 API 版本。1.2 增加可选 X11 后端与异步生命周期入口。
inline constexpr uint32_t HGS_API_VERSION = (1u << 16) | 2u;

// -- 标识类型 --
using ToplevelId        = uint32_t;  // Wayland toplevel (协议对象); 0 = 无效
using SurfaceToken      = uint64_t;  // OHOS XComponent surfaceId; ArkTS 侧 bigint
using SurfaceGeneration = uint32_t;  // surface 绑定代次; 0 = 无效/通配 (见 AttachSurface)

// -- 窗口模式 --
// Unspecified = 不修改宿主当前模式 (红线: WineHua 在 startServer 前已按
// 屏幕算好 output 尺寸并 applyModePolicy, HgsInstance::Start 不得用默认值
// 覆盖 — phase0 §1.3)。
enum class CompositionMode : uint8_t {
    Unspecified = 0,
    PerToplevel = 1,   // 每个 toplevel 一个 OHOS 窗口 (WineHua PC/HarmonyBox/BetterSH 现态)
    DesktopRoot = 2,   // 全部 toplevel 合成到一个 root surface (WineHua 平板/融合桌面)
};

// -- 输入后端 (阶段 0 仅记录; 阶段 1 生效, phase0 §14) --
// ArkTS  = 现有 .onTouch/.onMouse/.onAxisEvent/.onKeyEvent 路径。
// Native = OH_NativeXComponent 回调直通 C++ (ArkTS 零输入事件注册)。
enum class InputBackend : uint8_t {
    Auto = 0,   // HGS 按宿主/平台选择; 当前等价 ArkTS, 目标 Native
    ArkTS = 1,
    Native = 2,
};

struct OutputConfig {
    // 0 = 不修改 (同 CompositionMode::Unspecified 红线)
    int32_t width = 0;
    int32_t height = 0;
    int32_t refreshMilliHz = 60000;  // 预留; 阶段 0 未使用
    double scale = 0.0;              // 0 = 不修改; 对应 setDisplayScale (现 no-op)
};

struct FeatureConfig {
    // 声明式能力开关。阶段 0 仅记录, 不改变注册行为 (shm/subsurface/
    // viewporter 现恒开; winehuaToplevel 现恒注册 — 阶段 1 起按此生效,
    // phase0 §15.9: BetterSH 须 dataDevice=true 且 winehuaToplevel=false)。
    bool shm = true;
    bool subsurface = true;
    bool viewporter = true;
    bool xdgPopup = true;
    bool dataDevice = false;       // wl_data_device_manager (GTK seat/clipboard 硬要求)
    bool relativePointer = false;  // zwp_relative_pointer/warp/constraints (Wine)
    bool textInput = false;        // zwp_text_input_v3 (IME)
    bool externalSurface = false;  // zero-copy / VirGL / Venus provider
    bool winehuaToplevel = false;  // WineHua 专用协议
};

struct HgsConfig {
    std::string socketPath;  // 必须绝对路径且带 basename (Start 校验, 见 hgs_endpoint.h)

    OutputConfig output;
    CompositionMode composition = CompositionMode::Unspecified;
    InputBackend inputBackend = InputBackend::Auto;
    FeatureConfig features;

    // true = 沿用旧行为 (WaylandServer::Start 内部 setenv XDG_RUNTIME_DIR/
    // WAYLAND_DISPLAY)。阶段 0 仅记录; 目标默认 false, 宿主经
    // ClientEnvironment() 显式传给子进程 (phase0 §4.2)。
    bool setProcessEnvironment = true;
};

// -- Surface 绑定 --
struct SurfaceBinding {
    ToplevelId toplevelId = 0;
    SurfaceToken token = 0;
    SurfaceGeneration generation = 0;  // AttachSurface 返回时填充分配值; 0 = 失败
};

// -- 输入事件 (与 InputManager 现有参数逐项对应, input_manager.h:70-81) --
enum class PointerAction : int32_t {
    Press = 1,    // ArkTS MouseAction.Press
    Release = 2,
    Move = 3,
};

struct PointerEvent {
    ToplevelId hintToplevel = 0;   // PerToplevel 模式由宿主指定目标
    PointerAction action = PointerAction::Move;
    double x = 0, y = 0;           // 物理像素 (组件内)
    uint32_t button = 0;           // evdev button code (0x110/0x111/0x112)
    double rawDeltaX = 0, rawDeltaY = 0;  // MouseEvent.rawDelta (相对模式位移源)
    bool fromMouse = false;        // onMouse 通道 (区分触屏点击语义)
};

struct KeyEvent {
    ToplevelId toplevelId = 0;
    int32_t evdevCode = 0;         // 已映射的 evdev keycode
    bool pressed = false;
};

struct ScrollEvent {
    ToplevelId hintToplevel = 0;
    int32_t axis = 0;              // 0=垂直 1=水平
    double value = 0;              // 正=向下/向右
    int32_t scrollStep = 0;        // ArkTS AxisEvent.scrollStep
    double x = 0, y = 0;           // 物理像素 (组件内)
};

// -- 宿主窗口操作请求 (阶段 0 仅定义; 阶段 1 由 xdg_shell 产生) --
enum class HostRequestType : uint8_t {
    Move, Resize, Maximize, Unmaximize, Minimize, Fullscreen, Close,
};

struct HostRequest {
    HostRequestType type = HostRequestType::Move;
    ToplevelId toplevelId = 0;
    uint32_t edges = 0;            // xdg_resize_edge 位图 (Resize)
};

// -- toplevel 信息快照 (阶段 2 起由总线构造点直接填结构体; 阶段 0 契约占位。
//    不做 JSON 反解 — 标题转义是已知历史行为, parser 会引入新边界, phase0 §6) --
struct ToplevelInfo {
    ToplevelId id = 0;
    uint32_t clientPid = 0;
    uint64_t surfaceKey = 0;
    std::string title;
    std::string appId;
    ToplevelId parentId = 0;
    int32_t x = 0, y = 0, width = 0, height = 0;
    bool visible = true;
    bool minimized = false;
    bool maximized = false;
    bool fullscreen = false;
};

} // namespace hgs
