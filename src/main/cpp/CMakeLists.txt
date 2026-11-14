cmake_minimum_required(VERSION 3.5.0)
project(hgs)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(NATIVERENDER_ROOT_PATH ${CMAKE_CURRENT_SOURCE_DIR})

include_directories(
    ${NATIVERENDER_ROOT_PATH}
    # 公共契约头 (api/): 裸名 include (hgs_events.h 等), 零平台依赖 —
    # host_tests 经 HOST_TEST_INC 直连编译 (与 compositor 域同款约定)
    ${NATIVERENDER_ROOT_PATH}/api
    ${NATIVERENDER_ROOT_PATH}/protocols
    # 合成器按域分组 (frame/toplevel/input) + 协议 C 头 (protocols/): 域内
    # 互引保持裸名 include (与 entry 同款约定)。seam/ 接缝头经根路径解析。
    ${NATIVERENDER_ROOT_PATH}/compositor/frame
    ${NATIVERENDER_ROOT_PATH}/compositor/toplevel
    ${NATIVERENDER_ROOT_PATH}/compositor/input
)

if(DEFINED PACKAGE_FIND_FILE)
    include(${PACKAGE_FIND_FILE})
endif()

# 预编译 wayland-server
option(HGS_BUILD_X11 "Build the optional OHOS Xwayland backend" OFF)
set(HGS_X11_DEPS_ROOT "" CACHE PATH "OHOS wlroots and Xwayland SDK prefix")
set(HGS_X11_DEPS_BY_ABI "" CACHE PATH "Build root with <OHOS_ARCH>/usr Xwayland SDKs")
if(HGS_X11_DEPS_BY_ABI)
    set(HGS_X11_DEPS_ROOT "${HGS_X11_DEPS_BY_ABI}/${OHOS_ARCH}/usr")
endif()
option(HGS_BUILD_XIM "Build the optional X11 input method server" OFF)
set(HGS_XIM_DEPS_ROOT "" CACHE PATH "ARM64 OHOS xcb-imdkit SDK prefix")
if(HGS_BUILD_XIM AND NOT HGS_BUILD_X11)
    message(FATAL_ERROR "HGS_BUILD_XIM requires HGS_BUILD_X11")
endif()
set(WAYLAND_LIBS_DIR ${NATIVERENDER_ROOT_PATH}/../../../libs/${OHOS_ARCH})
if(HGS_BUILD_X11)
    set(WAYLAND_LIBS_DIR "${HGS_X11_DEPS_ROOT}/lib")
endif()

add_library(hgs SHARED
    napi_init.cpp

    # ── 公共 API 门面 (阶段 0) ──
    # api/*.h 零平台依赖; 实现文件是唯一允许依赖平台/现有单例的位置
    core/window_registry.cpp
    core/event_bus.cpp
    core/input_router.cpp
    bindings/napi/service_napi.cpp
    runtime/service_runtime.cpp
    runtime/service_instance.cpp
    backends/wayland/legacy_wayland_backend.cpp
    platform/unix/socket_listener.cpp
    api/hgs_endpoint.cpp
    api/hgs_instance.cpp
    api/hgs_window_policy.cpp
    api/hgs_window_manager.cpp

    # ── 合成器域 ──
    compositor/wayland_server.cpp
    compositor/wl_core.cpp
    compositor/data_device_stub.cpp
    compositor/frame/compositor_blit.cpp
    compositor/toplevel/toplevel_manager.cpp
    compositor/toplevel/popup_manager.cpp
    compositor/toplevel/move_grab.cpp
    compositor/frame/compositor_utils.cpp
    compositor/frame/geometry.cpp
    compositor/toplevel/desktop_compositor.cpp
    compositor/frame/zc_bridge.cpp
    compositor/frame/frame_composer.cpp
    compositor/frame/frame_pipeline.cpp
    compositor/frame/shm_frame_source.cpp
    compositor/input/input_resolver.cpp
    compositor/input/input_space_mapper.cpp
    compositor/input/input_queue.cpp
    compositor/input/input_state_tracker.cpp
    compositor/input/input_injector.cpp
    compositor/toplevel/desktop_root_manager.cpp
    compositor/toplevel/toplevel_event_bus.cpp
    compositor/xdg_shell.cpp
    compositor/winehua_toplevel.cpp
    seam/zero_copy.cpp
    seam/host_hooks.cpp
    seam/session_control.cpp
    seam/symbol_resolver.cpp

    # ── 真实 EGL 渲染器 + 输入 ──
    graphics/egl_surface.cpp
    graphics/egl_renderer.cpp
    graphics/shader_utils.cpp
    bridge/plugin_manager.cpp
    bridge/napi_host_adapter.cpp  # P1.1: NAPI 层 IHgsHost 适配 (set*Callback TSFN)
    bridge/window_napi.cpp        # W2a: 窗口管理器 NAPI (setWindowCallback/notify/query)
    common/perf_utils.cpp
    logger/logger.cpp
    input/seat.cpp
    input/input_manager.cpp
    input/native_input.cpp       # N1: OH_NativeXComponent 输入回调
    input/native_keymap.cpp      # N1: OHOS KeyCode → evdev (KeyMap.ets 移植)
    input/pointer_extras.cpp
    input/text_input.cpp
    input/game_controller_bridge.cpp
    input/controller/controller_hub.cpp
    input/controller/controller_napi.cpp
    input/controller/controller_runtime.cpp
    input/controller/gamepad_bridge.cpp
    input/controller/physical_gamepad.cpp

    protocols/xdg-shell-protocol.c
    protocols/winehua-toplevel-protocol.c
    protocols/viewporter-server-protocol.c
    protocols/pointer-warp-v1-protocol.c
    protocols/pointer-constraints-unstable-v1-protocol.c
    protocols/relative-pointer-unstable-v1-protocol.c
    protocols/text-input-unstable-v3-protocol.c
)

