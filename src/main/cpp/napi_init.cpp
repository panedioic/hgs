// hgs 原生入口 — NativeXComponent 模式 (参照 blender-harmonyos):
// XComponent(libraryname:'hgs') 触发模块 Init 时 exports 携带
// OH_NATIVE_XCOMPONENT_OBJ, 在此注册 surface 回调; 生命周期由
// OnSurfaceCreated/Changed/Destroyed 在原生层驱动, 不再暴露
// create/destroy 导出。


#include <ace/xcomponent/native_interface_xcomponent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <cstdint>
#include <hilog/log.h>

#undef LOG_TAG
#define LOG_TAG "HGS_NAPI"
#include <mutex>
#include <unordered_map>
#include <window_manager/oh_window.h>
#include "napi/native_api.h"
#include "compositor/wayland_server.h"
#include "bridge/plugin_manager.h"
#include "input/input_manager.h"
#include "input/pointer_extras.h"
#include "input/game_controller_bridge.h"
#include "input/controller/controller_napi.h"
#include "input/text_input.h"

#include "graphics/egl_surface.h"


namespace {

// ============================================================================
// 以下为自 entry napi_init 迁入的合成器/输入/渲染器 NAPI 面 (方案 B1b):
// startServer/stopServer、toplevel 事件与状态 TSFN、渲染器管理、输入注入、
// IME、手柄/控制器。宿主 (entry) 的 ArkTS 侧经 libhgs.so 调用同名导出。
// ============================================================================

// -- NAPI: createRenderer -- (XComponentController.onSurfaceCreated 调用)
static napi_value CreateRenderer(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 2) {
        OH_LOG_ERROR(LOG_APP, "[MW-NAPI] createRenderer: need 2 args (toplevelId, surfaceId)");
        return nullptr;
    }
    uint32_t tid = 0;
    napi_get_value_uint32(env, args[0], &tid);
    int64_t surfaceId = 0;
    bool lossless = true;
    napi_status s = napi_get_value_bigint_int64(env, args[1], &surfaceId, &lossless);
    if (s != napi_ok) {
        OH_LOG_ERROR(LOG_APP, "[MW-NAPI] createRenderer: BIGINT parse failed status=%{public}d", s);
        return nullptr;
    }
    OH_LOG_INFO(LOG_APP, "[MW-NAPI] createRenderer tl=%{public}u surfaceId=%{public}ld", tid, surfaceId);
    PluginManager::GetInstance()->CreateRenderer(tid, surfaceId);
    return nullptr;
}

// -- NAPI: resizeRenderer -- (XComponentController.onSurfaceChanged 调用)
static napi_value ResizeRenderer(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 3) {
        OH_LOG_ERROR(LOG_APP, "[MW-NAPI] resizeRenderer: need 3 args (toplevelId, w, h)");
        return nullptr;
    }
    uint32_t tid = 0;
    napi_get_value_uint32(env, args[0], &tid);
    int32_t w = 0, h = 0;
    napi_get_value_int32(env, args[1], &w);
    napi_get_value_int32(env, args[2], &h);
    OH_LOG_INFO(LOG_APP, "[MW-NAPI] resizeRenderer tl=%{public}u %{public}dx%{public}d", tid, w, h);
    PluginManager::GetInstance()->ResizeRenderer(tid, w, h);
    return nullptr;
}

// -- NAPI: destroyRenderer -- (XComponentController.onSurfaceDestroyed 调用)
static napi_value DestroyRenderer(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    uint32_t tid = 0;
    if (argc >= 1) {
        napi_get_value_uint32(env, args[0], &tid);
    }
    OH_LOG_INFO(LOG_APP, "[MW-NAPI] destroyRenderer tl=%{public}u", tid);
    PluginManager::GetInstance()->DestroyToplevel(tid);
    return nullptr;
}

