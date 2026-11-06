#include "wayland_server.h"
#include "input/seat.h"
#include "input/text_input.h"
#include "input/input_manager.h"
#include "input/pointer_extras.h"  // NAPI 段 destroyToplevel 兜底解锁
#include "xdg_shell.h"
#include "xdg_configure.h"
#include "common/fps_counter.h"
#include "compositor/frame/debug_assert.h"
#include "seam/host_hooks.h"
#include "protocols/xdg-shell-server-protocol.h"
#include "protocols/wayland-server-protocol.h"  // HgsRegisterDataDeviceManager (data_device_stub.cpp)

extern "C" void HgsRegisterDataDeviceManager(struct wl_display* display);
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <cmath>
#include <cerrno>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "napi/native_api.h"


extern "C" void RegisterXdgShell(wl_display* display);
extern "C" void RegisterWinehuaToplevel(wl_display* display);
extern "C" void RegisterWlCoreGlobals(wl_display* display);
#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_Server"
#include <hilog/log.h>
#include "bridge/plugin_manager.h"
#include "bridge/napi_host_adapter.h"  // NAPI IHgsHost 适配 (阶段 1 P1.1)
#include "hgs_instance.h"             // 公共门面 (阶段 1: NAPI 业务入口经此转发)

// 核心协议接口表与实现已剥离到 wl_core.cpp (Phase 3 纯搬移)

// -- 单例 --
WaylandServer* WaylandServer::GetInstance() {
    static WaylandServer s;
    return &s;
}

bool WaylandServer::Start(const std::string& socketPath, bool setProcessEnvironment) {
    if (running_) {
        OH_LOG_WARN(LOG_APP, "[WL] already running");
        return true;
    }

    OH_LOG_INFO(LOG_APP, "[WL] Starting compositor, socket=%{public}s", socketPath.c_str());

    // 清理残留 socket
    unlink(socketPath.c_str());

    // 确保 socket 目录存在
    auto pos = socketPath.find_last_of('/');
    std::string dir = socketPath.substr(0, pos);
    std::string name = socketPath.substr(pos + 1);
    int rc = mkdir(dir.c_str(), 0700);
    OH_LOG_INFO(LOG_APP, "[WL] mkdir(%{public}s) = %{public}d, errno=%{public}d",
                dir.c_str(), rc, errno);

    // 环境变量注入 (阶段 1 P1.3): 默认沿用旧行为; false 时宿主经
    // HgsInstance::ClientEnvironment() 显式传给子进程 (WineHua 的
    // wine_env.cpp 已自组 XDG_RUNTIME_DIR/WAYLAND_DISPLAY, 不依赖本进程)
    if (setProcessEnvironment) {
        setenv("XDG_RUNTIME_DIR", dir.c_str(), 1);
    }

    display_ = wl_display_create();
    if (!display_) {
        OH_LOG_ERROR(LOG_APP, "[WL] wl_display_create failed, errno=%{public}d", errno);
        return false;
    }
    OH_LOG_INFO(LOG_APP, "[WL] wl_display created");

    if (wl_display_add_socket(display_, name.c_str()) != 0) {
        OH_LOG_ERROR(LOG_APP, "[WL] wl_display_add_socket(%{public}s) failed, errno=%{public}d",
                     name.c_str(), errno);
        return false;
    }
    OH_LOG_INFO(LOG_APP, "[WL] socket added: %{public}s", name.c_str());

    if (setProcessEnvironment) {
        setenv("WAYLAND_DISPLAY", name.c_str(), 1);
    }

    // 注册 global 对象 (核心协议实现已剥离到 wl_core.cpp)
    RegisterWlCoreGlobals(display_);
    wl_display_init_shm(display_);
    RegisterXdgShell(display_);
    RegisterWinehuaToplevel(display_);
    Seat::GetInstance()->Register(display_);
    // GDK gate 解除: GTK 的 wayland 后端要求 registry 里有
    // wl_data_device_manager 才初始化 seat (否则 get_pointer 永远不被调用,
    // 指针/键盘输入全部静默丢失)。完整剪贴板仍是 P1.4, 此处是 stub。
    HgsRegisterDataDeviceManager(display_);
    // 装配注入 (重构第 6A 步): 输入编排层直接引用子组件 (tmgr/resolver/
    // moveGrab/policy/rootId), 替代 WaylandServer 到业务的转发调用 —
    // 装配在 wl 事件循环启动前 (与 4C1 warpSink 同模式: 之后回调仅在
    // Wayland 线程 / NAPI 线程读, 只读共享, 无新锁; 引用与单例同生命周期,
    // Start 前成员已全构造)。见 input_manager.h BindCompositorDeps。
    // 共享状态引用指向 session_ 字段 (重构第 6B 步: 存储自根字段迁移到
    // DesktopSessionState POD, 注入形态/时序不变 — 见 input_manager.h 注释)
    InputManager::GetInstance()->BindCompositorDeps(toplevelMgr_, inputResolver_,
                                                    moveGrab_, session_.policy,
                                                    session_.desktopRootToplevelId);
    InputManager::GetInstance()->Initialize(display_);
    OH_LOG_INFO(LOG_APP, "[WL] globals registered (compositor+shm+xdg+subcompositor+viewporter+output+seat+input)");

    running_ = true;
    session_.firstFrame = false;   // 存储 = 会话共享 POD (重构第 6B 步)
    thread_ = std::thread(&WaylandServer::EventLoop, this);
    OH_LOG_INFO(LOG_APP, "[WL] compositor started OK");
    return true;
}

void WaylandServer::Stop() {
    if (!running_) return;
    running_ = false;
    // 主动清空全部 toplevel: stopAll 强杀 Wine 进程时, wl_display_terminate
    // 在 client 断开事件被 dispatch 之前终止事件循环, 依赖断开事件触发的
    // OnToplevelDestroyed 不会执行 → ToplevelManager 残留旧 toplevel (重启后
    // 旧窗口画面共存、占 zOrder、不响应事件)。此处显式遍历逐个收口并补发
    // destroyed 通知给 ArkTS (让 Wine 子窗口正确关闭)
    DestroyAllToplevels();
    InputManager::GetInstance()->Shutdown();
    Seat::GetInstance()->Unregister();
    TextInput::GetInstance()->Unregister();
    if (display_) wl_display_terminate(display_);
    if (thread_.joinable()) thread_.join();
    if (display_) {
        wl_display_destroy(display_);
        display_ = nullptr;
    }
    session_.firstFrame = false;   // 存储 = 会话共享 POD (重构第 6B 步)
}

