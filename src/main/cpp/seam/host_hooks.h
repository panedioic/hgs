// ============================================================================
// seam/host_hooks — 合成器 → 宿主进程域的生命周期通知接缝
//
// 合成器在窗口生命周期事件里通知宿主做进程簿记 (对应 proc/wine_process.cpp):
//   - OnDesktopRootAppeared: 桌面 root 首次出现, 宿主把当前 running 的进程
//     标记为桌面 shell 基础进程 (ArkTS 据此隐藏"结束"防误操作)
//   - OnDesktopSessionEnded: 桌面 root 销毁, 宿主标记会话由 explorer 主动
//     结束 (ProcMon 按 state:stopped 收口而非误报 failed)
// 宿主 (entry) 装配期注入; 未注入时为 no-op (仅影响簿记, 不影响渲染)。
// ============================================================================
#pragma once

namespace hgs {
namespace seam {

class HostLifecycleHooks
{
public:
    virtual ~HostLifecycleHooks() = default;
    virtual void OnDesktopRootAppeared() = 0;
    virtual void OnDesktopSessionEnded() = 0;
};

class NullHostLifecycleHooks : public HostLifecycleHooks
{
public:
    void OnDesktopRootAppeared() override {}
    void OnDesktopSessionEnded() override {}
};

// 进程级访问: 弱符号 Hgs_hostHooks (registry.h) 由宿主注入,
// 未注入时返回 NullHostLifecycleHooks (簿记 no-op)。
HostLifecycleHooks &GetHostLifecycleHooks();

} // namespace seam
} // namespace hgs
