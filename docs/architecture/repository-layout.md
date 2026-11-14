# HGS 目录结构

路径均相对于 HGS 仓库根目录。HGS 的 ArkTS 与 C/C++ 源码同属一个 HAR，由 Hvigor 构建，原生目标由 CMake 配置。

```text
hgs/
├── Index.ets                    # HAR 对外 ArkTS 导出
├── oh-package.json5             # 包名 hgs，声明 libhgs.so 的 ArkTS 类型包
├── build-profile.json5          # HAR 构建配置、CMake 参数和 ABI
├── hvigorfile.ts
├── docs/                        # 架构、公共 API、接入和后端构建
├── src/main/
│   ├── module.json5             # HAR 模块声明
│   ├── ets/
│   │   ├── api/                 # HgsService、HgsSession、HgsStartError
│   │   ├── service/             # Wayland 和 X11 的默认窗口承载
│   │   ├── entryability/        # Wayland 单窗口 Ability 基类
│   │   ├── components/          # XComponent、Surface 绑定和窗口页面
│   │   └── common/              # ArkTS 输入和按键映射
│   └── cpp/
│       ├── CMakeLists.txt       # libhgs.so、smoke NAPI 和可选的 X11/XIM 子进程库
│       ├── include/hgs/         # 公共 C++ 服务、数据类型、外部帧接缝
│       ├── bindings/napi/       # 公共服务 NAPI、参数转换、异步任务与事件
│       ├── types/libhgs/        # libhgs.so 的 ArkTS 声明
│       ├── core/                # 窗口注册、事件总线、输入路由
│       ├── runtime/             # 默认实例、后端选择、端点与生命周期
│       ├── backends/
│       │   ├── wayland/          # 原 Wayland 合成器到公共服务的适配
│       │   └── xwayland/         # X11 后端、端点、wlroots 会话和外部帧桥
│       ├── platform/
│       │   ├── unix/             # Unix socket 创建及清理
│       │   └── ohos/             # NCP 启动、Surface 与 CPU 像素提交
│       ├── api/                 # 既有原生接口、事件及窗口策略
│       ├── bridge/              # XComponent 插件与旧窗口 NAPI 适配
│       ├── compositor/          # Wayland 协议实现、帧合成和窗口关系
│       ├── graphics/            # EGL、着色器、GPU 外部纹理呈现
│       ├── input/               # Wayland seat、IME、原生输入与手柄
│       ├── protocols/           # 协议 XML、生成的头文件与胶水代码
│       ├── seam/                # 宿主钩子、会话控制、零拷贝适配
│       ├── common/              # 原生通用工具与显示诊断
│       ├── logger/              # 日志桥
│       ├── smoke/               # 测试专用 NAPI
│       └── napi_init.cpp        # libhgs.so 模块注册及既有接口导出
├── scripts/
│   ├── build_all.sh             # 交叉构建基础运行库
│   ├── x11/                     # XIM 构建、X11 资源和运行库闭包
│   ├── optional/                # GTK 等可选依赖
│   └── smoke/                   # 构建 Wayland / GTK 验证客户端
├── tests/
│   ├── unit/                    # 公共 Runtime 和 Unix 监听测试
│   └── integration/             # Wayland、GTK、Xlib 与最小宿主
└── thirdparty/                  # 依赖源码工作区；两个 smoke 客户端源码纳入 Git
```

## 按问题找入口

| 要查看或修改 | 从这里开始 |
| --- | --- |
| ArkTS 对外方法、类型或导出 | [Index.ets](../../Index.ets)、[HgsService.ets](../../src/main/ets/api/HgsService.ets)、[libhgs 声明](../../src/main/cpp/types/libhgs/Index.d.ts)。改动跨 NAPI 时对照 [service_napi.cpp](../../src/main/cpp/bindings/napi/service_napi.cpp)。 |
| start/stop、端点就绪、失败回滚 | [service_runtime.cpp](../../src/main/cpp/runtime/service_runtime.cpp)、[service_instance.cpp](../../src/main/cpp/runtime/service_instance.cpp) 和 [公共 C++ 接口](../../src/main/cpp/include/hgs/service.h)。 |
| 公共窗口 ID、事件序号和输入配对 | [window_registry.cpp](../../src/main/cpp/core/window_registry.cpp)、[event_bus.cpp](../../src/main/cpp/core/event_bus.cpp)、[input_router.cpp](../../src/main/cpp/core/input_router.cpp)。 |
| Wayland 握手、客户端建窗或画面问题 | [legacy_wayland_backend.cpp](../../src/main/cpp/backends/wayland/legacy_wayland_backend.cpp)、[wayland_server.cpp](../../src/main/cpp/compositor/wayland_server.cpp)、[wl_core.cpp](../../src/main/cpp/compositor/wl_core.cpp)、[egl_renderer.cpp](../../src/main/cpp/graphics/egl_renderer.cpp)。 |
| X11 连接、认证、XWM 或呈现 | [xwayland_backend.cpp](../../src/main/cpp/backends/xwayland/xwayland_backend.cpp)、[x11_endpoint.cpp](../../src/main/cpp/backends/xwayland/runtime/x11_endpoint.cpp)、[session.c](../../src/main/cpp/backends/xwayland/runtime/session.c)。跨进程启动见 [xwayland_process.cpp](../../src/main/cpp/platform/ohos/process/xwayland_process.cpp)。 |
| OHOS 窗口外观、几何或 Surface 绑定 | [HgsWindowHostService.ets](../../src/main/ets/service/HgsWindowHostService.ets) 是默认入口。Wayland 看 [HgsAppWindowAbility.ets](../../src/main/ets/entryability/HgsAppWindowAbility.ets) 与 [HgsSurface.ets](../../src/main/ets/components/HgsSurface.ets)，X11 看 [HgsServiceWindowHost.ets](../../src/main/ets/service/HgsServiceWindowHost.ets) 与 [HgsWindowSurface.ets](../../src/main/ets/components/HgsWindowSurface.ets)。 |
| 新增协议后端、构建依赖或 HAP 运行库 | [backend.h](../../src/main/cpp/backends/backend.h)、[原生 CMake](../../src/main/cpp/CMakeLists.txt)、[HAR 构建配置](../../build-profile.json5) 和 [X11 构建说明](../integration/x11.md)。 |

`libs/<ABI>/` 是 HAR 要打包的预编译依赖；`build/sysroot-ext/` 是交叉构建使用的头文件、库和 pkg-config 工作区。它们由脚本生成。独立使用时，脚本可在 `thirdparty/` 准备依赖；嵌入宿主时也可复用宿主的源码或通过环境变量指定路径。通过 `make -C tests test` 验证 Runtime 与监听逻辑，实际 OHOS 窗口及客户端回归见[集成测试说明](../../tests/integration/README.md)。
