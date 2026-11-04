#pragma once
// ============================================================================
// hgs_events.h — HGS 公共事件契约 (阶段 0, P0.1): 事件枚举单一来源。
//
// 出处: docs/architecture/hgs-shared-runtime-phase0.md §4.3 与
//       docs/architecture/hgs-exported-api.md §4.2。
// 从 compositor/toplevel/toplevel_event_bus.h 迁入 namespace hgs;
// 原位置经 using 别名保持兼容 (bus/wl_core/xdg_shell/wayland_server/
// winehua_toplevel 与 host_tests 的现有调用零改动)。
//
// 零依赖约定: 仅标准库头 (与 phase0 §4.1 一致) — 可被 host_tests 直连
// 编译 (make test), 不得引入 napi/hilog/wayland/EGL。
//
// 红线: ToplevelEventName 的字符串与 ArkTS 消费逐字一致
// (WineWindowManager.ets / PopupWindowManager.ets / WaymonyService.ets)。
// 映射漂移由 host_tests/toplevel_event_test.cpp (黄金值) 与
// host_tests/hgs_event_map_test.cpp (双向映射) 双重固化。
// ============================================================================

#include <cstdint>
#include <cstring>

namespace hgs {

// toplevel 事件类型 (原 bus enum 迁入 + Unknown=0 哨兵)。
// 显式数值, 勿重排/复用 — NAPI setToplevelEventCallback 与
// HgsInstance 事件契约 (hgs-exported-api.md §4.2 HgsToplevelEventType)
// 自 1.0 起按数值透传给宿主。
enum class ToplevelEventType : uint32_t {
    Unknown = 0,   // 哨兵: 反查失败/无效事件; 不参与投递
    // 生命周期
    Created = 1,       // "created"        PC 首帧 ({\"w\":\"h\"}) / 桌面 get_toplevel ({\"w\":640,\"h\":480})
    Destroyed = 2,     // "destroyed"
    // popup (PC 模式菜单)
    PopupHide = 3,     // "popup_hide"
    PopupMove = 4,     // "popup_move"
    PopupShow = 5,     // "popup_show"
    PopupResize = 6,   // "popup_resize"
    // ARGB 窗口格式
    Argb = 7,          // "argb"           shm format 切换 (首帧必发)
    // 尺寸/格式
    Resize = 8,        // "resize"
    Surface = 9,       // "surface"        renderer surface 物理像素尺寸
    Title = 10,        // "title"
    Limits = 11,       // "limits"         min/max size
    // 窗口状态 (xdg)
    Maximized = 12,    // "maximized"
    Unmaximized = 13,  // "unmaximized"
    Fullscreen = 14,   // "fullscreen"
    Unfullscreen = 15, // "unfullscreen"
    Minimized = 16,    // "minimized"
    Restored = 17,     // "restored"       最小化还原 (Wine 恢复帧 / 主动显示窗口)
    // 交互式移动
    MoveStart = 18,    // "move_start"     xdg_toplevel.move grab 开始
    MoveEnd = 19,      // "move_end"       grab 结束 (或 grab 窗口销毁复位)
    // WineHua modal 关系 (winehua_toplevel.set_modal 上报)
    Modal = 20,        // "modal"          1=置为模态 (modal/owner 报关系), 0=解除
    // 会话
    DesktopRoot = 21,  // "desktop_root"   桌面 root 出现 (识别/PromotePending 两路径)
};

// 事件类型 → 字符串 (与 ArkTS 消费的原始字符串逐字; 日志与 sink 均用此映射)
inline const char* ToplevelEventName(ToplevelEventType evt) {
    switch (evt) {
        case ToplevelEventType::Unknown:       return "unknown";
        case ToplevelEventType::Created:       return "created";
        case ToplevelEventType::Destroyed:     return "destroyed";
        case ToplevelEventType::PopupHide:     return "popup_hide";
        case ToplevelEventType::PopupMove:     return "popup_move";
        case ToplevelEventType::PopupShow:     return "popup_show";
        case ToplevelEventType::PopupResize:   return "popup_resize";
        case ToplevelEventType::Argb:          return "argb";
        case ToplevelEventType::Resize:        return "resize";
        case ToplevelEventType::Surface:       return "surface";
        case ToplevelEventType::Title:         return "title";
        case ToplevelEventType::Limits:        return "limits";
        case ToplevelEventType::Maximized:     return "maximized";
        case ToplevelEventType::Unmaximized:   return "unmaximized";
        case ToplevelEventType::Fullscreen:    return "fullscreen";
        case ToplevelEventType::Unfullscreen:  return "unfullscreen";
        case ToplevelEventType::Minimized:     return "minimized";
        case ToplevelEventType::Restored:      return "restored";
        case ToplevelEventType::MoveStart:     return "move_start";
        case ToplevelEventType::MoveEnd:       return "move_end";
        case ToplevelEventType::Modal:         return "modal";
        case ToplevelEventType::DesktopRoot:   return "desktop_root";
    }
    return "unknown";  // 防御: 枚举越界永不发生 (仅编译器告警消噪)
}

// 字符串 → 事件类型 (互逆映射; 找不到/空指针返回 Unknown)。
// 消费方: HgsInstance 事件适配层 (api/hgs_instance.cpp) 与阶段 1 的
// 类型化 NAPI 通道 — bus 内部仍用 enum 直传, 不经字符串。
inline ToplevelEventType ToplevelEventTypeFromName(const char* name) {
    if (name == nullptr) return ToplevelEventType::Unknown;
    if (strcmp(name, "created") == 0)        return ToplevelEventType::Created;
    if (strcmp(name, "destroyed") == 0)      return ToplevelEventType::Destroyed;
    if (strcmp(name, "popup_hide") == 0)     return ToplevelEventType::PopupHide;
    if (strcmp(name, "popup_move") == 0)     return ToplevelEventType::PopupMove;
    if (strcmp(name, "popup_show") == 0)     return ToplevelEventType::PopupShow;
    if (strcmp(name, "popup_resize") == 0)   return ToplevelEventType::PopupResize;
    if (strcmp(name, "argb") == 0)           return ToplevelEventType::Argb;
    if (strcmp(name, "resize") == 0)         return ToplevelEventType::Resize;
    if (strcmp(name, "surface") == 0)        return ToplevelEventType::Surface;
    if (strcmp(name, "title") == 0)          return ToplevelEventType::Title;
    if (strcmp(name, "limits") == 0)         return ToplevelEventType::Limits;
    if (strcmp(name, "maximized") == 0)      return ToplevelEventType::Maximized;
    if (strcmp(name, "unmaximized") == 0)    return ToplevelEventType::Unmaximized;
    if (strcmp(name, "fullscreen") == 0)     return ToplevelEventType::Fullscreen;
    if (strcmp(name, "unfullscreen") == 0)   return ToplevelEventType::Unfullscreen;
    if (strcmp(name, "minimized") == 0)      return ToplevelEventType::Minimized;
    if (strcmp(name, "restored") == 0)       return ToplevelEventType::Restored;
    if (strcmp(name, "move_start") == 0)     return ToplevelEventType::MoveStart;
    if (strcmp(name, "move_end") == 0)       return ToplevelEventType::MoveEnd;
    if (strcmp(name, "modal") == 0)          return ToplevelEventType::Modal;
    if (strcmp(name, "desktop_root") == 0)   return ToplevelEventType::DesktopRoot;
    return ToplevelEventType::Unknown;
}

// 全部已知事件 (遍历/测试用; 不含 Unknown 哨兵)。
// 新增枚举值时同步在 kAllToplevelEvents 与两个映射函数补条目 —
// hgs_event_map_test 的覆盖率检查会抓漏网。
inline constexpr ToplevelEventType kAllToplevelEvents[] = {
    ToplevelEventType::Created,
    ToplevelEventType::Destroyed,
    ToplevelEventType::PopupHide,
    ToplevelEventType::PopupMove,
    ToplevelEventType::PopupShow,
    ToplevelEventType::PopupResize,
    ToplevelEventType::Argb,
    ToplevelEventType::Resize,
    ToplevelEventType::Surface,
    ToplevelEventType::Title,
    ToplevelEventType::Limits,
    ToplevelEventType::Maximized,
    ToplevelEventType::Unmaximized,
    ToplevelEventType::Fullscreen,
    ToplevelEventType::Unfullscreen,
    ToplevelEventType::Minimized,
    ToplevelEventType::Restored,
    ToplevelEventType::MoveStart,
    ToplevelEventType::MoveEnd,
    ToplevelEventType::Modal,
    ToplevelEventType::DesktopRoot,
};

inline constexpr uint32_t kToplevelEventCount =
    sizeof(kAllToplevelEvents) / sizeof(kAllToplevelEvents[0]);

} // namespace hgs
