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
        // 输入相关
        {"sendPointerEvent",        nullptr, hgs::im::SendPointerEventNAPI,         nullptr, nullptr, nullptr, napi_default, nullptr },
        {"sendKeyEvent",            nullptr, hgs::im::SendKeyEventNAPI,             nullptr, nullptr, nullptr, napi_default, nullptr },
        {"sendScrollEvent",         nullptr, hgs::im::SendScrollEventNAPI,          nullptr, nullptr, nullptr, napi_default, nullptr },
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
        // 诊断工具
        {"dumpWindowLayout",            nullptr, DumpWindowLayout,                  nullptr, nullptr, nullptr, napi_default, nullptr },

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
