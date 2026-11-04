// api/hgs_window_manager.cpp — HGS 窗口管理器 core (阶段 2 W1)
//
// 锁纪律: mutex_ 只保护 windows_/zOrder_/focused_/policy_/host_/eventSink_;
// policy/host/sink 调用全部在锁外 (先快照数据)。policy 回调不得重入同一
// 窗口的修改方法 (与核心 ToplevelManager 锁纪律一致)。

#include "hgs_window_manager.h"

#include <algorithm>
#include <utility>

namespace hgs {

namespace {
// 从 z-order 移除 (保持其余顺序)。
void EraseFromZOrder(std::vector<WindowId>& order, WindowId id) {
    order.erase(std::remove(order.begin(), order.end(), id), order.end());
}
} // namespace

HgsWindowManager::HgsWindowManager()
    : policy_(std::make_unique<FloatingWindowPolicy>()) {}

HgsWindowManager::~HgsWindowManager() = default;

void HgsWindowManager::SetPolicy(std::unique_ptr<IWindowPolicy> policy) {
    std::lock_guard<std::mutex> lk(mutex_);
    policy_ = policy ? std::move(policy) : std::make_unique<FloatingWindowPolicy>();
}

void HgsWindowManager::SetHost(IWindowHost* host) {
    std::lock_guard<std::mutex> lk(mutex_);
    host_ = host;
}

void HgsWindowManager::SetEventSink(EventSink sink) {
    std::lock_guard<std::mutex> lk(mutex_);
    eventSink_ = std::move(sink);
}

// ---------------------------------------------------------------------------
// 事件入口
// ---------------------------------------------------------------------------

void HgsWindowManager::OnWindowCreated(WindowSpec spec) {
    if (spec.id == 0) return;

    IWindowPolicy* policy = nullptr;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        policy = policy_.get();
    }
    if (policy != nullptr) policy->OnCreated(spec);  // 锁外; 可改初始几何

    IWindowHost* host = nullptr;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        windows_[spec.id] = spec;
        EraseFromZOrder(zOrder_, spec.id);
        zOrder_.push_back(spec.id);
        focused_ = spec.id;
        host = host_;
    }
    // host 创建 (默认 ArkTS host 由此起 Ability); EventSink 只作观察者通知,
    // 不负责创建 — 避免 host 与 sink 双建窗口 (phase0 §15.5 修订)。
    if (host != nullptr) host->CreateWindow(spec);
    EmitState(WindowEventType::Created, spec, true);
}

void HgsWindowManager::OnWindowDestroyed(WindowId id) {
    if (id == 0) return;
    WindowSpec spec;
    IWindowHost* host = nullptr;
    bool found = false;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = windows_.find(id);
        if (it == windows_.end()) return;
        spec = it->second;
        windows_.erase(it);
        EraseFromZOrder(zOrder_, id);
        if (focused_ == id) focused_ = 0;
        host = host_;
        found = true;
    }
    if (!found) return;
    if (host != nullptr) host->DestroyWindow(id);
    EmitState(WindowEventType::Destroyed, spec, false);
}

void HgsWindowManager::OnTitleChanged(WindowId id, const std::string& title) {
    IWindowHost* host = nullptr;
    WindowSpec spec;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = windows_.find(id);
        if (it == windows_.end()) return;
        it->second.title = title;
        spec = it->second;
        host = host_;
    }
    if (host != nullptr) host->SetTitle(id, title);
    EmitState(WindowEventType::TitleChanged, spec, false);
}

void HgsWindowManager::OnClientGeometryChanged(WindowId id, const WindowGeometry& geometry) {
    IWindowHost* host = nullptr;
    WindowSpec spec;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = windows_.find(id);
        if (it == windows_.end()) return;
        it->second.geometry = geometry;
        spec = it->second;
        host = host_;
    }
    // 客户端内容尺寸变化 → host 调整窗口 (与 BetterSH 现 resize 事件语义一致)
    if (host != nullptr) host->SetGeometry(id, geometry);
    EmitState(WindowEventType::GeometryChanged, spec, false);
}

void HgsWindowManager::OnClientStateChanged(WindowId id, WindowState state) {
    IWindowHost* host = nullptr;
    WindowSpec spec;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = windows_.find(id);
        if (it == windows_.end()) return;
        it->second.state = state;
        spec = it->second;
        host = host_;
    }
    if (host != nullptr) host->SetState(id, state);
    EmitState(WindowEventType::StateChanged, spec, false);
}

// ---------------------------------------------------------------------------
// host 回执 (host 已执行, core 只记账 + 通知观察者, 不再回调 host)
// ---------------------------------------------------------------------------

void HgsWindowManager::NotifyHostGeometry(WindowId id, const WindowGeometry& geometry) {
    WindowSpec spec;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = windows_.find(id);
        if (it == windows_.end()) return;
        it->second.geometry = geometry;
        spec = it->second;
    }
    EmitState(WindowEventType::GeometryChanged, spec, false);
}

void HgsWindowManager::NotifyHostFocus(WindowId id, bool focused) {
    IWindowPolicy* policy = nullptr;
    WindowSpec spec;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = windows_.find(id);
        if (it == windows_.end()) return;
        focused_ = focused ? id : (focused_ == id ? 0 : focused_);
        spec = it->second;
        policy = policy_.get();
    }
    if (focused && policy != nullptr) policy->OnFocus(id);
    EmitState(WindowEventType::FocusChanged, spec, focused);
}