void WaylandServer::EventLoop() {
    int tick = 0;
    while (running_) {
        wl_event_loop* loop = wl_display_get_event_loop(display_);
        int ret = wl_event_loop_dispatch(loop, 50); // 50ms timeout
        if (ret < 0) {
            OH_LOG_ERROR(LOG_APP, "[WL-ERR] event loop error: %{public}s (errno=%{public}d)",
                         strerror(errno), errno);
        }
        wl_display_flush_clients(display_);  // dispatch 可能写数据, 之后 flush

        // 每 30 秒输出一次资源快照。按 dispatch 迭代计数会在高事件率下变成
        // 每 130ms 一行 (GTK 动画 120fps 每帧一次 dispatch) → 刷爆 hilog 且
        // 把别的日志挤出环形缓冲。改用真实墙钟间隔。
        {
            static int64_t lastStatMs = 0;
            int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            if (nowMs - lastStatMs >= 30000) {
                lastStatMs = nowMs;
                size_t renderers = PluginManager::GetInstance()->GetRendererCount();
                OH_LOG_INFO(LOG_APP, "[WL-STAT] toplevels=%{public}zu surfaces=%{public}zu renderers=%{public}zu",
                            toplevelMgr_.ToplevelResourceCount(), toplevelMgr_.ToplevelSurfaceCount(), renderers);
            }
        }
    }
}

void WaylandServer::DestroyAllToplevels() {
    // 收集 id (锁内), 逐个收口 (锁外): OnToplevelDestroyed 内部重新拿锁,
    // PostToplevelEvent 发 ArkTS 事件不应持 compositor 锁。模拟正常 client
    // 断开路径 (wl_core.cpp surface_destroy) 的清理 + destroyed 通知 —
    // stopAll 强杀 Wine 后该路径不执行, 导致 toplevel 残留 + ArkTS 子窗口不关。
    // 桌面 root 在内时 OnToplevelDestroyed 内部会 ResetSessionState (幂等)。
    std::vector<uint32_t> ids;
    {
        auto lk = toplevelMgr_.Lock();
        for (const auto& [id, state] : toplevelMgr_.toplevels()) {
            (void)state;
            ids.push_back(id);
        }
    }
    OH_LOG_INFO(LOG_APP, "[MW] DestroyAllToplevels: %{public}zu toplevel(s) teardown",
                ids.size());
    for (uint32_t id : ids) {
        OnToplevelDestroyed(id);
        PostToplevelEvent(id, ToplevelEventType::Destroyed);
    }
}



void WaylandServer::RaiseToplevel(uint32_t id, bool userInitiated) {
    auto lk = toplevelMgr_.Lock();
    toplevelMgr_.RaiseToplevel(id);
    // 全屏优先级: 仅"用户显式 raise (任务栏/窗口点击经 ArkTS 发起) 且目标
    // 当前已 fullscreen"时重新取号 — 两个全屏窗口互相切换靠它;
    // tl_set_fullscreen 批处理里的 raise 不重新取号 (显示模式切换会批量连带
    // 标记旧窗口, 重新取号即退回到达顺序决定论); 窗口化窗口不重新取号
    // (点过 notepad 不该让它日后被连带标全屏时盖过游戏)。
    // 注意: AddToZOrder 对首次入列的窗口会取初始号 (红警2 set_fullscreen
    // 先于首帧 commit 时经此路径取号), 与"不重新取号"不冲突。
    // 见 ToplevelState::fsPriority
    if (userInitiated) {
        if (const auto* rst = toplevelMgr_.FindToplevelLocked(id); rst && rst->IsFullscreen())
            toplevelMgr_.BumpFsPriorityLocked(id);
    }
    // 任务栏始终在顶层 (app_id == "explorer.exe.taskbar");
    // 全屏窗口例外 — 游戏全屏必须压过任务栏 (规则实现收口在 ToplevelManager::PinToTop)
    toplevelMgr_.PinToTop(session_.taskbarId, id);   // 存储 = 会话共享 POD (重构第 6B 步)
    MarkDesktopRootDirtyLocked();
}

// -- 交互式窗口移动 (xdg_toplevel.move) --
void WaylandServer::StartMoveGrab(uint32_t toplevelId, uint32_t serial) {
    // 用最近一次注入的全局指针位置立即算固定 grab 偏移:
    // 绝对定位后窗口每帧由 全局坐标−偏移 决定, 不依赖消费时刻的 st->x
    moveGrab_.StartMoveGrab(toplevelMgr_, toplevelId, serial,
                            wl_fixed_to_int(InputManager::GetInstance()->GetLastGlobalPointerX()),
                            wl_fixed_to_int(InputManager::GetInstance()->GetLastGlobalPointerY()));
    if (Policy().OhosWindowPerToplevel()) {
        PostToplevelEvent(toplevelId, ToplevelEventType::MoveStart);
    }
}

void WaylandServer::EndMoveGrab() {
    uint32_t tl = moveGrab_.GetToplevelId();
    moveGrab_.EndMoveGrab(toplevelMgr_);
    if (Policy().OhosWindowPerToplevel() && tl != 0) {
        PostToplevelEvent(tl, ToplevelEventType::MoveEnd);
    }
}

bool WaylandServer::ProcessMoveGrabMotion(wl_fixed_t wx, wl_fixed_t wy) {
    // 注意: InputManager 在 grab 激活时注入的是桌面全局坐标 (wl_fixed_t),
    // 这里截断为整数全局坐标交 MoveGrabHandler 绝对定位
    if (!moveGrab_.ProcessMoveGrabMotion(toplevelMgr_, wl_fixed_to_int(wx),
                                         wl_fixed_to_int(wy))) return false;
    MarkDesktopRootDirtyLocked();
    return true;
}

void WaylandServer::PostToplevelEvent(uint32_t id, ToplevelEventType evt,
                                      const std::string& json) {
    // 事件收口 (重构第 5D 步): 抑制门禁 + [MW] FireToplevel 日志 + 主事件通道
    // (ArkTS toplevel 回调) 全部在 ToplevelEventBus::Post 内 — 事件名经
    // ToplevelEventName 映射为旧字符串 (逐字), 日志文本/顺序/抑制条件逐字;
    // NAPI 通道移出 compositor 核心 (旧本函数直调 gStateTsfn, 见下方旁路)。
    toplevelEventBus_.Post(id, evt, json);
    /* 桌面根出现 = 引擎消息通道的 evt:desktop-ready: LaunchPadMode 的 15s
     * root 等待超时后状态机停在 ready-degraded, 靠这个补票升级为正式 ready。
     * 挂在统一的 toplevel 事件收口点而非 LaunchPadMode 等待循环 — root 在任意
     * 时刻出现 (含慢设备超时后姗姗来迟) 都能补发; ArkTS 只在 degraded 态消费,
     * 其余情况 (正常启动 root 先到 / root 重建) 为无害空转。
     * 重构第 5D 步: 旧实现在此直调 napi_call_threadsafe_function(gStateTsfn)
     * (PLAN §2.2 点名的 NAPI 通道泄漏), 现经会话状态通道 FireState →
     * stateCb_ (napi_init SetStateCallback 注入的转发) 投递 — 消息与通道
     * 不变 (同一 "evt:desktop-ready" + WLState TSFN + block 投递), 判空
     * 语义等价 (napi 侧 lambda 内 if (gStateTsfn), 与旧 if (gStateTsfn)
     * 一致), compositor 核心不再接触 napi 符号。 */
    if (evt == ToplevelEventType::DesktopRoot) {
        // 桌面 root 首次出现: 把当前 running 的进程标记为桌面 shell 基础进程
        // (desktop + 桌面出现前加入的 explorer 等), ArkTS 据此隐藏"结束"防误操作。
        hgs::seam::GetHostLifecycleHooks().OnDesktopRootAppeared();
        FireState("evt:desktop-ready");
    }
}

