#pragma once
// ============================================================================
// hgs_window_policy.h — 窗口策略扩展点 (阶段 2 W1)。
//
// 默认 FloatingWindowPolicy = 桌面式浮动窗口 (创建/聚焦/叠放/状态/关闭
// 透传); WineHua 可用 WineFloatingPolicy 覆盖任务栏/桌面 root/explorer
// 特判, BetterSH 可覆盖 UI_SCALE/终端窗口偏好, 不改 core。
//
// 调用线程: 由 HgsWindowManager 在自身锁外调用 (策略可回调 manager 查询,
// 但不得重入同一窗口的修改方法 — 与现有 ToplevelManager 锁纪律一致)。
// ============================================================================

#include "hgs_export.h"
#include "hgs_window_types.h"

namespace hgs {

class HGS_API IWindowPolicy {
public:
    virtual ~IWindowPolicy() = default;

    // 创建前可修改初始几何/装饰/可见性; 返回后 manager 落库并通知 host。
    virtual void OnCreated(WindowSpec& spec) = 0;

    virtual void OnRaise(WindowId id) = 0;
    virtual void OnFocus(WindowId id) = 0;
    virtual void OnStateRequest(WindowId id, WindowState state) = 0;
    virtual void OnGeometryRequest(WindowId id, const WindowGeometry& geometry) = 0;

    // false = 拒绝关闭 (例如模态确认中); manager 不会调 host.RequestClose,
    // 但仍会向宿主发 CloseRequested 事件供 UI 决策。
    virtual bool OnCloseRequest(WindowId id) = 0;
};

// 默认策略: 全部透传 (PerToplevel 模式下 OHOS 系统窗口管理器负责最终
// 叠放/移动/缩放; core 只维护逻辑顺序与焦点)。
class HGS_API FloatingWindowPolicy : public IWindowPolicy {
public:
    void OnCreated(WindowSpec& spec) override;
    void OnRaise(WindowId id) override;
    void OnFocus(WindowId id) override;
    void OnStateRequest(WindowId id, WindowState state) override;
    void OnGeometryRequest(WindowId id, const WindowGeometry& geometry) override;
    bool OnCloseRequest(WindowId id) override;
};

} // namespace hgs
