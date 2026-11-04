// bridge/napi_host_adapter.cpp — NapiHostAdapter 实现 (阶段 1 P1.1)
//
// TSFN 机制自 compositor/wayland_server.cpp 的 hgs::tl/hgs::ws NAPI 段迁入
// (CallJsToplevel/CallJsState 逐字语义, 日志保留), 换宿主: 旧直挂
// WaylandServer 回调 lambda → 现实现 IHgsHost, 经 HgsInstance::SetHost 注册。

#include "napi_host_adapter.h"

#include <cstdlib>
#include <string>

#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_DOMAIN 0x0000
#define LOG_TAG "HGS_NAPI"
#include <hilog/log.h>

#include "hgs_events.h"  // ToplevelEventName (字符串模式回转, JS 契约逐字)

namespace hgs {

namespace {

// —— TSFN JS 调用器 (自 wayland_server.cpp 迁入, 逐字语义) ——

struct ToplevelJsEvent {
    uint32_t id;
    bool typed;         // true = (id, type, json); false = (id, event, json)
    uint32_t type;      // typed 模式
    std::string event;  // string 模式
    std::string data;
};

void CallJsToplevel(napi_env env, napi_value cb, void*, void* raw) {
    auto* ev = static_cast<ToplevelJsEvent*>(raw);
    if (env && cb && ev) {
        OH_LOG_INFO(LOG_APP, "[MW-TSCB] calling JS toplevel cb: id=%{public}u event=%{public}s data=%{public}s",
                    ev->id, ev->event.c_str(), ev->data.c_str());
        napi_value undef, args[3];
        napi_get_undefined(env, &undef);
        napi_create_uint32(env, ev->id, &args[0]);
        if (ev->typed) {
            napi_create_uint32(env, ev->type, &args[1]);
        } else {
            napi_create_string_utf8(env, ev->event.c_str(), NAPI_AUTO_LENGTH, &args[1]);
        }
        napi_create_string_utf8(env, ev->data.c_str(), NAPI_AUTO_LENGTH, &args[2]);
        napi_call_function(env, undef, cb, 3, args, nullptr);
    }
    delete ev;
}

void CallJsState(napi_env env, napi_value cb, void*, void* data) {
    char* msg = static_cast<char*>(data);
    if (env && cb && msg) {
        napi_value undef, arg;
        napi_get_undefined(env, &undef);
        napi_create_string_utf8(env, msg, NAPI_AUTO_LENGTH, &arg);
        napi_call_function(env, undef, cb, 1, &arg, nullptr);
    }
    free(msg);
}

napi_threadsafe_function MakeTsfn(napi_env env, napi_value cb, const char* name,
                                  napi_threadsafe_function_call_js callJs) {
    napi_value fname;
    napi_create_string_utf8(env, name, NAPI_AUTO_LENGTH, &fname);
    napi_threadsafe_function tsfn = nullptr;
    napi_create_threadsafe_function(env, cb, nullptr, fname,
                                    0, 1, nullptr, nullptr, nullptr, callJs, &tsfn);
    return tsfn;
}

} // namespace

NapiHostAdapter& NapiHostAdapter::Get() {
    static NapiHostAdapter s;
    return s;
}

void NapiHostAdapter::SetToplevelString(napi_env env, napi_value cb) {
    if (toplevelTsfn_) {
        napi_release_threadsafe_function(toplevelTsfn_, napi_tsfn_release);
        toplevelTsfn_ = nullptr;
    }
    toplevelTyped_ = false;
    toplevelTsfn_ = MakeTsfn(env, cb, "WLToplevel", CallJsToplevel);
}

void NapiHostAdapter::SetToplevelTyped(napi_env env, napi_value cb) {
    if (toplevelTsfn_) {
        napi_release_threadsafe_function(toplevelTsfn_, napi_tsfn_release);
        toplevelTsfn_ = nullptr;
    }
    toplevelTyped_ = true;
    toplevelTsfn_ = MakeTsfn(env, cb, "HgsToplevelEvent", CallJsToplevel);
}

void NapiHostAdapter::SetState(napi_env env, napi_value cb) {
    if (stateTsfn_) {
        napi_release_threadsafe_function(stateTsfn_, napi_tsfn_release);
        stateTsfn_ = nullptr;
    }
    stateTsfn_ = MakeTsfn(env, cb, "HgsState", CallJsState);
}

void NapiHostAdapter::OnToplevelEvent(const ToplevelEvent& event) {
    if (!toplevelTsfn_) return;
    OH_LOG_INFO(LOG_APP, "[MW-TSCB] enqueue toplevel cb: id=%{public}u event=%{public}s",
                event.id, ToplevelEventName(event.type));
    auto* ev = new ToplevelJsEvent();
    ev->id = event.id;
    ev->typed = toplevelTyped_;
    ev->type = static_cast<uint32_t>(event.type);
    ev->event = ToplevelEventName(event.type);  // 字符串模式回转, JS 契约逐字
    ev->data = std::string(event.json);
    napi_call_threadsafe_function(toplevelTsfn_, ev, napi_tsfn_blocking);
}

void NapiHostAdapter::OnStateChanged(std::string_view state) {
    if (!stateTsfn_) return;
    napi_call_threadsafe_function(stateTsfn_,
                                  strdup(std::string(state).c_str()),
                                  napi_tsfn_blocking);
}

} // namespace hgs
