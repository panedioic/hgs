# HGS - HarmonyOS Graphics UserInterface Service

HGS 提供可由宿主接入的图形窗口服务。API 1.3 可选择启动 Wayland、X11 或双协议监听，并提供默认 ArkTS 窗口宿主。

[公共服务 API 与当前能力](docs/api/service-api.md) 说明 start/stop、真实端点、客户端环境、窗口字符串 ID、事件和输入契约。[X11 后端构建和接入](docs/integration/x11.md) 记录可选 wlroots/Xwayland 构建、客户端认证和当前能力。

主机验证使用 `make -C tests test`。HAR 根入口是 Index.ets，其中导出服务、会话、窗口宿主及对应的窗口组件。