void WaylandServer::RegisterToplevelResource(uint32_t toplevelId, wl_resource* tl) {
    toplevelMgr_.RegisterToplevelResource(toplevelId, tl);
    OH_LOG_INFO(LOG_APP, "[MW] RegisterToplevelResource id=%{public}u tl=%{public}p", toplevelId, tl);
}

void WaylandServer::UnregisterToplevelResource(uint32_t toplevelId) {
    auto* tl = toplevelMgr_.FindToplevelResource(toplevelId);
    if (tl) {
        OH_LOG_INFO(LOG_APP, "[MW] UnregisterToplevelResource id=%{public}u tl=%{public}p (Wine destroyed toplevel)",
                    toplevelId, tl);
    }
    toplevelMgr_.UnregisterToplevelResource(toplevelId);
}

void WaylandServer::OnToplevelDestroyed(uint32_t toplevelId) {
    std::vector<uint32_t> cascadePopups;
    bool wasDesktopRoot = false;
    {
        auto lk = toplevelMgr_.Lock();
        // WineHua: 先清 modal 组员关系 (EraseModalLocked 依赖 ToplevelState 的
        // owner 回查, 必须先于 EraseToplevelLocked; 组员不在 z-order, 漏清会让
        // BuildLayerListLocked 展开出已亡窗口)。对称清理"它作为 owner 的组":
        // owner 先亡 (wine 关闭属主窗口) 且模态对话框尚存活时, 组员带着亡
        // owner 指针残留在 modalOf_, 其 lane 消失 → 对话框从合成/命中列表
        // 消失且不可恢复
        toplevelMgr_.EraseModalLocked(toplevelId);
        for (uint32_t m : toplevelMgr_.ModalListLocked(toplevelId))
            toplevelMgr_.EraseModalLocked(m);
        toplevelMgr_.EraseToplevelLocked(toplevelId);
        // 会话状态读写经 DesktopSessionState (重构第 6B 步: 旧为宿主私有字段,
        // 清空点/时机/锁域逐字不变)
        if (session_.pendingDesktopRootToplevelId == toplevelId)
            session_.pendingDesktopRootToplevelId = 0;
        if (session_.taskbarId == toplevelId) {
            OH_LOG_INFO(LOG_APP, "[MW] taskbar toplevel #%{public}u destroyed, clearing cached id",
                        toplevelId);
            session_.taskbarId = 0;
        }
        // root 本体被销毁 (xs_destroy / 客户端断连路径同样走到这里): 复位, 等待下一个 explorer
        if (session_.desktopRootToplevelId == toplevelId) {
            OH_LOG_INFO(LOG_APP, "[MW] desktop root toplevel #%{public}u destroyed, clearing root",
                        toplevelId);
            session_.desktopRootToplevelId = 0;
            wasDesktopRoot = true;
            // 桌面会话由 explorer 主动结束: 随后 wineserver 跟随退出属正常终结,
            // ProcMon 据此按 state:stopped 收口而非误报 failed (仅 desktop 模式
            // 有 root, PC 窗口模式不会走到这)
            hgs::seam::GetHostLifecycleHooks().OnDesktopSessionEnded();
        }
        // 被抓取窗口销毁 → 复位 move grab, 防止悬空 grab 吞掉后续 motion
        if (moveGrab_.GetToplevelId() == toplevelId) {
            OH_LOG_INFO(LOG_APP, "[MW-MOVE] grabbed toplevel #%{public}u destroyed, reset grab",
                        toplevelId);
            moveGrab_.EndMoveGrab(toplevelMgr_);
        }
        toplevelMgr_.RemoveFromZOrder(toplevelId);
        // 级联清理该 toplevel 的全部 PC popup (帧数据 + 映射) — popup 表与
        // 清理收口于 PopupManager (重构第 5B2 步; 锁域不变: 收集/删除仍在
        // toplevelMgr 锁内, popup_hide fire 仍在锁外, 见下方)
        cascadePopups = popupMgr_.CollectPopupIdsForParentLocked(toplevelId);
        for (uint32_t pid : cascadePopups) popupMgr_.RemovePopupDataLocked(pid);
        MarkDesktopRootDirtyLocked();  // 非 desktop / root 已复位时 root=0, 自然 no-op
        // 对称清理 surface 映射 (popup 路径在 RemovePopupDataLocked 已清, toplevel 路径此前缺失):
        // xs_destroy 时 wl_surface 可能仍存活, 不清会让 GetSurfaceForToplevel(死 id) 命中
        // 已无 toplevel 身份的 surface。嵌套锁序同 RemovePopupDataLocked。
        toplevelMgr_.UnmapToplevelSurface(toplevelId);
    }
    // 桌面会话终结统一收口 (锁外): 重置进程级一次性状态, 使下次引擎启动
    // (热重启连旧 wineserver) 与冷启动同基线 — 状态生命周期按「Wine 会话」
    // 而非「进程」建模。stopClient 路径由 napi_init 显式调用同函数。
    // 注意: 不在 Wayland 线程销毁 renderer (DestroyToplevel → Shutdown →
    // join 渲染线程 — 渲染线程可能卡在 eglSwapBuffers 等不可中断点,
    // 同步 join 会永久阻塞 Wayland 事件循环, 热重启 explorer 连接无人
    // 处理, 桌面永远起不来)。root renderer 的销毁由 ArkTS DesktopLayer
    // onSurfaceDestroyed 负责 (缓存 rootId 配对销毁, 与创建对称)
    if (wasDesktopRoot) ResetSessionState();
    // 通知 ArkTS 销毁 popup 子窗口 (锁外触发)
    for (uint32_t pid : cascadePopups) {
        PostToplevelEvent(toplevelId, ToplevelEventType::PopupHide,
                          ToplevelEventBus::JsonPopupHide(pid));
    }
}

