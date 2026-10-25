cmake_minimum_required(VERSION 3.5.0)
project(hgs)

set(NATIVERENDER_ROOT_PATH ${CMAKE_CURRENT_SOURCE_DIR})

include_directories(${NATIVERENDER_ROOT_PATH})

add_library(hgs SHARED
    napi_init.cpp
    render/egl_renderer.cpp
)

target_link_libraries(hgs PUBLIC
    libace_napi.z.so
    libhilog_ndk.z.so
    libnative_window.so
    libEGL.so
    libGLESv3.so
)
