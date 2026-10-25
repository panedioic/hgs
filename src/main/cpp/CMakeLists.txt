cmake_minimum_required(VERSION 3.5.0)
project(hgs)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(NATIVERENDER_ROOT_PATH ${CMAKE_CURRENT_SOURCE_DIR})

include_directories(
    ${NATIVERENDER_ROOT_PATH}
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
set(WAYLAND_LIBS_DIR ${NATIVERENDER_ROOT_PATH}/../../../libs/${OHOS_ARCH})

add_library(hgs SHARED
    napi_init.cpp
    render/egl_renderer.cpp

    # ── 合成器域 ──
    compositor/wayland_server.cpp
    compositor/wl_core.cpp
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

    # ── 真实 EGL 渲染器 + 输入 ──
    graphics/egl_renderer.cpp
    graphics/shader_utils.cpp
    bridge/plugin_manager.cpp
    common/perf_utils.cpp
    input/seat.cpp
    input/input_manager.cpp
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