void WaylandServer::TryBeginSessionFirstFrame(uint32_t toplevelId, wl_resource* surfRes) {
    // 首帧通知 + 预设 pointer/keyboard focus (参考 HarmonyBox)
    // 决策归属 (重构第 5B1 步): 从 wl_core FinishCommit 内联段收为命名策略 —
    // 协议壳只陈述"首帧 commit 发生", 焦点预注入 (注入条件/顺序) 逐字平移。
    // 存储 = 会话共享 POD 的 session_.firstFrame (重构第 6B 步, 仍 atomic)
    bool expected = false;
    if (session_.firstFrame.compare_exchange_strong(expected, true)) {
        FireState("active");
        // 预设 focus: Wine 在用户操作前就需要 enter
        // 安全检查: 只有 resource 已创建才注入 (否则 Inject*Enter 内部会 DROP)
        if (Seat::GetInstance()->HasPointerResource()) {
            InputManager::GetInstance()->InjectPointerEnter(toplevelId, surfRes,
                                                            wl_fixed_from_int(0),
                                                            wl_fixed_from_int(0));
        }
        if (Seat::GetInstance()->HasKeyboardResource()) {
            InputManager::GetInstance()->InjectKeyboardEnter(toplevelId, surfRes);
        }
    }
}

void WaylandServer::ResetSessionState() {
    // Wine 会话终结统一收口。只重置「进程级一次性/漂移状态」— 随 toplevel
    // 销毁自愈的字段 (root/pending/taskbar, OnToplevelDestroyed 锁内清理)
    // 不在这里重复, 避免锁外写非 atomic 字段与锁内读的竞态。
    session_.firstFrame = false;   // 热重启不重走 Start, 不重置则新会话首帧不注入 focus
    moveGrab_.EndMoveGrab(toplevelMgr_);  // 幂等兜底 (grab 窗口非 root 时已随销毁复位)
    InputManager::GetInstance()->ResetSessionState();
    OH_LOG_INFO(LOG_APP, "[MW] session state reset (firstFrame/grab/input focus+keys)");
}

// RemovePopupDataLocked / RemovePopupBySurfaceKeyLocked 已移至 PopupManager
// (compositor/popup_manager.{h,cpp}, 重构第 5B2 步)

void WaylandServer::SendToplevelClose(uint32_t toplevelId) {
    wl_resource* tl = toplevelMgr_.FindToplevelResource(toplevelId);
    if (tl) {
        toplevelMgr_.UnregisterToplevelResource(toplevelId);
        OH_LOG_INFO(LOG_APP, "[MW] SendToplevelClose id=%{public}u -> xdg_toplevel_send_close", toplevelId);
        xdg_toplevel_send_close(tl);
    } else {
        OH_LOG_WARN(LOG_APP, "[MW] SendToplevelClose id=%{public}u NOT found", toplevelId);
    }
}

// IsToplevelVisibleLocked 已移至 ToplevelManager (compositor/toplevel_manager.cpp)

int32_t WaylandServer::GetWorkAreaHeight() {
    auto lk = toplevelMgr_.Lock();
    if (session_.taskbarId == 0) return session_.outputH;
    const auto* st = toplevelMgr_.FindToplevelLocked(session_.taskbarId);
    if (!st) return session_.outputH;
    return st->Y();  // 工作区 = 任务栏上方空间
}

void WaylandServer::SetToplevelMinimized(uint32_t id) {
    auto lk = toplevelMgr_.Lock();
    // 保留 operator[] 建档语义: pre-commit 最小化同样记录状态
    toplevelMgr_.EnsureToplevelLocked(id).SetMinimized(true);
    MarkDesktopRootDirtyLocked();
}

void WaylandServer::SetToplevelRestored(uint32_t id) {
    // 清除 minimized 状态
    {
        auto lk = toplevelMgr_.Lock();
        if (auto* st = toplevelMgr_.FindToplevelLocked(id)) st->SetMinimized(false);
        MarkDesktopRootDirtyLocked();
    }
    // 通知 ArkTS 把 OHOS 承载窗口显示回来 (子窗口 minimize 后系统无 Dock 还原
    // 入口, 只能本应用 showWindow — 见 @ohos.window 文档): 覆盖 set_maximized /
    // set_fullscreen / NAPI setToplevelVisible 三条恢复路径。Wine 自发恢复帧
    // 不走本函数, 由 wl_core 的 auto-restore 通道另发 (两处互补, 同一恢复动作
    // 只会命中其一 — 都以 IsToplevelMinimized 为前置)。此处锁已释放。
    if (Policy().OhosWindowPerToplevel()) {
        PostToplevelEvent(id, ToplevelEventType::Restored);
    }
    // 发 configure 通知 Wine (如果 toplevel resource 存在)
    wl_resource* tl = toplevelMgr_.FindToplevelResource(id);
    if (!tl) return;
    auto* td = static_cast<ToplevelData*>(wl_resource_get_user_data(tl));
    if (!td || !td->xdgSurface) return;
    auto* xdg = static_cast<XdgSurface*>(wl_resource_get_user_data(td->xdgSurface));
    if (!xdg || !xdg->wlSurface) return;
    std::vector<uint32_t> states = {XDG_TOPLEVEL_STATE_ACTIVATED};
    // 全屏窗口从最小化还原: 维持 FULLSCREEN 状态 (尺寸 0,0 = Wine 保持当前尺寸)
    // 状态查询直调 toplevelMgr_ (6A 删转发; 本函数为 WaylandServer 成员, 直调同值)
    if (toplevelMgr_.IsToplevelFullscreen(id)) states.push_back(XDG_TOPLEVEL_STATE_FULLSCREEN);
    XdgConfigureSend(tl, xdg->xdgSurface, 0, 0, states);
}

void WaylandServer::SetToplevelMaximized(uint32_t id) {
    OH_LOG_INFO(LOG_APP, "[MW] SetToplevelMaximized id=%{public}u desktop=%{public}s",
                id, IsDesktopMode() ? "yes" : "no");
    auto lk = toplevelMgr_.Lock();
    if (auto* st = toplevelMgr_.FindToplevelLocked(id); st && st->HasPosition()) {
        st->AnchorToOrigin();
    }
    MarkDesktopRootDirtyLocked();
}

// 重构第 5C 步: maximized 状态位写 (xdg_shell 协议处理调用), 权威自
// SurfaceData::maximized 迁入 ToplevelState (PLAN §2.4 状态权威分裂修复 —
// 旧 tl_set_fullscreen 须手工清 sd->maximized 以防 configure 误带 MAXIMIZED)。
// Ensure 建档与 SetToplevelMinimized 同款 (pre-commit 状态转换同样记录)。
// 裸状态赋值: 无 dirty/无日志 — 与旧 sd->maximized = x 直接赋值逐字等价,
// dirty 由调用点随后的 SetToplevelMaximized (锚定) / configure 路径负责。
void WaylandServer::SetToplevelMaximizedState(uint32_t id, bool on) {
    auto lk = toplevelMgr_.Lock();
    toplevelMgr_.EnsureToplevelLocked(id).SetMaximized(on);
}

