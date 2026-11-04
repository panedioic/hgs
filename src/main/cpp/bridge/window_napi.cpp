// bridge/window_napi.cpp — 窗口管理器 NAPI 桥 (阶段 2 W2a)
//
// WindowEvent 从 wl 线程产生 → TSFN 投递到 JS 线程 (与 toplevel 通道同款)。
// host 回执 (notifyWindow*) 从 JS 线程直接调 manager (manager 内部加锁)。

#include "window_napi.h"

#include <cstring>
#include <string>
#include <vector>

#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_DOMAIN 0x0000
#define LOG_TAG "HGS_WindowNAPI"
#include <hilog/log.h>

#include "hgs_instance.h"
#include "hgs_window_types.h"

namespace hgs {
namespace window_napi {

namespace {

napi_threadsafe_function gWindowTsfn = nullptr;

napi_value MakeU32(napi_env env, uint32_t v) {
    napi_value r;
    napi_create_uint32(env, v, &r);
    return r;
}

napi_value MakeI32(napi_env env, int32_t v) {
    napi_value r;
    napi_create_int32(env, v, &r);
    return r;
}

napi_value MakeBool(napi_env env, bool v) {
    napi_value r;
    napi_get_boolean(env, v, &r);
    return r;
}

napi_value MakeStr(napi_env env, const std::string& v) {
    napi_value r;
    napi_create_string_utf8(env, v.c_str(), NAPI_AUTO_LENGTH, &r);
    return r;
}

void SetProp(napi_env env, napi_value obj, const char* key, napi_value value) {
    napi_set_named_property(env, obj, key, value);
}

napi_value BuildGeometry(napi_env env, const WindowGeometry& g) {
    napi_value obj;
    napi_create_object(env, &obj);
    SetProp(env, obj, "x", MakeI32(env, g.x));
    SetProp(env, obj, "y", MakeI32(env, g.y));
    SetProp(env, obj, "width", MakeI32(env, g.width));
    SetProp(env, obj, "height", MakeI32(env, g.height));
    return obj;
}

napi_value BuildSpec(napi_env env, const WindowSpec& s) {
    napi_value obj;
    napi_create_object(env, &obj);
    SetProp(env, obj, "id", MakeU32(env, s.id));
    SetProp(env, obj, "pid", MakeU32(env, s.clientPid));
    SetProp(env, obj, "appId", MakeStr(env, s.appId));
    SetProp(env, obj, "title", MakeStr(env, s.title));
    // 扁平几何 (与 Index.d.ts HgsWindowSpec 对应)
    SetProp(env, obj, "x", MakeI32(env, s.geometry.x));
    SetProp(env, obj, "y", MakeI32(env, s.geometry.y));
    SetProp(env, obj, "width", MakeI32(env, s.geometry.width));
    SetProp(env, obj, "height", MakeI32(env, s.geometry.height));
    SetProp(env, obj, "minWidth", MakeI32(env, s.minWidth));
    SetProp(env, obj, "minHeight", MakeI32(env, s.minHeight));
    SetProp(env, obj, "maxWidth", MakeI32(env, s.maxWidth));
    SetProp(env, obj, "maxHeight", MakeI32(env, s.maxHeight));
    SetProp(env, obj, "parentId", MakeU32(env, s.parentId));
    SetProp(env, obj, "modal", MakeBool(env, s.modal));
    SetProp(env, obj, "decorated", MakeBool(env, s.decorated));
    SetProp(env, obj, "visible", MakeBool(env, s.visible));
    SetProp(env, obj, "state", MakeU32(env, static_cast<uint32_t>(s.state)));
    return obj;
}

napi_value BuildWindowEvent(napi_env env, const WindowEvent& e) {
    napi_value obj;
    napi_create_object(env, &obj);
    SetProp(env, obj, "type", MakeU32(env, static_cast<uint32_t>(e.type)));
    SetProp(env, obj, "id", MakeU32(env, e.id));
    SetProp(env, obj, "spec", BuildSpec(env, e.spec));
    SetProp(env, obj, "geometry", BuildGeometry(env, e.geometry));
    SetProp(env, obj, "state", MakeU32(env, static_cast<uint32_t>(e.state)));
    SetProp(env, obj, "focused", MakeBool(env, e.focused));
    SetProp(env, obj, "edges", MakeU32(env, e.edges));
    return obj;
}

void CallJsWindowEvent(napi_env env, napi_value cb, void*, void* raw) {
    auto* event = static_cast<WindowEvent*>(raw);
    if (env && cb && event) {
        napi_value arg = BuildWindowEvent(env, *event);
        napi_call_function(env, nullptr, cb, 1, &arg, nullptr);
    }
    delete event;
}

} // namespace

napi_value SetWindowCallbackNAPI(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) return nullptr;

