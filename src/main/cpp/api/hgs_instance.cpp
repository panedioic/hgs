// hgs/src/main/cpp/api/hgs_instance.cpp — HgsInstance 门面实现 (阶段 0, P0.3)
//
// 唯一允许依赖平台/现有单例的 api 实现文件 (phase0 §4.1: 头文件零平台依赖,
// 实现文件允许)。全部方法转发到 WaylandServer/PluginManager/InputManager/
// PointerExtras 既有单例 — 行为与旧 NAPI 直连路径逐项一致 (phase0 §5 映射表)。
//
// 加锁约定: Impl::mutex 只保护 host_/inputBackend_/generation 表/lastError_/
// socketPath_, 不跨任何转发调用持锁 (转发目标有自己的锁域/线程域)。

#include "hgs_instance.h"

#include <mutex>
#include <unordered_map>
#include <utility>

// 项目统一日志器 (logger/logger.h): stderr/沙箱文件/hilog 三 sink,
// printf 风格 — api 实现不直接调 hilog, 与 ets/NAPI 侧 logger 接入同款约定
#include "logger/logger.h"

#include "compositor/wayland_server.h"
#include "bridge/plugin_manager.h"
#include "input/input_manager.h"
#include "input/pointer_extras.h"
#include "hgs_endpoint.h"
#include "include/hgs/service.h"

namespace hgs {

struct HgsInstance::Impl {
    std::mutex mutex;
    IHgsHost* host = nullptr;                 // 不接管所有权, 宿主负责生命周期
    InputBackend inputBackend = InputBackend::Auto;
    SurfaceGeneration nextGeneration = 0;     // 单调递增; 0 恒为无效值
    // toplevelId -> (token, generation): 当前经 AttachSurface 存活的绑定
    std::unordered_map<ToplevelId, std::pair<SurfaceToken, SurfaceGeneration>> surfaces;
    std::string lastError;                    // 失败写入, 成功不清空 (契约)
    std::string socketPath;                   // 最近一次 Start 的路径
    HgsWindowManager windowManager;           // 阶段 2: 窗口管理器 core

    void SetError(const char* msg) {
        {
            std::lock_guard<std::mutex> lk(mutex);
            lastError = msg;
        }
        LOGW("[HGS-API] %s", msg);
    }

    IHgsHost* HostSnapshot() {
        std::lock_guard<std::mutex> lk(mutex);
        return host;
    }

    // -- 阶段 2 W2a: Wayland 事件 → 窗口管理器 (wl 线程) --
    // 关键约束: PostToplevelEvent 的调用点可能已持 toplevelMgr_ 锁
    // (wl_core.cpp TryAutoRestoreLocked 分支注释明确 "PostToplevelEvent 不碰
    // 该锁") — 本适配层因此**不得**再取 tmgr 锁, 只读 SurfaceData (wl 线程
    // 独占, 无锁) 的 committed 快照。几何/状态权威仍是 ToplevelManager,
    // 由后续事件增量更新; 不解析 JSON。
    SurfaceData* FindSurfaceData(ToplevelId id) {
        ToplevelManager& tmgr = WaylandServer::GetInstance()->GetToplevelManager();
        wl_resource* surf = tmgr.GetSurfaceForToplevel(id);  // 独立 mutex
        if (surf == nullptr) return nullptr;
        return static_cast<SurfaceData*>(wl_resource_get_user_data(surf));
    }

    WindowSpec BuildWindowSpec(ToplevelId id) {
        WindowSpec spec;
        spec.id = id;
        SurfaceData* sd = FindSurfaceData(id);
        if (sd == nullptr) return spec;
        spec.clientPid = sd->clientPid;
        spec.title = sd->title;
        spec.appId = sd->appId;
        spec.minWidth = sd->hasSizeLimits ? sd->minWidth : 0;
        spec.minHeight = sd->hasSizeLimits ? sd->minHeight : 0;
        spec.maxWidth = sd->hasSizeLimits ? sd->maxWidth : 0;
        spec.maxHeight = sd->hasSizeLimits ? sd->maxHeight : 0;
        spec.geometry.x = sd->committed.screenX;
        spec.geometry.y = sd->committed.screenY;
        spec.geometry.width = sd->w;
        spec.geometry.height = sd->h;
        return spec;
    }

