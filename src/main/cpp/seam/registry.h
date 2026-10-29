// hgs/src/main/cpp/seam/registry.h — 接缝装配注册表
//
// 不实现任何逻辑，只做三件事：
//
//     (a) 声明三个跨 so 符号的契约（现在全部是运行时解析，没有 weak 属性）：
//         libentry.so 提供：  Hgs_hostHooks          （HostLifecycleHooks* 变量）
//                             Hgs_zeroCopyBackend    （ZeroCopyBackend* 变量）
//         libhgs.so  提供：   Hgs_getSessionControl()（返回 SessionControl* 的 C 函数）
//     (b) 定义 SessionControl 纯虚接口——12 个方法，全是宿主侧编排需要问合成器的问题：
//     (c) 声明 NullSessionControl——全部返回安全默认值（IsDesktopMode=false、尺寸 0、
//         动作 no-op）。它是未注入时的兜底：libhgs 还没加载，宿主照样能跑（只是走 managed 模式）。
//     (d) 记录两次崩的经验：
//          为什么不能链接期绑定 (实测教训): OHOS 的 NAPI so 在各自加载命名空间
//          内, 彼此的加载期重定位看不到对方 — weak 数据符号在 GOT 里解析为 0,
//          解引用即 SIGSEGV@0; 函数弱符号则常驻 NULL (IsDesktopMode 恒 false)。
//          dlsym 在调用期解析 (对方模块此时已加载), 无方向问题; 查符号 ≠ dlopen
//          加载, 不破坏 "entry 不加载 hgs / hgs 不加载 entry" 的隔离原则。
#pragma once

#include "host_hooks.h"
#include "zero_copy.h"

namespace hgs {
namespace seam {

class SessionControl;  // 定义见下

} // namespace seam
} // namespace hgs

namespace hgs {
namespace seam {

// 合成器会话控制 — 宿主编排 (wine_launch / stopClient / stopAll / wine_env)
// 调用, 实现由 libhgs.so 提供 (包装 WaylandServer 单例)。
class SessionControl
{
public:
    virtual ~SessionControl() = default;
    // 决定自动拉起 explorer、是否传 WINEHUA_DESKTOP_MODE
    virtual bool IsDesktopMode() const = 0;
    // 决定 /desktop=shell,WxH 的虚拟桌面尺寸
    virtual int OutputWidth() const = 0;
    virtual int OutputHeight() const = 0;
    // 等待并确认桌面 root 就绪（15s 超时 → ready-degraded
    virtual void PromotePendingDesktopRoot() = 0;
    virtual uint32_t GetDesktopRootToplevelId() const = 0;
    // 首启 wineboot 期间抑制 root 识别/窗口事件 (PC 模式初始化等待窗)
    virtual void SetDesktopRootRecognitionEnabled(bool on) = 0;
    virtual void SetToplevelEventSuppressed(bool on) = 0;
    // 会话终结收口
    virtual void Stop() = 0;
    virtual void ResetSessionState() = 0;
    virtual void ResetFirstFrame() = 0;
    // 手柄桥 (controller 域, 随合成器迁入 hgs): 启动期 env 注入与桥初始化
    virtual bool EnsureBridgeForWineLaunch(const std::string &runtimeDir) = 0;
    virtual void AppendWineGamepadEnv(std::vector<std::string> &env) = 0;
};

// 空实现: 未装配时的降级语义 (entry 侧 dlsym 落空 / hgs 侧宿主未注入)
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

// 宿主侧访问点 (entry 的 host_seams.cpp 提供, 供 wine_launch 等
// entry 内调用点使用; 转发到 dlsym 查询的 hgs 实现)
SessionControl &HostSessionControl();

} // namespace seam
} // namespace hgs

// ── hgs → 宿主的访问器 (libhgs 提供强符号, 宿主调用期 dlsym 查询) ─────────
extern "C" {

// （实现见：session_control.cpp）
hgs::seam::SessionControl* Hgs_getSessionControl(void);

} // extern "C"
