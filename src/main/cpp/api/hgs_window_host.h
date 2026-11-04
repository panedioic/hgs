#pragma once
// ============================================================================
// hgs_window_host.h — 平台窗口操作接口 (阶段 2 W1)。
//
// core → host (UI 线程): 默认 ArkTS host 经 NAPI/TSFN 转成 startAbility /
// window.Window API; 原生 host 可直接实现。所有方法返回 void — 调用即
// "已投递", 失败由 host 调 HgsWindowManager::NotifyHostClosed 回滚。
//
// host → core 的回执不放在本接口: 直接调 HgsWindowManager::NotifyHost*。
// ============================================================================

#include <cstdint>
#include <string>

#include "hgs_export.h"
#include "hgs_window_types.h"

namespace hgs {

class HGS_API IWindowHost {
public:
    virtual ~IWindowHost() = default;

    virtual void CreateWindow(const WindowSpec& spec) = 0;
    virtual void DestroyWindow(WindowId id) = 0;
    virtual void SetGeometry(WindowId id, const WindowGeometry& geometry) = 0;
    virtual void SetVisible(WindowId id, bool visible) = 0;
    virtual void SetTitle(WindowId id, const std::string& title) = 0;
    virtual void SetState(WindowId id, WindowState state) = 0;
    virtual void StartMove(WindowId id) = 0;
    virtual void StartResize(WindowId id, uint32_t edges) = 0;
    virtual void RaiseWindow(WindowId id) = 0;
    virtual void RequestClose(WindowId id) = 0;
};

} // namespace hgs