// -- Input forwarding NAPI (unified InputManager path) --
static napi_value SendPointerEvent(napi_env env, napi_callback_info info) {
    size_t argc = 8;
    napi_value args[8];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 5) return nullptr;
    uint32_t tl; int32_t action; double px, py; int32_t button;
    napi_get_value_uint32(env, args[0], &tl);
    napi_get_value_int32(env, args[1], &action);
    napi_get_value_double(env, args[2], &px);
    napi_get_value_double(env, args[3], &py);
    napi_get_value_int32(env, args[4], &button);
    // 可选: MouseEvent.rawDeltaX/Y (API15+, 仅 Move 传; 缺省 0 = 无 raw 数据,
    // InputManager 回退绝对差分); args[7] fromMouse: onMouse 物理鼠标通道
    // 传 true (触屏 onTouch 路径不传) — 相对模式 PRESS 是否跳过 enter 重定位
    double rawDx = 0, rawDy = 0;
    if (argc >= 7) {
        napi_get_value_double(env, args[5], &rawDx);
        napi_get_value_double(env, args[6], &rawDy);
    }
    bool fromMouse = false;
    if (argc >= 8) {
        napi_get_value_bool(env, args[7], &fromMouse);
    }
    // 跳过 MOVE (=3, 高频), 只记录 button/enter/leave。
    // 曾误写 action!=1: 跳过的是 PRESS (日志只见 Release 不见 Press),
    // 且 MOVE 全量刷屏 — ArkTS MouseAction: Press=1 Release=2 Move=3
    // (与 input_manager.cpp ACT_* 注释一致)
    if (action != 3) {
        OH_LOG_INFO(LOG_APP, "[PIPE] ptr tl=%{public}u a=%{public}d btn=0x%{public}x "
                    "px=(%{public}.0f,%{public}.0f) raw=(%{public}.1f,%{public}.1f) fromMouse=%{public}d",
                    tl, action, button, px, py, rawDx, rawDy, fromMouse ? 1 : 0);
    }
    InputManager::GetInstance()->SendPointerEvent(tl, action, px, py, button, rawDx, rawDy, fromMouse);
    return nullptr;
}

static napi_value SendKeyEvent(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 3) return nullptr;
    uint32_t tl; int32_t evdevCode; bool pressed;
    napi_get_value_uint32(env, args[0], &tl);
    napi_get_value_int32(env, args[1], &evdevCode);
    napi_get_value_bool(env, args[2], &pressed);
    OH_LOG_INFO(LOG_APP, "[PIPE] key tl=%{public}u evdev=%{public}d down=%{public}s",
                tl, evdevCode, pressed ? "true" : "false");
    InputManager::GetInstance()->SendKeyEvent(tl, evdevCode, pressed);
    return nullptr;
}

static napi_value SendScrollEvent(napi_env env, napi_callback_info info) {
    size_t argc = 6;
    napi_value args[6];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 6) return nullptr;
    uint32_t tl; int32_t axis; double value; int32_t scrollStep; double px; double py;
    napi_get_value_uint32(env, args[0], &tl);
    napi_get_value_int32(env, args[1], &axis);
    napi_get_value_double(env, args[2], &value);
    napi_get_value_int32(env, args[3], &scrollStep);
    napi_get_value_double(env, args[4], &px);
    napi_get_value_double(env, args[5], &py);
    OH_LOG_INFO(LOG_APP, "[PIPE] scroll tl=%{public}u axis=%{public}s val=%{public}.1f step=%{public}d px=(%{public}.0f,%{public}.0f)",
                tl, axis == 0 ? "VERT" : "HORIZ", value, scrollStep, px, py);
    InputManager::GetInstance()->SendScrollEvent(tl, axis, value, scrollStep, px, py);
    return nullptr;
}

// -- NAPI: registerHostWindow -- (ets 各 Ability 注册主窗口 id, 供
// OH_WindowManager_LockCursor 锁定光标用 — 仅获焦窗口能锁, 逐个尝试)
static napi_value RegisterHostWindow(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) return nullptr;
    int32_t windowId = 0;
    napi_get_value_int32(env, args[0], &windowId);
    PointerExtras::RegisterHostWindow(windowId);
    return nullptr;
}

// -- NAPI: dumpWindowLayout -- (WMS 全窗口布局列表, 层级排序 index 0 = 最高)
// 白屏 z 序判定: Fusion 主窗 (335 类) 与画面 popup (336 类) 的 rect 在列表中
// 的先后; 与 ArkTS 侧 subWinId 打点对照归属。
static napi_value DumpWindowLayout(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    int64_t displayId = 0;
    if (argc >= 1) napi_get_value_int64(env, args[0], &displayId);
    WindowManager_Rect* list = nullptr;
    size_t count = 0;
    const int32_t ret = OH_WindowManager_GetAllWindowLayoutInfoList(displayId, &list, &count);
    if (ret != 0 || !list) {
        OH_LOG_WARN(LOG_APP, "[WinLayout] Get table failed ret=%{public}d", ret);
        if (list) OH_WindowManager_ReleaseAllWindowLayoutInfoList(list);
        return nullptr;
    }
    for (size_t i = 0; i < count; i++) {
        OH_LOG_INFO(LOG_APP,
                    "[WinLayout] idx=%{public}zu/%{public}zu rect=(%{public}d,%{public}d %{public}ux%{public}u)",
                    i, count, list[i].posX, list[i].posY, list[i].width, list[i].height);
    }
    OH_WindowManager_ReleaseAllWindowLayoutInfoList(list);
    return nullptr;
}