target_include_directories(hgs PUBLIC ${NATIVERENDER_ROOT_PATH}/include)

# BlitScaled (双线性缩放 blit) 是桌面合成热路径 — -O2 必须跟着源文件走
set_source_files_properties(compositor/frame/compositor_blit.cpp PROPERTIES COMPILE_OPTIONS "-O2")

target_link_libraries(hgs PUBLIC
    libace_napi.z.so
    libace_ndk.z.so
    libhilog_ndk.z.so
    libnative_window.so
    libnative_image.so
    libnative_vsync.so
    libnative_window_manager.so
    libEGL.so
    libGLESv3.so
    ${WAYLAND_LIBS_DIR}/libwayland-server.so.0
)

# smoke 测试设施的原生侧 (自 entry 迁入): 查询/注入原语, 产品代码不引用。
# 符号来自 libhgs.so (同进程运行时解析), 与 entry 同包加载。
add_library(smoke_napi SHARED
    smoke/smoke_napi.cpp
)
target_link_libraries(smoke_napi PRIVATE
    hgs
    libace_napi.z.so
    libhilog_ndk.z.so
)

if(HGS_BUILD_X11)
    set(HGS_X11_LIB "${HGS_X11_DEPS_ROOT}/lib")
    set(HGS_X11_INCLUDE "${HGS_X11_DEPS_ROOT}/include")
    if(NOT EXISTS "${HGS_X11_LIB}/libwlroots-0.20.a" OR
       NOT EXISTS "${HGS_X11_INCLUDE}/wlroots-0.20/wlr/xwayland/xwayland.h" OR
       NOT EXISTS "${HGS_X11_LIB}/libxwayland_ohos.so")
        message(FATAL_ERROR "HGS_BUILD_X11 requires a staged OHOS wlroots 0.20.2 and Xwayland SDK")
    endif()
    add_library(hgs_xwayland_session OBJECT
        backends/xwayland/runtime/session.c
        platform/ohos/presentation/cpu_surface.c
    )
    set_target_properties(hgs_xwayland_session PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_compile_definitions(hgs_xwayland_session PRIVATE WLR_USE_UNSTABLE _GNU_SOURCE)
    target_sources(hgs PRIVATE
        backends/xwayland/xwayland_backend.cpp
        backends/xwayland/external_frame_bridge.cpp
        graphics/external_texture_surface.cpp
        $<TARGET_OBJECTS:hgs_xwayland_session>
        backends/xwayland/runtime/x11_endpoint.cpp
        platform/ohos/process/xwayland_process.cpp
    )
    target_compile_definitions(hgs PRIVATE HGS_BUILD_X11)
    target_include_directories(hgs_xwayland_session BEFORE PRIVATE
        "${HGS_X11_INCLUDE}"
        "${HGS_X11_INCLUDE}/wlroots-0.20"
        "${HGS_X11_INCLUDE}/pixman-1"
    )
    target_link_libraries(hgs PRIVATE
        "${HGS_X11_LIB}/libwlroots-0.20.a"
        "${HGS_X11_LIB}/libwayland-client.so.0"
        "${HGS_X11_LIB}/libxkbcommon.so.0"
        "${HGS_X11_LIB}/libpixman-1.so.0"
        "${HGS_X11_LIB}/libdrm.so.2"
        "${HGS_X11_LIB}/libxcb.so.1"
        "${HGS_X11_LIB}/libxcb-composite.so.0"
        "${HGS_X11_LIB}/libxcb-ewmh.so.2"
        "${HGS_X11_LIB}/libxcb-icccm.so.4"
        "${HGS_X11_LIB}/libxcb-render.so.0"
        "${HGS_X11_LIB}/libxcb-res.so.0"
        "${HGS_X11_LIB}/libxcb-xfixes.so.0"
        libnative_buffer.so
        libchild_process.so
    )
    add_library(hgs_xwayland_child SHARED platform/ohos/process/xwayland_child.cpp)
    target_link_libraries(hgs_xwayland_child PRIVATE libhilog_ndk.z.so)
    if(HGS_BUILD_XIM)
        if(NOT EXISTS "${HGS_XIM_DEPS_ROOT}/include/xcb-imdkit/imdkit.h" OR
           NOT EXISTS "${HGS_XIM_DEPS_ROOT}/lib/libxcb-imdkit.so.1")
            message(FATAL_ERROR "HGS_BUILD_XIM requires staged OHOS xcb-imdkit headers and library")
        endif()
        target_compile_definitions(hgs PRIVATE HGS_BUILD_XIM)
        target_compile_definitions(hgs_xwayland_session PRIVATE HGS_BUILD_XIM)
        target_sources(hgs PRIVATE platform/ohos/process/xim_process.cpp)
        add_library(hgs_xim_child SHARED platform/ohos/process/xim_child.cpp)
        target_include_directories(hgs_xim_child PRIVATE "${HGS_XIM_DEPS_ROOT}/include" "${HGS_X11_INCLUDE}")
        target_link_libraries(hgs_xim_child PRIVATE
            "${HGS_XIM_DEPS_ROOT}/lib/libxcb-imdkit.so.1"
            "${HGS_X11_LIB}/libxcb.so.1"
        )
    endif()
endif()
