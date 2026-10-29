// hgs/src/main/cpp/seam/session_control.h — Wayland Session Contril 实现
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

} // namespace

} // namespace seam
} // namespace hgs

// C 访问器 (强符号) — 宿主调用期经 dlsym(RTLD_DEFAULT) 获取
extern "C" hgs::seam::SessionControl* Hgs_getSessionControl(void)
{
    return &hgs::seam::g_sessionControl;
}