EXTERN_C_START
static napi_value Init(napi_env env, napi_value exports) {
    const napi_property_descriptor desc[] = {
        // Surface 生命周期函数绑定。（暂留，未来具体实现）
        {"bindXComponent",          nullptr, hgs::surface::BindXComponent,          nullptr, nullptr, nullptr, napi_default, nullptr },
        // 鼠标锁定函数绑定（暂时使用 ETS 实现，后续考虑更换为 Native 实现）
        {"setPointerLockCallback",  nullptr, hgs::pe::SetPointerLockCallbackNAPI,   nullptr, nullptr, nullptr, napi_default, nullptr },
        // IME 相关
        {"setImeCallback",          nullptr, hgs::ti::SetImeCallbackNAPI,           nullptr, nullptr, nullptr, napi_default, nullptr },
        {"sendImeCommit",           nullptr, hgs::ti::SendImeCommitNAPI,            nullptr, nullptr, nullptr, napi_default, nullptr },
        {"sendImePreedit",          nullptr, hgs::ti::SendImePreeditNAPI,           nullptr, nullptr, nullptr, napi_default, nullptr },
        {"imeBackspace",            nullptr, hgs::ti::ImeBackspaceNAPI,             nullptr, nullptr, nullptr, napi_default, nullptr },
        // TopLevel 相关
        {"setToplevelCallback",     nullptr, hgs::tl::SetToplevelCallbackNAPI,      nullptr, nullptr, nullptr, napi_default, nullptr },
        {"getCurrentToplevelId",    nullptr, hgs::tl::GetCurrentToplevelIdNAPI,     nullptr, nullptr, nullptr, napi_default, nullptr },
        {"setPendingToplevel",      nullptr, hgs::tl::SetPendingToplevelNAPI,       nullptr, nullptr, nullptr, napi_default, nullptr },
        {"cancelPendingToplevel",   nullptr, hgs::tl::CancelPendingToplevelNAPI,    nullptr, nullptr, nullptr, napi_default, nullptr },
        {"destroyToplevel",         nullptr, hgs::tl::DestroyToplevelNAPI,          nullptr, nullptr, nullptr, napi_default, nullptr },
        {"sendToplevelClose",       nullptr, hgs::tl::SendToplevelCloseNAPI,        nullptr, nullptr, nullptr, napi_default, nullptr },
        {"notifyToplevelResize",    nullptr, hgs::tl::NotifyToplevelResizeNAPI,     nullptr, nullptr, nullptr, napi_default, nullptr },
        {"notifyToplevelResizeEnd", nullptr, hgs::tl::NotifyToplevelResizeEndNAPI,  nullptr, nullptr, nullptr, napi_default, nullptr },
        {"findToplevelAt",          nullptr, hgs::tl::FindToplevelAtNAPI,           nullptr, nullptr, nullptr, napi_default, nullptr },
        {"raiseToplevel",           nullptr, hgs::tl::RaiseToplevelNAPI,            nullptr, nullptr, nullptr, napi_default, nullptr },
        {"setToplevelVisible",      nullptr, hgs::tl::SetToplevelVisibleNAPI,       nullptr, nullptr, nullptr, napi_default, nullptr },
        // Wayland Server 相关
        {"startServer",             nullptr, hgs::ws::StartServerNAPI,              nullptr, nullptr, nullptr, napi_default, nullptr },
        {"stopServer",              nullptr, hgs::ws::StopServerNAPI,               nullptr, nullptr, nullptr, napi_default, nullptr },
        {"setOutputSize",           nullptr, hgs::ws::SetOutputSizeNAPI,            nullptr, nullptr, nullptr, napi_default, nullptr },
        {"setDisplayScale",         nullptr, hgs::ws::SetDisplayScaleNAPI,          nullptr, nullptr, nullptr, napi_default, nullptr },
        {"setDesktopMode",          nullptr, hgs::ws::SetDesktopModeNAPI,           nullptr, nullptr, nullptr, napi_default, nullptr },
        {"getDesktopRootId",        nullptr, hgs::ws::GetDesktopRootIdNAPI,         nullptr, nullptr, nullptr, napi_default, nullptr },
        {"setStateCallback",        nullptr, hgs::ws::SetStateCallbackNAPI,         nullptr, nullptr, nullptr, napi_default, nullptr },
        {"createRenderer",        nullptr, CreateRenderer,        nullptr, nullptr, nullptr, napi_default, nullptr },
        {"resizeRenderer",        nullptr, ResizeRenderer,        nullptr, nullptr, nullptr, napi_default, nullptr },
        {"destroyRenderer",       nullptr, DestroyRenderer,       nullptr, nullptr, nullptr, napi_default, nullptr },
        {"sendPointerEvent",      nullptr, SendPointerEvent,      nullptr, nullptr, nullptr, napi_default, nullptr },
        {"sendKeyEvent",          nullptr, SendKeyEvent,          nullptr, nullptr, nullptr, napi_default, nullptr },
        {"sendScrollEvent",       nullptr, SendScrollEvent,       nullptr, nullptr, nullptr, napi_default, nullptr },
        {"registerHostWindow",    nullptr, RegisterHostWindow,    nullptr, nullptr, nullptr, napi_default, nullptr },
        {"dumpWindowLayout",      nullptr, DumpWindowLayout,      nullptr, nullptr, nullptr, napi_default, nullptr },
        {"initGameController",        nullptr, InitGameController,        nullptr, nullptr, nullptr, napi_default, nullptr },
        {"cleanupGameController",     nullptr, CleanupGameController,     nullptr, nullptr, nullptr, napi_default, nullptr },
        {"isGamepadConnected",        nullptr, IsGamepadConnected,        nullptr, nullptr, nullptr, napi_default, nullptr },
        {"getGamepadCount",           nullptr, GetGamepadCount,           nullptr, nullptr, nullptr, napi_default, nullptr },
        {"setGamepadButtonCallback",  nullptr, SetGamepadButtonCallback,  nullptr, nullptr, nullptr, napi_default, nullptr },
        {"setGamepadAxisCallback",    nullptr, SetGamepadAxisCallback,    nullptr, nullptr, nullptr, napi_default, nullptr },
        {"setGamepadDeviceCallback",  nullptr, SetGamepadDeviceCallback,  nullptr, nullptr, nullptr, napi_default, nullptr },
        {"setGamepadRumbleCallback",  nullptr, SetGamepadRumbleCallback,  nullptr, nullptr, nullptr, napi_default, nullptr },
        {"controllerSetEnabled",      nullptr, ControllerSetEnabled,      nullptr, nullptr, nullptr, napi_default, nullptr },
        {"controllerSetButton",       nullptr, ControllerSetButton,       nullptr, nullptr, nullptr, napi_default, nullptr },
        {"controllerSetAxis",         nullptr, ControllerSetAxis,         nullptr, nullptr, nullptr, napi_default, nullptr },
        {"controllerSetHat",          nullptr, ControllerSetHat,          nullptr, nullptr, nullptr, napi_default, nullptr },
        {"controllerResetSource",     nullptr, ControllerResetSource,     nullptr, nullptr, nullptr, napi_default, nullptr },
        {"controllerGetState",        nullptr, ControllerGetState,        nullptr, nullptr, nullptr, napi_default, nullptr },
        {"controllerGetStateText",    nullptr, ControllerGetStateText,    nullptr, nullptr, nullptr, napi_default, nullptr },
        {"controllerStartBridge",     nullptr, ControllerStartBridge,     nullptr, nullptr, nullptr, napi_default, nullptr },
        {"controllerStopBridge",      nullptr, ControllerStopBridge,      nullptr, nullptr, nullptr, napi_default, nullptr },
        {"controllerGetSocketPath",   nullptr, ControllerGetSocketPath,   nullptr, nullptr, nullptr, napi_default, nullptr },
        {"controllerSetOutputMode",   nullptr, ControllerSetOutputMode,   nullptr, nullptr, nullptr, napi_default, nullptr },
        {"controllerGetOutputMode",   nullptr, ControllerGetOutputMode,   nullptr, nullptr, nullptr, napi_default, nullptr },

    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);

    // XComponent(libraryname:'hgs') 触发的加载: exports 携带原生对象
    napi_value xc;
    if (napi_get_named_property(env, exports, OH_NATIVE_XCOMPONENT_OBJ, &xc) == napi_ok) {
        OH_NativeXComponent *nx = nullptr;
        if (napi_unwrap(env, xc, reinterpret_cast<void **>(&nx)) == napi_ok && nx != nullptr) {
            OH_NativeXComponent_RegisterCallback(nx, &hgs::surface::gXcompCb);
            char idBuf[128] = {0};
            uint64_t idLen = sizeof(idBuf);
            OH_NativeXComponent_GetXComponentId(nx, idBuf, &idLen);
            OH_LOG_INFO(LOG_APP, "[HGS-NAPI] XComponent callbacks registered id=%{public}s", idBuf);
        }
    }
    return exports;
}
EXTERN_C_END

static napi_module hgsModule = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,
    .nm_modname = "hgs",
    .nm_priv = nullptr,
    .reserved = {0},
};

extern "C" __attribute__((constructor)) void RegisterHgsModule(void) {
    napi_module_register(&hgsModule);
}

} // namespace
