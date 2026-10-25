// ============================================================================
// seam/registry — 接缝装配注册表 (方案 B 边界契约, 对应 contracts C11)
//
// 两个方向的跨 so 注入, 机制均为链接期弱符号绑定 (同进程 NAPI so 互解析,
// 工程先例: smoke_napi — 无 dlopen/dlsym):
//
//   宿主 (entry.so) 定义并填充 → libhgs.so 弱引用消费:
//     Hgs_zeroCopyBackend      GraphicsBroker 适配器 (ZC 查询/挂载/ready)
//     Hgs_hostHooks            wine_process 适配器 (桌面 shell 进程簿记)
//     Hgs_sessionControlSlot   SessionControl 槽位 (void*)
//
//   ⚠ 方向性: entry.so 先于 libhgs.so 加载, 其对后加载 so 的数据符号
//   重定位不会回补 — 因此 "entry 消费 hgs 实现" 必须走槽位模式: libhgs
//   加载时 (构造器) 经其对 entry 符号的绑定 (后加载方可解析先行模块)
//   把实现指针写入槽位; entry 在调用期读槽。
//
// 任一方向未注入时消费方取 null 语义:
//   - ZC 关闭 → 合成器走 wl_shm CPU 路径 (完整可用形态)
//   - 簿记 no-op → 仅影响 ArkTS 防误操作标记与退出分类
//   - 会话控制缺省 → IsDesktopMode=false / 输出尺寸回退 1280x720 /
//     root 等待超时走 ready-degraded
// ============================================================================
#pragma once

#include "host_hooks.h"
#include "zero_copy.h"

namespace hgs {
namespace seam {
class SessionControl;  // 定义见下
} // namespace seam
} // namespace hgs

// ── 弱符号装配点 (C 链接, 名字不参与重整) ──────────────────────────────────
extern "C" {

// 宿主 (entry.so) 定义: ZC 后端与进程簿记适配器 (libhgs.so 加载期绑定)
__attribute__((weak)) extern hgs::seam::ZeroCopyBackend*    Hgs_zeroCopyBackend;
__attribute__((weak)) extern hgs::seam::HostLifecycleHooks* Hgs_hostHooks;

// 宿主 (entry.so) 定义的槽位; libhgs.so 加载时经构造器写入实现指针。
// 槽位属 entry 自有数据, entry 调用期直接读 — 无跨 so 重定位问题。
__attribute__((weak)) extern void* Hgs_sessionControlSlot;

} // extern "C"

namespace hgs {
namespace seam {

// 合成器会话控制 — 宿主编排 (wine_launch / stopClient / stopAll / wine_env)
// 调用, 实现由 libhgs.so 提供 (包装 WaylandServer 单例)。
class SessionControl
{
public:
    virtual ~SessionControl() = default;
    virtual bool IsDesktopMode() const = 0;
    virtual int OutputWidth() const = 0;
    virtual int OutputHeight() const = 0;
    virtual void PromotePendingDesktopRoot() = 0;
    virtual uint32_t GetDesktopRootToplevelId() const = 0;
    // 首启 wineboot 期间抑制 root 识别/窗口事件 (PC 模式初始化等待窗)
    virtual void SetDesktopRootRecognitionEnabled(bool on) = 0;
    virtual void SetToplevelEventSuppressed(bool on) = 0;
    virtual void Stop() = 0;
    virtual void ResetSessionState() = 0;
    virtual void ResetFirstFrame() = 0;
    // 手柄桥 (controller 域, 随合成器迁入 hgs): 启动期 env 注入与桥初始化
    virtual bool EnsureBridgeForWineLaunch(const std::string &runtimeDir) = 0;
    virtual void AppendWineGamepadEnv(std::vector<std::string> &env) = 0;
};

// 空实现: 宿主未装配 (libhgs 未加载/未写入) 时的降级语义
class NullSessionControl : public SessionControl
{
public:
    bool IsDesktopMode() const override { return false; }
    int OutputWidth() const override { return 0; }
    int OutputHeight() const override { return 0; }
    void PromotePendingDesktopRoot() override {}
    uint32_t GetDesktopRootToplevelId() const override { return 0; }
    void SetDesktopRootRecognitionEnabled(bool) override {}
    void SetToplevelEventSuppressed(bool) override {}
    void Stop() override {}
    void ResetSessionState() override {}
    void ResetFirstFrame() override {}
    bool EnsureBridgeForWineLaunch(const std::string &) override { return false; }
    void AppendWineGamepadEnv(std::vector<std::string> &) override {}
};

// 会话控制访问 — 读槽位 (entry 自有数据); libhgs 未装配时返回空实现
inline SessionControl &GetSessionControl()
{
    if (Hgs_sessionControlSlot != nullptr) {
        return *static_cast<SessionControl *>(Hgs_sessionControlSlot);
    }
    static NullSessionControl nullSession;
    return nullSession;
}

} // namespace seam
} // namespace hgs