void WaylandServer::SetToplevelFullscreen(uint32_t id, bool on) {
    OH_LOG_INFO(LOG_APP, "[MW] SetToplevelFullscreen id=%{public}u on=%{public}s",
                id, on ? "yes" : "no");
    auto lk = toplevelMgr_.Lock();
    // Ensure 建档语义同 SetToplevelMinimized: pre-commit 全屏同样记录状态。
    // 状态转换 (置位 + 锚定 + preFs 快照 + 不变式断言) 收在
    // ToplevelState::ApplyFullscreen
    auto& st = toplevelMgr_.EnsureToplevelLocked(id);
    st.ApplyFullscreen(on);
    MarkDesktopRootDirtyLocked();
}

void WaylandServer::ForceToplevelRedraw(uint32_t id) {
    auto lk = toplevelMgr_.Lock();
    if (auto* st = toplevelMgr_.FindToplevelLocked(id)) st->MarkDirty();
}

void WaylandServer::NotifyToplevelResize(uint32_t toplevelId, int32_t w, int32_t h, bool resizing) {
    // 最小化门禁: 窗口最小化期间不向 Wine 发任何 configure。
    // winewayland 的「最小化→还原」握手 (window.c restoring_from_minimize)
    // 依赖窗口 rect 停在 -32000 哨兵位; 此时收到 configure, wine 走普通
    // configure 路径处理会 SetWindowPos 把窗口挪回屏幕内, 哨兵位被污染 →
    // 用户还原时检测永不成立, WS_MINIMIZE 清不掉 (war3 还原后黑屏 +
    // 点击再最小化的根因之一)。还原通道 SetToplevelRestored 不走本函数。
    // 状态查询直调 toplevelMgr_ (6A 删转发; 本函数为 WaylandServer 成员, 同值)
    if (toplevelMgr_.IsToplevelMinimized(toplevelId)) {
        OH_LOG_INFO(LOG_APP, "[MW] NotifyToplevelResize id=%{public}u %{public}dx%{public}d SKIPPED (minimized)",
                    toplevelId, w, h);
        return;
    }
    wl_resource* tl = toplevelMgr_.FindToplevelResource(toplevelId);
    if (!tl) return;

    auto* td = static_cast<ToplevelData*>(wl_resource_get_user_data(tl));
    if (!td || !td->xdgSurface) return;
    auto* xdg = static_cast<XdgSurface*>(wl_resource_get_user_data(td->xdgSurface));
    if (!xdg) return;

    // maximized 状态位权威在 ToplevelState (重构第 5C 步: 旧读 sd->maximized
    // 迁移为读 ToplevelState; 未建档/sd 缺失同值 false)。一次读取供本函数
    // 三处消费 (日志/状态位/日志), 独立加锁与 IsToplevelFullscreen 同模式
    // (本函数入口的 IsToplevelMinimized 已是该模式, 无锁嵌套)。
    // 6A: 状态查询直调 toplevelMgr_ (删转发; 本函数为 WaylandServer 成员, 同值)。
    const bool maximized = toplevelMgr_.IsToplevelMaximized(toplevelId);

    OH_LOG_INFO(LOG_APP, "[MW] NotifyToplevelResize IN id=%{public}u %{public}dx%{public}d pc=%{public}s max=%{public}s resize=%{public}s",
                toplevelId, w, h,
                IsDesktopMode() ? "no" : "yes",
                maximized ? "yes" : "no",
                resizing ? "yes" : "no");

    // 拖拽缩放中: 渲染器整帧拉伸填满 — 窗口已变而 Wine 新帧未到的空档里,
    // 等比 fit 会按旧帧比例留黑边 (拖拽结束的 0 尺寸 configure 复位)
    PluginManager::GetInstance()->SetRendererStretchFill(toplevelId, resizing);

    std::vector<uint32_t> states = {XDG_TOPLEVEL_STATE_ACTIVATED};
    if (maximized) states.push_back(XDG_TOPLEVEL_STATE_MAXIMIZED);
    // 全屏窗口在 OHOS 侧尺寸变化时保持 FULLSCREEN 状态, 否则 Wine 会退出全屏。
    if (toplevelMgr_.IsToplevelFullscreen(toplevelId)) states.push_back(XDG_TOPLEVEL_STATE_FULLSCREEN);
    // 拖拽缩放中: Wine 仅采用带 RESIZING 的 configure 尺寸 (见头文件注释)
    if (resizing) states.push_back(XDG_TOPLEVEL_STATE_RESIZING);
    XdgConfigureSend(tl, xdg->xdgSurface, w, h, states);

    // 桌面 root 尺寸变化: 不反向写 output。output 的权威源是 ArkTS 启动时
    // setOutputSize (display 物理尺寸 / effectiveScale), root 的 resize 只反映
    // ArkUI surface 波动 — 桌面退出时 launcherVisible 翻 true, 窗口退出全屏,
    // XComponent 高度 1840→1683, 反写会把错误高度残留进 output, 下次热重启
    // explorer 用 /desktop=shell,WxH 建桌面就少了这段高度 (上下被裁)。
    if (Policy().RootCompositing() && toplevelId == session_.desktopRootToplevelId) {
        OH_LOG_INFO(LOG_APP, "[MW] NotifyToplevelResize root=%{public}u (output untouched) %{public}dx%{public}d",
                    toplevelId, w, h);
    } else {
        OH_LOG_INFO(LOG_APP, "[MW] NotifyToplevelResize id=%{public}u → %{public}dx%{public}d maximized=%{public}s",
                    toplevelId, w, h, maximized ? "yes" : "no");
    }
}
// GetSurfaceForToplevel 转发已删 (重构第 6A 步): 调用方 (InputManager/
// InputInjector) 经装配注入 ToplevelManager 引用直调 GetSurfaceForToplevel

// ========================================================================
//  NAPI Export
//  阶段 1 收口 (hgs-exported-api.md §4): 业务入口经 HgsInstance 门面;
//  回调通道经 NapiHostAdapter (IHgsHost) 注册 — 本层不再直挂
//  WaylandServer::SetToplevelCallback/SetStateCallback (bus 单 sink 由
//  HgsInstance::Start 唯一安装, 事件统一 bus → IHgsHost → TSFN → JS)。
//  旧名保留并转发新实现, 首次调用打 [HGS-DEPRECATED] (每名一次)。
// ========================================================================

