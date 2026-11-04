#pragma once
// ============================================================================
// hgs_window_manager.h — HGS 窗口管理器 core (阶段 2 W1)。
//
// 职责: 持有宿主窗口 spec 表 + 逻辑 z-order + 焦点, 编排 policy/host,
// 向宿主发 WindowEvent。不创建 Ability、不调 OHOS window API、不解析
// 项目路径 (phase0 §15.2)。
//
// 与 Wayland 的关系: 运行期几何/状态/z-order 的权威仍是 ToplevelManager
// (ToplevelState); 本类只维护宿主侧 spec 与逻辑顺序, 阶段 2 集成时由
// HandleToplevelEvent 把 bus 事件翻译为 OnWindowCreated/... 调用, 不复制状态。
//
// 线程: 方法可从 wl 线程 (事件) 或 UI 线程 (host 回执) 调用; 内部 mutex
// 只保护表/z-order/焦点, host/policy/eventSink 调用都在锁外。
// ============================================================================

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "hgs_export.h"
#include "hgs_window_host.h"
#include "hgs_window_policy.h"
#include "hgs_window_types.h"

namespace hgs {

class HGS_API HgsWindowManager {
public:
    using EventSink = std::function<void(const WindowEvent&)>;

    HgsWindowManager();
    ~HgsWindowManager();

    HgsWindowManager(const HgsWindowManager&) = delete;
    HgsWindowManager& operator=(const HgsWindowManager&) = delete;

    // policy 为空时使用 FloatingWindowPolicy (默认构造)。不接管 host 所有权。
    void SetPolicy(std::unique_ptr<IWindowPolicy> policy);
    void SetHost(IWindowHost* host);
    void SetEventSink(EventSink sink);

    // -- Wayland/上层事件入口 (wl 线程) --
    void OnWindowCreated(WindowSpec spec);
    void OnWindowDestroyed(WindowId id);
    void OnTitleChanged(WindowId id, const std::string& title);
    void OnClientGeometryChanged(WindowId id, const WindowGeometry& geometry);
    void OnClientStateChanged(WindowId id, WindowState state);

    // -- host 回执 (UI 线程) --
    void NotifyHostGeometry(WindowId id, const WindowGeometry& geometry);
    void NotifyHostFocus(WindowId id, bool focused);
    void NotifyHostState(WindowId id, WindowState state);
    void NotifyHostClosed(WindowId id);   // Ability 创建失败/窗口被系统销毁

    // -- 策略动作 (宿主 UI 调用) --
    void Raise(WindowId id);
    void Focus(WindowId id);
    void RequestMove(WindowId id);
    void RequestResize(WindowId id, uint32_t edges);
    void RequestClose(WindowId id);
    void SetVisible(WindowId id, bool visible);

    // -- 查询 (快照) --
    bool GetWindow(WindowId id, WindowSpec* out) const;
    std::vector<WindowSpec> ListWindows() const;
    std::vector<WindowId> ZOrder() const;
    WindowId Focused() const;
    size_t WindowCount() const;

private:
    // 锁外调用 helper (调用前把需要的数据拷出)
    void Emit(const WindowEvent& event);
    void EmitState(WindowEventType type, const WindowSpec& spec, bool focused,
                   uint32_t edges = 0);

    mutable std::mutex mutex_;
    std::unordered_map<WindowId, WindowSpec> windows_;
    std::vector<WindowId> zOrder_;
    WindowId focused_ = 0;
    std::unique_ptr<IWindowPolicy> policy_;
    IWindowHost* host_ = nullptr;
    EventSink eventSink_;
};

} // namespace hgs
