#pragma once
// ============================================================================
// napi_host_adapter.h — NAPI 层的 IHgsHost 实现 (阶段 1 P1.1)。
//
// setToplevelCallback (字符串版) / setToplevelEventCallback (typed 版) /
// setStateCallback 的 JS 回调经 TSFN 由本类承载, 经 HgsInstance::SetHost
// 注册。阶段 1 起 NAPI 层不再直连 WaylandServer::SetToplevelCallback/
// SetStateCallback — bus 单 sink 由 HgsInstance::Start 唯一安装, 事件统一
// 走 bus → HgsInstance 适配 → IHgsHost → 本类 → TSFN → JS
// (docs/architecture/hgs-shared-runtime-phase0.md §7)。
//
// toplevel 通道双模式 (字符串/typed): 后注册者覆盖前者 (TSFN release +
// 重建), 与旧单 sink 语义一致, mode = 最后一次注册。线程约定与旧实现
// 逐字: 注册在 JS 线程; OnToplevelEvent/OnStateChanged 在 wl 线程触发,
// 经 TSFN 投递回 JS 线程。
// ============================================================================

#include <napi/native_api.h>

#include "hgs_host.h"

namespace hgs {

class NapiHostAdapter : public IHgsHost {
public:
    static NapiHostAdapter& Get();

    // JS: cb(id: number, event: string, json: string) — 旧字符串契约
    void SetToplevelString(napi_env env, napi_value cb);
    // JS: cb(id: number, type: number, json: string) — typed 契约
    // (type = HgsToplevelEventType 数值, hgs-exported-api.md §4.2)
    void SetToplevelTyped(napi_env env, napi_value cb);
    // JS: cb(state: string)
    void SetState(napi_env env, napi_value cb);

    // IHgsHost (wl 线程)
    void OnToplevelEvent(const ToplevelEvent& event) override;
    void OnStateChanged(std::string_view state) override;

private:
    NapiHostAdapter() = default;

    napi_threadsafe_function toplevelTsfn_ = nullptr;
    bool toplevelTyped_ = false;
    napi_threadsafe_function stateTsfn_ = nullptr;
};

} // namespace hgs