    WindowGeometry ReadGeometry(ToplevelId id) {
        WindowGeometry g;
        SurfaceData* sd = FindSurfaceData(id);
        if (sd == nullptr) return g;
        g.x = sd->committed.screenX;
        g.y = sd->committed.screenY;
        g.width = sd->w;
        g.height = sd->h;
        return g;
    }

    std::string ReadTitle(ToplevelId id) {
        SurfaceData* sd = FindSurfaceData(id);
        return sd != nullptr ? sd->title : std::string();
    }

    void HandleWindowEvent(ToplevelId id, ToplevelEventType type) {
        switch (type) {
            case ToplevelEventType::Created:
                windowManager.OnWindowCreated(BuildWindowSpec(id));
                break;
            case ToplevelEventType::Destroyed:
                windowManager.OnWindowDestroyed(id);
                break;
            case ToplevelEventType::Title:
                windowManager.OnTitleChanged(id, ReadTitle(id));
                break;
            case ToplevelEventType::Resize:
                windowManager.OnClientGeometryChanged(id, ReadGeometry(id));
                break;
            case ToplevelEventType::Minimized:
                windowManager.OnClientStateChanged(id, WindowState::Minimized);
                break;
            case ToplevelEventType::Maximized:
                windowManager.OnClientStateChanged(id, WindowState::Maximized);
                break;
            case ToplevelEventType::Fullscreen:
                windowManager.OnClientStateChanged(id, WindowState::Fullscreen);
                break;
            case ToplevelEventType::Restored:
            case ToplevelEventType::Unmaximized:
            case ToplevelEventType::Unfullscreen:
                windowManager.OnClientStateChanged(id, WindowState::Normal);
                break;
            case ToplevelEventType::MoveStart:
                windowManager.RequestMove(id);
                break;
            // Surface (物理 surface 尺寸) 不是客户端内容尺寸; MoveEnd/Popup*/
            // Modal/DesktopRoot/Argb/Limits 等 W2a 暂不产生窗口事件 — 避免
            // 在可能持锁的事件回调里做额外查询 (popup/modal 由后续 W2b 处理)。
            default:
                break;
        }
    }
};

HgsInstance::HgsInstance() : impl_(std::make_unique<Impl>()) {}
HgsInstance::~HgsInstance() = default;

HgsInstance& HgsInstance::Instance() {
    static HgsInstance s;
    return s;
}

// ----------------------------------------------------------------------------
// 生命周期
// ----------------------------------------------------------------------------

bool HgsInstance::Start(const HgsConfig& config) {
    service::Config requested;
    const auto endpoint = SplitSocketPath(config.socketPath);
    requested.runtimeDir = endpoint.directory;
    requested.waylandSocket = endpoint.displayName;
    requested.output = config.output;
    requested.composition = config.composition;
    requested.inputBackend = config.inputBackend;
    requested.setProcessEnvironment = config.setProcessEnvironment;
    requested.legacyWaylandFeatures = config.features;
    const auto result = service::DefaultRuntime().Start(requested);
    if (!result.ok) impl_->SetError(result.error.message.c_str());
    return result.ok;
}

bool HgsInstance::StartBackend(const HgsConfig& config) {
    const SocketEndpoint ep = SplitSocketPath(config.socketPath);
    if (!ep.valid) {
        impl_->SetError("Start: invalid socketPath (absolute path with basename required)");
        return false;
    }

    auto* impl = impl_.get();
    {
        std::lock_guard<std::mutex> lk(impl->mutex);
        impl->socketPath = config.socketPath;
        impl->inputBackend = config.inputBackend;
    }

    WaylandServer* ws = WaylandServer::GetInstance();
    // 无条件安装适配 sink (phase0 §4.7): lambda 内实时读 host_, 允许
    // SetHost 在 Start 前后调用。注意 bus 是单 sink — 本安装是 bus 的唯一
    // 装配点 (阶段 1 P1.2 起 sink 类型化, NAPI 经 IHgsHost 消费)。
    ws->SetStateCallback([impl](const char* state) {
        if (IHgsHost* host = impl->HostSnapshot()) {
            host->OnStateChanged(std::string_view(state != nullptr ? state : ""));
        }
    });
    ws->SetToplevelCallback([this, impl](uint32_t id, ToplevelEventType type, const char* json) {
        // 阶段 2 W2a: 先喂窗口管理器 (读 ToplevelManager 权威状态, 不解析 JSON)
        impl->HandleWindowEvent(id, type);
        if (serviceObserver_) serviceObserver_(id, type, json);
        if (IHgsHost* host = impl->HostSnapshot()) {
            ToplevelEvent evt;
            evt.id = id;
            evt.type = type;
            evt.json = std::string_view(json != nullptr ? json : "{}");
            host->OnToplevelEvent(evt);
        }
    });

    // 默认值不下发 (红线, phase0 §1.3): output 0x0 / Unspecified 保持宿主
    // 在 Start 前经 SetOutputSize/applyModePolicy 设置的现状。
    if (config.output.width > 0 && config.output.height > 0) {
        ws->SetOutputSize(config.output.width, config.output.height);
    }
    if (config.composition != CompositionMode::Unspecified) {
        ws->SetDesktopMode(config.composition == CompositionMode::DesktopRoot);
    }
    // features 阶段 1 仍仅记录 (恒开协议不变); setProcessEnvironment 已生效:
    // false = 不污染宿主进程全局环境, 宿主用 ClientEnvironment() 传子进程。
    const bool ok = ws->Start(config.socketPath, config.setProcessEnvironment);
    if (!ok) impl->SetError("Start: WaylandServer::Start failed");
    return ok;
}

void HgsInstance::Stop() { service::DefaultRuntime().Stop(); }

void HgsInstance::StopBackend() {
    auto* impl = impl_.get();
    // 先停事件循环 (DestroyAllToplevels → bus 停止派发) 再清表, 保证
    // 适配 lambda 不再访问已清空的 generation 表 (phase0 §4.7)
    WaylandServer::GetInstance()->Stop();
    std::lock_guard<std::mutex> lk(impl->mutex);
    impl->surfaces.clear();
}

bool HgsInstance::IsRunning() const {
    return WaylandServer::GetInstance()->IsRunning();
}

std::string HgsInstance::LastError() const {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    return impl_->lastError;
}

std::string HgsInstance::WaylandDisplayName() const {
    std::string path;
    {
        std::lock_guard<std::mutex> lk(impl_->mutex);
        path = impl_->socketPath;
    }
    const SocketEndpoint ep = SplitSocketPath(path);
    return IsRunning() && ep.valid ? ep.displayName : std::string();
}

std::vector<std::string> HgsInstance::ClientEnvironment() const {
    std::string path;
    {
        std::lock_guard<std::mutex> lk(impl_->mutex);
        path = impl_->socketPath;
    }
    return IsRunning() ? BuildClientEnvironment(path) : std::vector<std::string>{};
}

void HgsInstance::SetHost(IHgsHost* host) {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    impl_->host = host;
}

// ----------------------------------------------------------------------------
// 会话配置
// ----------------------------------------------------------------------------

void HgsInstance::SetOutputSize(int32_t w, int32_t h) {
    WaylandServer::GetInstance()->SetOutputSize(w, h);
}

void HgsInstance::SetDesktopMode(bool on) {
    WaylandServer::GetInstance()->SetDesktopMode(on);
}

void HgsInstance::SetInputBackend(InputBackend backend) {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    impl_->inputBackend = backend;
}

InputBackend HgsInstance::GetInputBackend() const {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    return impl_->inputBackend;
}

ToplevelId HgsInstance::DesktopRootId() const {
    return WaylandServer::GetInstance()->GetDesktopRootToplevelId();
}

// ----------------------------------------------------------------------------
// Surface 生命周期 (generation 表 + PluginManager 转发)
// ----------------------------------------------------------------------------

SurfaceGeneration HgsInstance::AttachSurface(ToplevelId id, SurfaceToken token) {
    auto* impl = impl_.get();
    if (!PluginManager::GetInstance()->CreateRenderer(id, static_cast<int64_t>(token))) {
        impl->SetError("AttachSurface: CreateRenderer failed");
        return 0;
    }
    std::lock_guard<std::mutex> lk(impl->mutex);
    const SurfaceGeneration gen = ++impl->nextGeneration;
    impl->surfaces[id] = {token, gen};
    return gen;
}

bool HgsInstance::ResizeSurface(ToplevelId id, int32_t w, int32_t h, SurfaceGeneration gen) {
    auto* impl = impl_.get();
    if (gen != 0) {
        // 非 0 须匹配当前绑定; 不匹配 = stale 事件 (旧窗口的迟到的 resize),
        // 直接丢弃 (phase0 §4.6 generation 动机)
        std::lock_guard<std::mutex> lk(impl->mutex);
        const auto it = impl->surfaces.find(id);
        if (it == impl->surfaces.end() || it->second.second != gen) {
            LOGW("[HGS-API] ResizeSurface stale gen=%u id=%u dropped", gen, id);
            return false;
        }
    }
    PluginManager::GetInstance()->ResizeRenderer(id, w, h);
    return true;
}

bool HgsInstance::DetachSurface(ToplevelId id, SurfaceGeneration gen) {
    auto* impl = impl_.get();
    if (gen != 0) {
        std::lock_guard<std::mutex> lk(impl->mutex);
        const auto it = impl->surfaces.find(id);
        if (it == impl->surfaces.end() || it->second.second != gen) {
            LOGW("[HGS-API] DetachSurface stale gen=%u id=%u dropped", gen, id);
            return false;
        }
    }
    PluginManager::GetInstance()->DestroyToplevel(id);
    std::lock_guard<std::mutex> lk(impl->mutex);
    impl->surfaces.erase(id);
    return true;
}

// ----------------------------------------------------------------------------
// 输入 (逐字段转发, phase0 §5 映射表)
// ----------------------------------------------------------------------------

void HgsInstance::SendPointerEvent(const PointerEvent& event) {
    InputManager::GetInstance()->SendPointerEvent(
        event.hintToplevel, static_cast<int>(event.action),
        event.x, event.y, event.button,
        event.rawDeltaX, event.rawDeltaY, event.fromMouse);
}

void HgsInstance::SendKeyEvent(const KeyEvent& event) {
    InputManager::GetInstance()->SendKeyEvent(
        event.toplevelId, event.evdevCode, event.pressed);
}

void HgsInstance::SendScrollEvent(const ScrollEvent& event) {
    InputManager::GetInstance()->SendScrollEvent(
        event.hintToplevel, event.axis, event.value, event.scrollStep,
        event.x, event.y);
}

// ----------------------------------------------------------------------------
// 宿主窗口操作
// ----------------------------------------------------------------------------

void HgsInstance::NotifyToplevelResize(ToplevelId id, int32_t w, int32_t h, bool resizing) {
    WaylandServer::GetInstance()->NotifyToplevelResize(id, w, h, resizing);
}

void HgsInstance::SendToplevelClose(ToplevelId id) {
    WaylandServer::GetInstance()->SendToplevelClose(id);
}

void HgsInstance::RaiseToplevel(ToplevelId id) {
    // 用户显式操作路径 (任务栏/窗口点击): 已 fullscreen 的目标重新取
    // 全屏优先级号 — 与现 raiseToplevel NAPI 语义一致 (wayland_server.cpp)
    WaylandServer::GetInstance()->RaiseToplevel(id, true);
}

void HgsInstance::SetToplevelVisible(ToplevelId id, bool visible) {
    // 与现 setToplevelVisible NAPI 组合语义一致: 输入可见性 + 显示时 restore
    InputManager::GetInstance()->SetToplevelVisible(id, visible);
    if (visible) {
        WaylandServer::GetInstance()->SetToplevelRestored(id);
    }
}

void HgsInstance::DestroyToplevel(ToplevelId id) {
    // 与现 destroyToplevel NAPI 语义一致: renderer 收尾 + 相对模式锁定
    // 兜底释放 (游戏卡死时 wine 不响应 send_close, 解锁回调永不来的兜底)
    PluginManager::GetInstance()->DestroyToplevel(id);
    PointerExtras::GetInstance()->ReleaseLockForToplevel(id);
    std::lock_guard<std::mutex> lk(impl_->mutex);
    impl_->surfaces.erase(id);
}

ToplevelId HgsInstance::FindToplevelAt(int32_t x, int32_t y) const {
    return WaylandServer::GetInstance()->FindToplevelAt(x, y);
}

// ----------------------------------------------------------------------------
// 过渡兼容: pending 队列
// ----------------------------------------------------------------------------

void HgsInstance::SetPendingToplevel(ToplevelId id) {
    PluginManager::GetInstance()->SetPendingToplevel(id);
}

ToplevelId HgsInstance::TakePendingToplevel() {
    return PluginManager::GetInstance()->DequeuePendingToplevel();
}

void HgsInstance::CancelPendingToplevel(ToplevelId id) {
    PluginManager::GetInstance()->CancelPendingToplevel(id);
}

HgsWindowManager& HgsInstance::WindowManager() {
    return impl_->windowManager;
}

} // namespace hgs