void HgsWindowManager::NotifyHostState(WindowId id, WindowState state) {
    WindowSpec spec;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = windows_.find(id);
        if (it == windows_.end()) return;
        it->second.state = state;
        spec = it->second;
    }
    EmitState(WindowEventType::StateChanged, spec, false);
}

void HgsWindowManager::NotifyHostClosed(WindowId id) {
    // host 创建失败/系统关窗: 回滚 core 状态 (不回调 host.DestroyWindow,
    // 窗口已不在) — 与 OnWindowDestroyed 的区别只在 host 调用
    WindowSpec spec;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = windows_.find(id);
        if (it == windows_.end()) return;
        spec = it->second;
        windows_.erase(it);
        EraseFromZOrder(zOrder_, id);
        if (focused_ == id) focused_ = 0;
    }
    EmitState(WindowEventType::Destroyed, spec, false);
}

// ---------------------------------------------------------------------------
// 策略动作
// ---------------------------------------------------------------------------

void HgsWindowManager::Raise(WindowId id) {
    IWindowPolicy* policy = nullptr;
    IWindowHost* host = nullptr;
    WindowSpec spec;
    bool focused = false;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = windows_.find(id);
        if (it == windows_.end()) return;
        EraseFromZOrder(zOrder_, id);
        zOrder_.push_back(id);
        spec = it->second;
        focused = (focused_ == id);
        policy = policy_.get();
        host = host_;
    }
    if (policy != nullptr) policy->OnRaise(id);
    if (host != nullptr) host->RaiseWindow(id);
    // 事件驱动 host (ArkTS) 据此把 OHOS 窗口带到前台
    EmitState(WindowEventType::RaiseRequested, spec, focused);
}

void HgsWindowManager::Focus(WindowId id) {
    IWindowPolicy* policy = nullptr;
    IWindowHost* host = nullptr;
    WindowSpec spec;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = windows_.find(id);
        if (it == windows_.end()) return;
        focused_ = id;
        spec = it->second;
        policy = policy_.get();
        host = host_;
    }
    if (policy != nullptr) policy->OnFocus(id);
    // 系统窗口聚焦不可直接从 native 设: Raise 让系统把窗口带到前台,
    // 焦点由系统回执 NotifyHostFocus 确认 (PerToplevel 模式)
    if (host != nullptr) host->RaiseWindow(id);
    EmitState(WindowEventType::FocusChanged, spec, true);
}

void HgsWindowManager::RequestMove(WindowId id) {
    IWindowHost* host = nullptr;
    WindowSpec spec;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = windows_.find(id);
        if (it == windows_.end()) return;
        spec = it->second;
        host = host_;
    }
    if (host != nullptr) host->StartMove(id);
    EmitState(WindowEventType::MoveRequested, spec, false);
}

void HgsWindowManager::RequestResize(WindowId id, uint32_t edges) {
    IWindowHost* host = nullptr;
    WindowSpec spec;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = windows_.find(id);
        if (it == windows_.end()) return;
        spec = it->second;
        host = host_;
    }
    if (host != nullptr) host->StartResize(id, edges);
    EmitState(WindowEventType::ResizeRequested, spec, false, edges);
}

void HgsWindowManager::RequestClose(WindowId id) {
    IWindowPolicy* policy = nullptr;
    IWindowHost* host = nullptr;
    WindowSpec spec;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = windows_.find(id);
        if (it == windows_.end()) return;
        spec = it->second;
        policy = policy_.get();
        host = host_;
    }
    const bool allowed = (policy == nullptr) || policy->OnCloseRequest(id);
    // 观察者始终收到请求 (UI 可做确认); host.RequestClose 仅在策略允许时
    EmitState(WindowEventType::CloseRequested, spec, false);
    if (allowed && host != nullptr) host->RequestClose(id);
}

void HgsWindowManager::SetVisible(WindowId id, bool visible) {
    IWindowHost* host = nullptr;
    WindowSpec spec;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = windows_.find(id);
        if (it == windows_.end()) return;
        it->second.visible = visible;
        spec = it->second;
        host = host_;
    }
    if (host != nullptr) host->SetVisible(id, visible);
    EmitState(WindowEventType::VisibleChanged, spec, false);
}

// ---------------------------------------------------------------------------
// 查询
// ---------------------------------------------------------------------------

bool HgsWindowManager::GetWindow(WindowId id, WindowSpec* out) const {
    if (out == nullptr) return false;
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = windows_.find(id);
    if (it == windows_.end()) return false;
    *out = it->second;
    return true;
}

std::vector<WindowSpec> HgsWindowManager::ListWindows() const {
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<WindowSpec> out;
    out.reserve(windows_.size());
    for (const WindowId id : zOrder_) {
        auto it = windows_.find(id);
        if (it != windows_.end()) out.push_back(it->second);
    }
    return out;
}

std::vector<WindowId> HgsWindowManager::ZOrder() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return zOrder_;
}

WindowId HgsWindowManager::Focused() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return focused_;
}

size_t HgsWindowManager::WindowCount() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return windows_.size();
}

// ---------------------------------------------------------------------------
// 事件派发 (锁外)
// ---------------------------------------------------------------------------

void HgsWindowManager::Emit(const WindowEvent& event) {
    EventSink sink;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        sink = eventSink_;
    }
    if (sink) sink(event);
}

void HgsWindowManager::EmitState(WindowEventType type, const WindowSpec& spec,
                                 bool focused, uint32_t edges) {
    WindowEvent event;
    event.type = type;
    event.id = spec.id;
    event.spec = spec;
    event.geometry = spec.geometry;
    event.state = spec.state;
    event.focused = focused;
    event.edges = edges;
    Emit(event);
}

} // namespace hgs