namespace {

// -- napi 对象属性读取助手 (startServer 的 HgsConfig 解析用) --
bool NapiIsObject(napi_env env, napi_value v) {
    napi_valuetype t;
    return napi_typeof(env, v, &t) == napi_ok && t == napi_object;
}

bool NapiGetStr(napi_env env, napi_value obj, const char* key, std::string* out) {
    napi_value v;
    if (napi_get_named_property(env, obj, key, &v) != napi_ok) return false;
    napi_valuetype t;
    if (napi_typeof(env, v, &t) != napi_ok || t != napi_string) return false;
    char buf[512];
    size_t len = 0;
    if (napi_get_value_string_utf8(env, v, buf, sizeof(buf), &len) != napi_ok) return false;
    out->assign(buf, len);
    return true;
}

bool NapiGetI32(napi_env env, napi_value obj, const char* key, int32_t* out) {
    napi_value v;
    if (napi_get_named_property(env, obj, key, &v) != napi_ok) return false;
    napi_valuetype t;
    if (napi_typeof(env, v, &t) != napi_ok || t != napi_number) return false;
    double d = 0;
    if (napi_get_value_double(env, v, &d) != napi_ok) return false;
    *out = static_cast<int32_t>(d);
    return true;
}

bool NapiGetU32(napi_env env, napi_value obj, const char* key, uint32_t* out) {
    int32_t i = 0;
    if (!NapiGetI32(env, obj, key, &i) || i < 0) return false;
    *out = static_cast<uint32_t>(i);
    return true;
}

bool NapiGetF64(napi_env env, napi_value obj, const char* key, double* out) {
    napi_value v;
    if (napi_get_named_property(env, obj, key, &v) != napi_ok) return false;
    napi_valuetype t;
    if (napi_typeof(env, v, &t) != napi_ok || t != napi_number) return false;
    return napi_get_value_double(env, v, out) == napi_ok;
}

bool NapiGetBool(napi_env env, napi_value obj, const char* key, bool* out) {
    napi_value v;
    if (napi_get_named_property(env, obj, key, &v) != napi_ok) return false;
    napi_valuetype t;
    if (napi_typeof(env, v, &t) != napi_ok || t != napi_boolean) return false;
    return napi_get_value_bool(env, v, out) == napi_ok;
}

} // namespace

namespace hgs {
namespace tl {

// -- NAPI: setToplevelCallback -- (字符串契约 cb(id, event, json), 保留;
//    typed 版见 setToplevelEventCallback — 后注册者覆盖前者, 单 sink)
napi_value SetToplevelCallbackNAPI(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) return nullptr;
    NapiHostAdapter::Get().SetToplevelString(env, args[0]);
    HgsInstance::Instance().SetHost(&NapiHostAdapter::Get());
    return nullptr;
}

// -- NAPI: setToplevelEventCallback -- (typed 契约 cb(id, type, json);
//    type = HgsToplevelEventType 数值, hgs-exported-api.md §4.2)
napi_value SetToplevelEventCallbackNAPI(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) return nullptr;
    NapiHostAdapter::Get().SetToplevelTyped(env, args[0]);
    HgsInstance::Instance().SetHost(&NapiHostAdapter::Get());
    return nullptr;
}

// -- NAPI: getCurrentToplevelId -- @deprecated → takePendingToplevel
//   (WineWindow.aboutToAppear 同步读取, 无竞态)
napi_value GetCurrentToplevelIdNAPI(napi_env env, napi_callback_info info) {
    static bool s_dep = false;
    if (!s_dep) {
        s_dep = true;
        OH_LOG_WARN(LOG_APP, "[HGS-DEPRECATED] getCurrentToplevelId → takePendingToplevel (阶段 3 删除)");
    }
    uint32_t id = HgsInstance::Instance().TakePendingToplevel();
    OH_LOG_INFO(LOG_APP, "[MW-NAPI] getCurrentToplevelId = %{public}u", id);
    napi_value r;
    napi_create_uint32(env, id, &r);
    return r;
}

// -- NAPI: takePendingToplevel -- (canonical; 出队语义, 空 → 0)
napi_value TakePendingToplevelNAPI(napi_env env, napi_callback_info) {
    uint32_t id = HgsInstance::Instance().TakePendingToplevel();
    napi_value r;
    napi_create_uint32(env, id, &r);
    return r;
}

// -- NAPI: setPendingToplevel -- (WineWindowAbility 在 loadContent 前调用)
napi_value SetPendingToplevelNAPI(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    uint32_t id = 0;
    napi_get_value_uint32(env, args[0], &id);
    HgsInstance::Instance().SetPendingToplevel(id);
    OH_LOG_INFO(LOG_APP, "[MW-NAPI] setPendingToplevel id=%{public}u", id);
    return nullptr;
}

// -- NAPI: cancelPendingToplevel -- (窗口在 loadContent 前被销毁时清除队列残坑,
//   防止后续页面出队拿到死 id → 渲染器挂错 toplevel 黑屏; 未在队列时 no-op)
napi_value CancelPendingToplevelNAPI(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    uint32_t id = 0;
    napi_get_value_uint32(env, args[0], &id);
    HgsInstance::Instance().CancelPendingToplevel(id);
    return nullptr;
}

// -- NAPI: destroyToplevel -- (ArkTS 关闭子窗口后调用)
napi_value DestroyToplevelNAPI(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    uint32_t id = 0;
    napi_get_value_uint32(env, args[0], &id);
    // 门面语义 = renderer 收尾 + 相对模式锁定兜底释放 (PointerExtras)
    HgsInstance::Instance().DestroyToplevel(id);
    OH_LOG_INFO(LOG_APP, "[MW-NAPI] destroyToplevel id=%{public}u", id);
    return nullptr;
}

// -- NAPI: sendToplevelClose -- @deprecated → closeToplevel
napi_value SendToplevelCloseNAPI(napi_env env, napi_callback_info info) {
    static bool s_dep = false;
    if (!s_dep) {
        s_dep = true;
        OH_LOG_WARN(LOG_APP, "[HGS-DEPRECATED] sendToplevelClose → closeToplevel (阶段 3 删除)");
    }
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    uint32_t id = 0;
    napi_get_value_uint32(env, args[0], &id);
    OH_LOG_INFO(LOG_APP, "[MW-NAPI] sendToplevelClose id=%{public}u", id);
    HgsInstance::Instance().SendToplevelClose(id);
    return nullptr;
}

// -- NAPI: closeToplevel -- (canonical; xdg_toplevel.send_close)
napi_value CloseToplevelNAPI(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    uint32_t id = 0;
    napi_get_value_uint32(env, args[0], &id);
    HgsInstance::Instance().SendToplevelClose(id);
    return nullptr;
}

