#include "registry.h"

#include "compositor/wayland_server.h"
#include "input/controller/controller_runtime.h"
#include <hilog/log.h>

namespace hgs {
namespace seam {

namespace {

// SessionControl 实现 — 包装 WaylandServer 单例 (方法内懒取, 规避
// 静态初始化顺序问题)
class WaylandSessionControl : public SessionControl
{
public:
    bool IsDesktopMode() const override
    {
        return WaylandServer::GetInstance()->IsDesktopMode();
    }
    int OutputWidth() const override
    {
        return WaylandServer::GetInstance()->OutputWidth();
    }
    int OutputHeight() const override
    {
        return WaylandServer::GetInstance()->OutputHeight();
    }
    void PromotePendingDesktopRoot() override
    {
        WaylandServer::GetInstance()->PromotePendingDesktopRoot();
    }
    uint32_t GetDesktopRootToplevelId() const override
    {
        return WaylandServer::GetInstance()->GetDesktopRootToplevelId();
    }
    void SetDesktopRootRecognitionEnabled(bool on) override
    {
        WaylandServer::GetInstance()->SetDesktopRootRecognitionEnabled(on);
    }
    void SetToplevelEventSuppressed(bool on) override
    {
        WaylandServer::GetInstance()->SetToplevelEventSuppressed(on);
    }
    void Stop() override
    {
        WaylandServer::GetInstance()->Stop();
    }
    void ResetSessionState() override
    {
        WaylandServer::GetInstance()->ResetSessionState();
    }
    void ResetFirstFrame() override
    {
        WaylandServer::GetInstance()->ResetFirstFrame();
    }
    bool EnsureBridgeForWineLaunch(const std::string &runtimeDir) override
    {
        return winehua::controller::EnsureBridgeForWineLaunch(runtimeDir);
    }
    void AppendWineGamepadEnv(std::vector<std::string> &env) override
    {
        winehua::controller::AppendWineGamepadEnv(env);
    }
};

WaylandSessionControl g_sessionControl;

// 加载期装配: 把实现指针写入宿主 (entry.so) 的槽位。libhgs.so 晚于
// entry.so 加载, 其对 entry 符号的绑定此时已可解析。
struct SessionControlInstaller
{
    SessionControlInstaller()
    {
        Hgs_sessionControlSlot = static_cast<void *>(&g_sessionControl);
    }
};
const SessionControlInstaller g_sessionControlInstaller;

} // namespace

} // namespace seam
} // namespace hgs
