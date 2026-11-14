# HGS - HarmonyOS Graphics UserInterface Service

HGS 提供可由宿主接入的图形窗口服务。API 1.3 可选择启动 Wayland、X11 或双协议监听，并提供默认 ArkTS 窗口宿主。

- [技术架构](docs/architecture/overview.md)：从协议监听、客户端建窗到 OHOS Surface 的数据和输入路径。
- [目录结构](docs/architecture/repository-layout.md)：按模块和常见修改任务定位源码。
- [新项目接入](docs/integration/new-project.md)：添加源码 HAR，注册窗口页面，启动客户端并验证窗口。
- [公共服务 API](docs/api/service-api.md)和 [X11 后端构建](docs/integration/x11.md)：接口契约、可选依赖和资源打包。

主机验证使用 `make -C tests test`。HAR 根入口是 Index.ets，其中导出服务、会话、窗口宿主及对应的窗口组件。