napi_value NotifyToplevelResizeNAPI(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value args[4];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 3) return nullptr;
    uint32_t tl; int32_t w, h;
    bool resizing = false;
    napi_get_value_uint32(env, args[0], &tl);
    napi_get_value_int32(env, args[1], &w);
    napi_get_value_int32(env, args[2], &h);
    if (argc >= 4) napi_get_value_bool(env, args[3], &resizing);
    OH_LOG_INFO(LOG_APP, "[NAPI] notifyToplevelResize tl=%{public}u %{public}dx%{public}d resize=%{public}s",
                tl, w, h, resizing ? "yes" : "no");
    HgsInstance::Instance().NotifyToplevelResize(tl, w, h, resizing);
    return nullptr;
}

// 拖拽缩放结束 (ArkTS windowRectChange DRAG_END): 发 configure(0,0) 清 RESIZING
// 状态, Wine 保持当前尺寸 (0 尺寸 → SWP_NOSIZE) 并退出 size-move。
// -- NAPI: notifyToplevelResizeEnd -- @deprecated → notifyToplevelResize(id,0,0,false)
napi_value NotifyToplevelResizeEndNAPI(napi_env env, napi_callback_info info) {
    static bool s_dep = false;
    if (!s_dep) {
        s_dep = true;
        OH_LOG_WARN(LOG_APP, "[HGS-DEPRECATED] notifyToplevelResizeEnd → notifyToplevelResize(id,0,0,false) (阶段 3 删除)");
    }
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) return nullptr;
    uint32_t tl;
    napi_get_value_uint32(env, args[0], &tl);
    OH_LOG_INFO(LOG_APP, "[NAPI] notifyToplevelResizeEnd tl=%{public}u", tl);
    HgsInstance::Instance().NotifyToplevelResize(tl, 0, 0, false);
    return nullptr;
}

// Desktop 模式: 接收物理像素坐标 (px, py), 通过 viewport 映射为 Wine 逻辑坐标后查找
// resize 后 surface 和逻辑尺寸比例变化, 由 renderer viewport 保证映射正确
napi_value FindToplevelAtNAPI(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 2) return nullptr;
    int32_t px, py;  // 物理像素坐标
    napi_get_value_int32(env, args[0], &px);
    napi_get_value_int32(env, args[1], &py);

    auto* ws = WaylandServer::GetInstance();
    uint32_t rootId = ws->GetDesktopRootToplevelId();
    wl_fixed_t wx, wy;
    InputManager::GetInstance()->CoordTransform(px, py, rootId > 0 ? rootId : 1, &wx, &wy);
    int32_t lx = wl_fixed_to_int(wx);
    int32_t ly = wl_fixed_to_int(wy);

    uint32_t id = HgsInstance::Instance().FindToplevelAt(lx, ly);
    napi_value result;
    napi_create_uint32(env, id, &result);
    return result;
}

// Desktop 模式: 将 toplevel 提到 Z-order 最顶层
napi_value RaiseToplevelNAPI(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) return nullptr;
    uint32_t tl;
    napi_get_value_uint32(env, args[0], &tl);
    // 用户显式操作 (任务栏/窗口点击) 路径: 已 fullscreen 的目标会重新取
    // 全屏优先级号, 支撑两个全屏窗口间的主动切换 (门面 RaiseToplevel 固定
    // userInitiated=true)
    HgsInstance::Instance().RaiseToplevel(tl);
    return nullptr;
}

napi_value SetToplevelVisibleNAPI(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 2) return nullptr;
    uint32_t tl; bool visible;
    napi_get_value_uint32(env, args[0], &tl);
    napi_get_value_bool(env, args[1], &visible);
    // 门面组合语义: 输入可见性 + visible=true 附带 SetToplevelRestored
    HgsInstance::Instance().SetToplevelVisible(tl, visible);
    return nullptr;
}

} // namespace tl

namespace ws {

// -- NAPI: startServer -- (canonical: string | HgsConfig 对象)。
// 字符串形式 = 旧调用零改动 (其余配置缺省 = 不修改宿主预设);
// 对象形式字段: socketPath(必填)/output{width,height,scale}/composition/
// inputBackend/features{...}/setProcessEnvironment
// (枚举数值契约见 hgs-exported-api.md §4.2)。
napi_value StartServerNAPI(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) {
        OH_LOG_ERROR(LOG_APP, "[NAPI] startServer: 需要 socketPath 或 HgsConfig");
        napi_value r;
        napi_get_boolean(env, false, &r);
        return r;
    }

    HgsConfig cfg;
    napi_valuetype t;
    napi_typeof(env, args[0], &t);
    if (t == napi_string) {
        char path[512] = {};
        napi_get_value_string_utf8(env, args[0], path, sizeof(path), nullptr);
        cfg.socketPath = path;
    } else if (NapiIsObject(env, args[0])) {
        std::string path;
        if (NapiGetStr(env, args[0], "socketPath", &path)) cfg.socketPath = path;
        napi_value output;
        if (napi_get_named_property(env, args[0], "output", &output) == napi_ok &&
            NapiIsObject(env, output)) {
            NapiGetI32(env, output, "width", &cfg.output.width);
            NapiGetI32(env, output, "height", &cfg.output.height);
            NapiGetF64(env, output, "scale", &cfg.output.scale);
            NapiGetU32(env, output, "refreshMilliHz",
                       reinterpret_cast<uint32_t*>(&cfg.output.refreshMilliHz));
        }
        uint32_t comp = 0;
        if (NapiGetU32(env, args[0], "composition", &comp) &&
            comp <= static_cast<uint32_t>(CompositionMode::DesktopRoot)) {
            cfg.composition = static_cast<CompositionMode>(comp);
        }
        uint32_t backend = 0;
        if (NapiGetU32(env, args[0], "inputBackend", &backend) &&
            backend <= static_cast<uint32_t>(InputBackend::Native)) {
            cfg.inputBackend = static_cast<InputBackend>(backend);
        }
        napi_value feats;
        if (napi_get_named_property(env, args[0], "features", &feats) == napi_ok &&
            NapiIsObject(env, feats)) {
            NapiGetBool(env, feats, "shm", &cfg.features.shm);
            NapiGetBool(env, feats, "subsurface", &cfg.features.subsurface);
            NapiGetBool(env, feats, "viewporter", &cfg.features.viewporter);
            NapiGetBool(env, feats, "xdgPopup", &cfg.features.xdgPopup);
            NapiGetBool(env, feats, "dataDevice", &cfg.features.dataDevice);
            NapiGetBool(env, feats, "relativePointer", &cfg.features.relativePointer);
            NapiGetBool(env, feats, "textInput", &cfg.features.textInput);
            NapiGetBool(env, feats, "externalSurface", &cfg.features.externalSurface);
            NapiGetBool(env, feats, "winehuaToplevel", &cfg.features.winehuaToplevel);
        }
        NapiGetBool(env, args[0], "setProcessEnvironment", &cfg.setProcessEnvironment);
    } else {
        OH_LOG_ERROR(LOG_APP, "[NAPI] startServer: 参数类型需为 string 或 HgsConfig 对象");
        napi_value r;
        napi_get_boolean(env, false, &r);
        return r;
    }

