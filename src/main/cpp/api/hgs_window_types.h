#pragma once
// ============================================================================
// hgs_window_types.h — HGS 窗口管理器公共类型 (阶段 2 W1)。
//
// 出处: docs/architecture/hgs-shared-runtime-phase0.md §15.3。
// 边界: 只描述宿主窗口模型; Wayland 协议对象仍用 ToplevelId/WindowId 对应,
// 几何/状态/z-order 的运行期权威在核心 ToplevelManager, WindowSpec 只存
// 宿主侧 spec (pid/appId/title/parent/modal/装饰) 与初始/期望几何。
//
// 零平台依赖: 仅标准库 — host_tests 可直连编译。
// ============================================================================

#include <cstdint>
#include <string>

#include "hgs_export.h"
#include "hgs_types.h"

namespace hgs {

using WindowId = ToplevelId;

enum class WindowState : uint8_t {
    Normal = 0,
    Minimized = 1,
    Maximized = 2,
    Fullscreen = 3,
};

enum class WindowEventType : uint32_t {
    Unknown = 0,
    Created = 1,
    Destroyed = 2,
    TitleChanged = 3,
    GeometryChanged = 4,
    StateChanged = 5,
    FocusChanged = 6,
    VisibleChanged = 7,
    CloseRequested = 8,
    MoveRequested = 9,
    ResizeRequested = 10,
    PopupShown = 11,
    PopupMoved = 12,
    PopupResized = 13,
    PopupHidden = 14,
};

struct HGS_API WindowGeometry {
    int32_t x = 0;
    int32_t y = 0;
    int32_t width = 0;   // 客户端内容尺寸 (不是 OHOS 窗口尺寸; host 按缩放换算)
    int32_t height = 0;
};

struct HGS_API WindowSpec {
    WindowId id = 0;
    uint32_t clientPid = 0;
    std::string appId;
    std::string title;
    WindowGeometry geometry;   // 初始/期望几何; 运行期权威读 ToplevelState
    int32_t minWidth = 0;
    int32_t minHeight = 0;
    int32_t maxWidth = 0;
    int32_t maxHeight = 0;
    WindowId parentId = 0;
    bool modal = false;
    bool decorated = true;
    bool visible = true;
    WindowState state = WindowState::Normal;
};

struct HGS_API WindowEvent {
    WindowEventType type = WindowEventType::Unknown;
    WindowId id = 0;
    WindowSpec spec;
    WindowGeometry geometry;
    WindowState state = WindowState::Normal;
    bool focused = false;
    uint32_t edges = 0;       // ResizeRequested (xdg_resize_edge 位图)
    std::string json;         // 兼容旧 payload (过渡期; 可为空)
};

} // namespace hgs
