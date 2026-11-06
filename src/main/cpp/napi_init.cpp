// hgs/src/main/cpp/napi_init.cpp - hgs 原生函数导出
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
#include "bridge/window_napi.h"
#include "input/input_manager.h"
#include "input/pointer_extras.h"
#include "input/game_controller_bridge.h"
#include "input/controller/controller_napi.h"
#include "input/text_input.h"
#include "graphics/egl_surface.h"
#include "logger/logger.h"

namespace {

// -- NAPI: dumpWindowLayout -- (WMS 全窗口布局列表, 层级排序 index 0 = 最高)
// 调试工具，暂不特殊处理
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

// ── XComponent 回调的延迟注册 ────────────────────────────────────────────
// napi register 函数在 JS 首次 import libhgs.so 时只跑一次, 那时 ArkUI 通常
// 还没创建 XComponent → unwrap 失败 → OH_NativeXComponent 回调永远没注册
// (实测: 多窗口里第二个窗口起, native mouse/touch 全部静默)。
// 修法: 记住 env/exports, HgsSurface.onLoad → bindXComponent 时重试 unwrap
// (此时 ArkUI 已把 OH_NativeXComponent 挂到 exports 上)。
static napi_env gXcEnv = nullptr;
static napi_ref gXcExportsRef = nullptr;
static bool gXcRegistered = false;

// 返回 true = 已注册 (或早已注册); false = unwrap 仍失败 (XComponent 未创建)
bool RetryXComponentRegister()
{
    if (gXcRegistered || gXcEnv == nullptr || gXcExportsRef == nullptr) return gXcRegistered;
    napi_value exports = nullptr;
    if (napi_get_reference_value(gXcEnv, gXcExportsRef, &exports) != napi_ok || !exports) {
        return false;
    }
    napi_value xc = nullptr;
    if (napi_get_named_property(gXcEnv, exports, OH_NATIVE_XCOMPONENT_OBJ, &xc) != napi_ok) {
        return false;
    }
    OH_NativeXComponent* nx = nullptr;
    if (napi_unwrap(gXcEnv, xc, reinterpret_cast<void**>(&nx)) != napi_ok || nx == nullptr) {
        return false;
    }
    OH_NativeXComponent_RegisterCallback(nx, &hgs::surface::gXcompCb);
    char idBuf[128] = {0};
    uint64_t idLen = sizeof(idBuf);
    OH_NativeXComponent_GetXComponentId(nx, idBuf, &idLen);
    OH_LOG_INFO(LOG_APP, "[HGS-NAPI] XComponent callbacks registered (deferred) id=%{public}s comp=%{public}p",
                idBuf, static_cast<void*>(nx));
    gXcRegistered = true;
    return true;
}

// NAPI: hgs.regXComponentCallback() — HgsSurface.onLoad 调, 重试注册。
// 进程内只有一个 ArkTS VM, register 时存的 env/exports 即 XComponent 所在的
// env; ArkUI 在创建 XComponent 时才把原生对象挂上 exports (import 时还没有),
// 所以 onLoad 时刻重试 unwrap 就能拿到并注册回调。
napi_value RegXComponentCallbackNAPI(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    // 可选参数 force: 传 true 时无视已注册标记再试一次 (多 XComponent 场景:
    // 第二个 XComponent 的注册也要走 unwrap, 不能被第一个的 registered 挡住)
    bool force = false;
    if (argc >= 1) {
        napi_get_value_bool(env, argv[0], &force);
    }
    if (force) gXcRegistered = false;
    bool ok = RetryXComponentRegister();
    if (!ok) {
        OH_LOG_WARN(LOG_APP, "[HGS-NAPI] regXComponentCallback: 重试仍失败 (XComponent 未创建?)");
    }
    napi_value res = nullptr;
    napi_get_boolean(env, ok, &res);
    return res;
}

EXTERN_C_START
static napi_value Init(napi_env env, napi_value exports) {
    const napi_property_descriptor desc[] = {
        // Surface 生命周期函数绑定。（暂留，未来具体实现）
        {"bindXComponent",          nullptr, hgs::surface::BindXComponent,          nullptr, nullptr, nullptr, napi_default, nullptr },
        // 渲染器表管理 (定义暂在 plugin_manager, todo 迁入 egl_surface)
        {"createRenderer",          nullptr, hgs::surface::CreateRendererNAPI,      nullptr, nullptr, nullptr, napi_default, nullptr },
        {"resizeRenderer",          nullptr, hgs::surface::ResizeRendererNAPI,      nullptr, nullptr, nullptr, napi_default, nullptr },
        {"destroyRenderer",         nullptr, hgs::surface::DestroyRendererNAPI,     nullptr, nullptr, nullptr, napi_default, nullptr },
        // 鼠标锁定函数绑定（暂时使用 ETS 实现，后续考虑更换为 Native 实现）
        {"setPointerLockCallback",  nullptr, hgs::pe::SetPointerLockCallbackNAPI,   nullptr, nullptr, nullptr, napi_default, nullptr },
        {"registerHostWindow",      nullptr, hgs::pe::RegisterHostWindowNAPI,       nullptr, nullptr, nullptr, napi_default, nullptr },
        // IME 相关
        {"setImeCallback",          nullptr, hgs::ti::SetImeCallbackNAPI,           nullptr, nullptr, nullptr, napi_default, nullptr },
        {"sendImeCommit",           nullptr, hgs::ti::SendImeCommitNAPI,            nullptr, nullptr, nullptr, napi_default, nullptr },
        {"sendImePreedit",          nullptr, hgs::ti::SendImePreeditNAPI,           nullptr, nullptr, nullptr, napi_default, nullptr },
        {"imeBackspace",            nullptr, hgs::ti::ImeBackspaceNAPI,             nullptr, nullptr, nullptr, napi_default, nullptr },
        // TopLevel 相关
        {"setToplevelCallback",     nullptr, hgs::tl::SetToplevelCallbackNAPI,      nullptr, nullptr, nullptr, napi_default, nullptr },
        {"setToplevelEventCallback", nullptr, hgs::tl::SetToplevelEventCallbackNAPI, nullptr, nullptr, nullptr, napi_default, nullptr },
        {"getCurrentToplevelId",    nullptr, hgs::tl::GetCurrentToplevelIdNAPI,     nullptr, nullptr, nullptr, napi_default, nullptr },
        {"takePendingToplevel",     nullptr, hgs::tl::TakePendingToplevelNAPI,      nullptr, nullptr, nullptr, napi_default, nullptr },
        {"setPendingToplevel",      nullptr, hgs::tl::SetPendingToplevelNAPI,       nullptr, nullptr, nullptr, napi_default, nullptr },
        {"cancelPendingToplevel",   nullptr, hgs::tl::CancelPendingToplevelNAPI,    nullptr, nullptr, nullptr, napi_default, nullptr },
        {"destroyToplevel",         nullptr, hgs::tl::DestroyToplevelNAPI,          nullptr, nullptr, nullptr, napi_default, nullptr },
        {"sendToplevelClose",       nullptr, hgs::tl::SendToplevelCloseNAPI,        nullptr, nullptr, nullptr, napi_default, nullptr },
        {"closeToplevel",           nullptr, hgs::tl::CloseToplevelNAPI,            nullptr, nullptr, nullptr, napi_default, nullptr },
        {"notifyToplevelResize",    nullptr, hgs::tl::NotifyToplevelResizeNAPI,     nullptr, nullptr, nullptr, napi_default, nullptr },
        {"notifyToplevelResizeEnd", nullptr, hgs::tl::NotifyToplevelResizeEndNAPI,  nullptr, nullptr, nullptr, napi_default, nullptr },
        {"findToplevelAt",          nullptr, hgs::tl::FindToplevelAtNAPI,           nullptr, nullptr, nullptr, napi_default, nullptr },
        {"raiseToplevel",           nullptr, hgs::tl::RaiseToplevelNAPI,            nullptr, nullptr, nullptr, napi_default, nullptr },
        {"setToplevelVisible",      nullptr, hgs::tl::SetToplevelVisibleNAPI,       nullptr, nullptr, nullptr, napi_default, nullptr },
        // Wayland Server 相关
        {"startServer",             nullptr, hgs::ws::StartServerNAPI,              nullptr, nullptr, nullptr, napi_default, nullptr },
        {"stopServer",              nullptr, hgs::ws::StopServerNAPI,               nullptr, nullptr, nullptr, napi_default, nullptr },
        {"isRunning",               nullptr, hgs::ws::IsRunningNAPI,                nullptr, nullptr, nullptr, napi_default, nullptr },
        {"getWaylandDisplayName",   nullptr, hgs::ws::GetWaylandDisplayNameNAPI,    nullptr, nullptr, nullptr, napi_default, nullptr },
        {"getClientEnvironment",    nullptr, hgs::ws::GetClientEnvironmentNAPI,     nullptr, nullptr, nullptr, napi_default, nullptr },
        {"getApiVersion",           nullptr, hgs::ws::GetApiVersionNAPI,            nullptr, nullptr, nullptr, napi_default, nullptr },
        {"getLastError",            nullptr, hgs::ws::GetLastErrorNAPI,             nullptr, nullptr, nullptr, napi_default, nullptr },
        {"setInputBackend",         nullptr, hgs::ws::SetInputBackendNAPI,          nullptr, nullptr, nullptr, napi_default, nullptr },
        {"getInputBackend",         nullptr, hgs::ws::GetInputBackendNAPI,          nullptr, nullptr, nullptr, napi_default, nullptr },
        {"setOutputSize",           nullptr, hgs::ws::SetOutputSizeNAPI,            nullptr, nullptr, nullptr, napi_default, nullptr },
        {"setDisplayScale",         nullptr, hgs::ws::SetDisplayScaleNAPI,          nullptr, nullptr, nullptr, napi_default, nullptr },
        {"setDesktopMode",          nullptr, hgs::ws::SetDesktopModeNAPI,           nullptr, nullptr, nullptr, napi_default, nullptr },
        {"getDesktopRootId",        nullptr, hgs::ws::GetDesktopRootIdNAPI,         nullptr, nullptr, nullptr, napi_default, nullptr },
        {"setStateCallback",        nullptr, hgs::ws::SetStateCallbackNAPI,         nullptr, nullptr, nullptr, napi_default, nullptr },
        // 输入相关
        {"sendPointerEvent",        nullptr, hgs::im::SendPointerEventNAPI,         nullptr, nullptr, nullptr, napi_default, nullptr },
        {"sendKeyEvent",            nullptr, hgs::im::SendKeyEventNAPI,             nullptr, nullptr, nullptr, napi_default, nullptr },
        {"sendScrollEvent",         nullptr, hgs::im::SendScrollEventNAPI,          nullptr, nullptr, nullptr, napi_default, nullptr },
        // Surface 生命周期 (阶段 1 canonical; 旧名 createRenderer 等见上方注释区)
        {"attachSurface",           nullptr, hgs::surface::AttachSurfaceNAPI,       nullptr, nullptr, nullptr, napi_default, nullptr },
        {"resizeSurface",           nullptr, hgs::surface::ResizeSurfaceNAPI,       nullptr, nullptr, nullptr, napi_default, nullptr },
        {"detachSurface",           nullptr, hgs::surface::DetachSurfaceNAPI,       nullptr, nullptr, nullptr, napi_default, nullptr },
        // 窗口管理器 (阶段 2 W2a)
        {"setWindowCallback",       nullptr, hgs::window_napi::SetWindowCallbackNAPI, nullptr, nullptr, nullptr, napi_default, nullptr },
        {"notifyWindowGeometry",    nullptr, hgs::window_napi::NotifyWindowGeometryNAPI, nullptr, nullptr, nullptr, napi_default, nullptr },
        {"notifyWindowFocus",       nullptr, hgs::window_napi::NotifyWindowFocusNAPI, nullptr, nullptr, nullptr, napi_default, nullptr },
        {"notifyWindowState",       nullptr, hgs::window_napi::NotifyWindowStateNAPI, nullptr, nullptr, nullptr, napi_default, nullptr },
        {"notifyWindowClosed",      nullptr, hgs::window_napi::NotifyWindowClosedNAPI, nullptr, nullptr, nullptr, napi_default, nullptr },
        {"getWindow",               nullptr, hgs::window_napi::GetWindowNAPI,       nullptr, nullptr, nullptr, napi_default, nullptr },
        {"listWindows",             nullptr, hgs::window_napi::ListWindowsNAPI,     nullptr, nullptr, nullptr, napi_default, nullptr },
        {"getWindowZOrder",         nullptr, hgs::window_napi::GetWindowZOrderNAPI, nullptr, nullptr, nullptr, napi_default, nullptr },
        // Controller and GamePad 相关
        {"initGameController",          nullptr, InitGameController,                nullptr, nullptr, nullptr, napi_default, nullptr },
        {"cleanupGameController",       nullptr, CleanupGameController,             nullptr, nullptr, nullptr, napi_default, nullptr },
        {"isGamepadConnected",          nullptr, IsGamepadConnected,                nullptr, nullptr, nullptr, napi_default, nullptr },
        {"getGamepadCount",             nullptr, GetGamepadCount,                   nullptr, nullptr, nullptr, napi_default, nullptr },
        {"setGamepadButtonCallback",    nullptr, SetGamepadButtonCallback,          nullptr, nullptr, nullptr, napi_default, nullptr },
        {"setGamepadAxisCallback",      nullptr, SetGamepadAxisCallback,            nullptr, nullptr, nullptr, napi_default, nullptr },
        {"setGamepadDeviceCallback",    nullptr, SetGamepadDeviceCallback,          nullptr, nullptr, nullptr, napi_default, nullptr },
        {"setGamepadRumbleCallback",    nullptr, SetGamepadRumbleCallback,          nullptr, nullptr, nullptr, napi_default, nullptr },
        {"controllerSetEnabled",        nullptr, ControllerSetEnabled,              nullptr, nullptr, nullptr, napi_default, nullptr },
        {"controllerSetButton",         nullptr, ControllerSetButton,               nullptr, nullptr, nullptr, napi_default, nullptr },
        {"controllerSetAxis",           nullptr, ControllerSetAxis,                 nullptr, nullptr, nullptr, napi_default, nullptr },
        {"controllerSetHat",            nullptr, ControllerSetHat,                  nullptr, nullptr, nullptr, napi_default, nullptr },
        {"controllerResetSource",       nullptr, ControllerResetSource,             nullptr, nullptr, nullptr, napi_default, nullptr },
        {"controllerGetState",          nullptr, ControllerGetState,                nullptr, nullptr, nullptr, napi_default, nullptr },
        {"controllerGetStateText",      nullptr, ControllerGetStateText,            nullptr, nullptr, nullptr, napi_default, nullptr },
        {"controllerStartBridge",       nullptr, ControllerStartBridge,             nullptr, nullptr, nullptr, napi_default, nullptr },
        {"controllerStopBridge",        nullptr, ControllerStopBridge,              nullptr, nullptr, nullptr, napi_default, nullptr },
        {"controllerGetSocketPath",     nullptr, ControllerGetSocketPath,           nullptr, nullptr, nullptr, napi_default, nullptr },
        {"controllerSetOutputMode",     nullptr, ControllerSetOutputMode,           nullptr, nullptr, nullptr, napi_default, nullptr },
        {"controllerGetOutputMode",     nullptr, ControllerGetOutputMode,           nullptr, nullptr, nullptr, napi_default, nullptr },
        // logger
        {"LOGI",                        nullptr, logger::logiNAPI,                  nullptr, nullptr, nullptr, napi_default, nullptr },
        {"LOGW",                        nullptr, logger::logwNAPI,                  nullptr, nullptr, nullptr, napi_default, nullptr },
        {"LOGE",                        nullptr, logger::logeNAPI,                  nullptr, nullptr, nullptr, napi_default, nullptr },
        {"regXComponentCallback",       nullptr, RegXComponentCallbackNAPI,         nullptr, nullptr, nullptr, napi_default, nullptr },
        // 诊断工具
        {"dumpWindowLayout",            nullptr, DumpWindowLayout,                  nullptr, nullptr, nullptr, napi_default, nullptr },

    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);

    // XComponent(libraryname:'hgs') 触发的加载: exports 携带原生对象。
    // 注意: 本注册函数每个**进程**只跑一次 (JS 首次 import libhgs.so);
    // 多窗口 (AppRunnerAbility multiton 同进程/异进程) 时后续 XComponent
    // 靠 ArkUI 的 napi 对象创建回调再次触发本函数 —— 无论哪种路径都要有日志,
    // 排查"窗口进程回调没注册"时先看这行出没出现。
    OH_LOG_INFO(LOG_APP, "[HGS-NAPI] module register func enter");
    // 记住 env/exports; 此刻 XComponent 多半还没创建 (unwrap 会失败),
    // 真正的注册在 HgsSurface.onLoad → regXComponentCallback/bindXComponent 重试
    gXcEnv = env;
    napi_create_reference(env, exports, 0, &gXcExportsRef);
    RetryXComponentRegister();
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
