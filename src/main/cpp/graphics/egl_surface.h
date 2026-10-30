// hgs/src/main/cpp/graphics/egl_surface.h
//
// Native 侧 Surface 生命周期 (占位, 未来 useNativeSurfaceCallback 打开时
// 由本文件实现接管: XComponent libraryname 触发 NativeXComponent 回调,
// 生命周期与渲染在 native 层驱动)。
#pragma once

#include <ace/xcomponent/native_interface_xcomponent.h>
#include <hilog/log.h>
#include <napi/native_api.h>

#include <mutex>
#include <string>
#include <unordered_map>

#include "smoke/egl_renderer.h"

namespace hgs {
namespace surface {

// 以 OH_NativeXComponent* 为键的渲染器注册表 (每个 XComponent 实例一个)
extern std::mutex gMutex;
extern std::unordered_map<OH_NativeXComponent *, hgs::EmptyEglRenderer *> gRenderers;

void OnSurfaceCreated(OH_NativeXComponent *comp, void *window);

void OnSurfaceChanged(OH_NativeXComponent *comp, void *window);

void OnSurfaceDestroyed(OH_NativeXComponent *comp, void *window);

extern OH_NativeXComponent_Callback gXcompCb;

// NAPI: bindXComponent(xcompObject) — 非 Init 触发的 XComponent (同进程
// 第二个窗口: 模块已加载, Init 不会再次执行) 在 onLoad 里显式补挂回调。
// 参照 blender 的 bindSubXComponent。
napi_value BindXComponent(napi_env env, napi_callback_info info);

} // namespace surface
} // namespace hgs
