#pragma once
// ============================================================================
// hgs_host.h — 宿主回调接口 (阶段 0, P0.2)。
//
// 出处: docs/architecture/hgs-shared-runtime-phase0.md §4.5。
// 宿主实现 IHgsHost 并经 HgsInstance::SetHost 注册; HGS 经 OnToplevelEvent/
// OnStateChanged 通知宿主创建窗口/处理会话状态。HGS 不直接 startAbility、
// 不解析项目资源路径 (phase0 设计原则 2)。
//
// 线程契约: 两个回调都在 Wayland 事件循环线程触发 —
//   - 禁止在回调内调用 HgsInstance::Stop() (会 join 自身, 死锁)
//   - 回调应快速返回, 跨线程投递由宿主自行负责 (NAPI 侧经 TSFN,
//     见 napi_init.cpp 既有模式)
// ============================================================================

#include <string_view>

#include "hgs_export.h"
#include "hgs_events.h"
#include "hgs_types.h"

namespace hgs {

// toplevel 事件 (typed 契约)。
// json 与现有 ToplevelEventBus 的 snprintf 模板逐字一致, 原样透传不做
// 解析 (标题转义为已知历史行为, phase0 §6); 仅在回调期间有效, 需要保留
// 请自行拷贝。
struct HGS_API ToplevelEvent {
    ToplevelId id = 0;
    ToplevelEventType type = ToplevelEventType::Unknown;
    std::string_view json = "{}";
};

class HGS_API IHgsHost {
public:
    virtual ~IHgsHost() = default;

    // toplevel 生命周期/状态事件 (Wayland 线程)。
    // 类型 = hgs::ToplevelEventType (显式数值契约, api/hgs_events.h)。
    virtual void OnToplevelEvent(const ToplevelEvent& event) = 0;

    // 引擎状态消息 (Wayland 线程): 现有值 "active" (会话首帧) /
    // "evt:desktop-ready" (桌面 root 就绪旁路); 与 setStateCallback
    // 字符串逐字一致。
    virtual void OnStateChanged(std::string_view state) = 0;
};

} // namespace hgs