    if (gWindowTsfn) {
        napi_release_threadsafe_function(gWindowTsfn, napi_tsfn_release);
        gWindowTsfn = nullptr;
    }
    napi_value name;
    napi_create_string_utf8(env, "HgsWindowEvent", NAPI_AUTO_LENGTH, &name);
    napi_create_threadsafe_function(env, args[0], nullptr, name,
                                    0, 1, nullptr, nullptr, nullptr,
                                    CallJsWindowEvent, &gWindowTsfn);

    HgsInstance::Instance().WindowManager().SetEventSink(
        [](const WindowEvent& event) {
            if (!gWindowTsfn) return;
            // wl 线程: 拷贝事件后非阻塞投递 (队列满时丢弃, 不阻塞合成器)
            auto* copy = new WindowEvent(event);
            if (napi_call_threadsafe_function(gWindowTsfn, copy,
                                              napi_tsfn_nonblocking) != napi_ok) {
                delete copy;
            }
        });
    return nullptr;
}

napi_value NotifyWindowGeometryNAPI(napi_env env, napi_callback_info info) {
    size_t argc = 5;
    napi_value args[5];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 5) return nullptr;
    uint32_t id = 0;
    int32_t x = 0, y = 0, w = 0, h = 0;
    napi_get_value_uint32(env, args[0], &id);
    napi_get_value_int32(env, args[1], &x);
    napi_get_value_int32(env, args[2], &y);
    napi_get_value_int32(env, args[3], &w);
    napi_get_value_int32(env, args[4], &h);
    HgsInstance::Instance().WindowManager().NotifyHostGeometry(id, {x, y, w, h});
    return nullptr;
}

napi_value NotifyWindowFocusNAPI(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 2) return nullptr;
    uint32_t id = 0;
    bool focused = false;
    napi_get_value_uint32(env, args[0], &id);
    napi_get_value_bool(env, args[1], &focused);
    HgsInstance::Instance().WindowManager().NotifyHostFocus(id, focused);
    return nullptr;
}

napi_value NotifyWindowStateNAPI(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 2) return nullptr;
    uint32_t id = 0, state = 0;
    napi_get_value_uint32(env, args[0], &id);
    napi_get_value_uint32(env, args[1], &state);
    if (state > static_cast<uint32_t>(WindowState::Fullscreen)) return nullptr;
    HgsInstance::Instance().WindowManager().NotifyHostState(
        id, static_cast<WindowState>(state));
    return nullptr;
}

napi_value NotifyWindowClosedNAPI(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    uint32_t id = 0;
    if (argc >= 1) napi_get_value_uint32(env, args[0], &id);
    HgsInstance::Instance().WindowManager().NotifyHostClosed(id);
    return nullptr;
}

napi_value GetWindowNAPI(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    uint32_t id = 0;
    if (argc >= 1) napi_get_value_uint32(env, args[0], &id);
    WindowSpec spec;
    if (!HgsInstance::Instance().WindowManager().GetWindow(id, &spec)) {
        napi_value nullValue;
        napi_get_null(env, &nullValue);
        return nullValue;
    }
    return BuildSpec(env, spec);
}

napi_value ListWindowsNAPI(napi_env env, napi_callback_info) {
    const std::vector<WindowSpec> list =
        HgsInstance::Instance().WindowManager().ListWindows();
    napi_value arr;
    napi_create_array_with_length(env, list.size(), &arr);
    for (size_t i = 0; i < list.size(); i++) {
        napi_set_element(env, arr, static_cast<uint32_t>(i), BuildSpec(env, list[i]));
    }
    return arr;
}

napi_value GetWindowZOrderNAPI(napi_env env, napi_callback_info) {
    const std::vector<WindowId> order =
        HgsInstance::Instance().WindowManager().ZOrder();
    napi_value arr;
    napi_create_array_with_length(env, order.size(), &arr);
    for (size_t i = 0; i < order.size(); i++) {
        napi_set_element(env, arr, static_cast<uint32_t>(i), MakeU32(env, order[i]));
    }
    return arr;
}

} // namespace window_napi
} // namespace hgs