    OH_LOG_WARN(LOG_APP, "[NAPI] startServer: %{public}s", cfg.socketPath.c_str());
    // 确保 socket 父目录存在 (WINEPREFIX=.wine/; 0755 供 wine 子进程访问 —
    // WaylandServer::Start 内部另有 0700 mkdir, 此处保持旧 NAPI 行为)
    {
        std::string sockDir = cfg.socketPath;
        auto pos = sockDir.find_last_of('/');
        if (pos != std::string::npos) {
            sockDir = sockDir.substr(0, pos);
            mkdir(sockDir.c_str(), 0755);
        }
    }
    bool ok = HgsInstance::Instance().Start(cfg);
    OH_LOG_WARN(LOG_APP, "[NAPI] startServer result: %{public}s", ok ? "OK" : "FAIL");
    // 确认 socket 文件存在
    if (ok) {
        struct stat st;
        int sr = stat(cfg.socketPath.c_str(), &st);
        OH_LOG_WARN(LOG_APP, "[NAPI] wayland socket stat=%{public}d (errno=%{public}d)",
                    sr, sr == 0 ? 0 : errno);
    }

    napi_value r;
    napi_get_boolean(env, ok, &r);
    return r;
}

// -- NAPI: stopServer — 停止内嵌 Wayland compositor --
napi_value StopServerNAPI(napi_env, napi_callback_info) {
    HgsInstance::Instance().Stop();
    return nullptr;
}

// -- NAPI: isRunning -- (canonical)
napi_value IsRunningNAPI(napi_env env, napi_callback_info) {
    napi_value r;
    napi_get_boolean(env, HgsInstance::Instance().IsRunning(), &r);
    return r;
}

// -- NAPI: getWaylandDisplayName -- (canonical; 未启动/无效路径返回空串)
napi_value GetWaylandDisplayNameNAPI(napi_env env, napi_callback_info) {
    const std::string name = HgsInstance::Instance().WaylandDisplayName();
    napi_value r;
    napi_create_string_utf8(env, name.c_str(), NAPI_AUTO_LENGTH, &r);
    return r;
}

// -- NAPI: getClientEnvironment -- (canonical; string[]: XDG_RUNTIME_DIR/WAYLAND_DISPLAY)
napi_value GetClientEnvironmentNAPI(napi_env env, napi_callback_info) {
    const std::vector<std::string> envs = HgsInstance::Instance().ClientEnvironment();
    napi_value arr;
    napi_create_array_with_length(env, envs.size(), &arr);
    for (size_t i = 0; i < envs.size(); i++) {
        napi_value s;
        napi_create_string_utf8(env, envs[i].c_str(), NAPI_AUTO_LENGTH, &s);
        napi_set_element(env, arr, static_cast<uint32_t>(i), s);
    }
    return arr;
}

// -- NAPI: getApiVersion -- (canonical; major<<16 | minor, 阶段 1 = 0x10001)
napi_value GetApiVersionNAPI(napi_env env, napi_callback_info) {
    napi_value r;
    napi_create_uint32(env, HgsInstance::ApiVersion(), &r);
    return r;
}

// -- NAPI: getLastError -- (canonical; 最近一次失败原因, 空串 = 无错误)
napi_value GetLastErrorNAPI(napi_env env, napi_callback_info) {
    const std::string err = HgsInstance::Instance().LastError();
    napi_value r;
    napi_create_string_utf8(env, err.c_str(), NAPI_AUTO_LENGTH, &r);
    return r;
}

// -- NAPI: setInputBackend -- (canonical; 0=Auto 1=ArkTS 2=Native, 阶段 1 记录)
napi_value SetInputBackendNAPI(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    uint32_t mode = 0;
    if (argc >= 1) napi_get_value_uint32(env, args[0], &mode);
    HgsInstance::Instance().SetInputBackend(
        mode <= static_cast<uint32_t>(InputBackend::Native)
            ? static_cast<InputBackend>(mode) : InputBackend::Auto);
    return nullptr;
}

// -- NAPI: getInputBackend -- (canonical)
napi_value GetInputBackendNAPI(napi_env env, napi_callback_info) {
    napi_value r;
    napi_create_uint32(
        env, static_cast<uint32_t>(HgsInstance::Instance().GetInputBackend()), &r);
    return r;
}

// -- NAPI: setOutputSize --
napi_value SetOutputSizeNAPI(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 2) return nullptr;
    int32_t w, h;
    napi_get_value_int32(env, args[0], &w);
    napi_get_value_int32(env, args[1], &h);
    HgsInstance::Instance().SetOutputSize(w, h);
    return nullptr;
}

// -- NAPI: setDisplayScale -- @deprecated (现 no-op; 阶段 2 由
//    HgsConfig.output.scale 取代)
// 已无效: C++ 坐标换算不使用 display scale (letterbox 由 renderer viewport 推导,
// globalDisplayScale_ 只写不读已删除)。保留导出仅为兼容 ArkTS 侧调用, 收到直接忽略。
napi_value SetDisplayScaleNAPI(napi_env env, napi_callback_info info) {
    static bool s_dep = false;
    if (!s_dep) {
        s_dep = true;
        OH_LOG_WARN(LOG_APP, "[HGS-DEPRECATED] setDisplayScale 是 no-op (阶段 2 由 HgsConfig.output.scale 取代)");
    }
    (void)env;
    (void)info;
    return nullptr;
}

napi_value SetDesktopModeNAPI(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc >= 1) {
        bool on;
        napi_get_value_bool(env, args[0], &on);
        HgsInstance::Instance().SetDesktopMode(on);
        OH_LOG_INFO(LOG_APP, "[MW-NAPI] setDesktopMode = %{public}s", on ? "true" : "false");
    }
    return nullptr;
}

napi_value GetDesktopRootIdNAPI(napi_env env, napi_callback_info) {
    uint32_t id = HgsInstance::Instance().DesktopRootId();
    napi_value r;
    napi_create_uint32(env, id, &r);
    return r;
}

// -- State 回调 -> ArkTS (合成器 FireState: evt:desktop-ready 等) --
// 阶段 1: TSFN 机制迁 NapiHostAdapter, 本入口只做注册 (经 IHgsHost)。
napi_value SetStateCallbackNAPI(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) return nullptr;
    NapiHostAdapter::Get().SetState(env, args[0]);
    HgsInstance::Instance().SetHost(&NapiHostAdapter::Get());
    return nullptr;
}

} // namespace ws
} // namespace hgs

