# HGS - HarmonyOS Graphics UserInterface Service

HGS 提供可由宿主接入的图形窗口服务，当前包含现有 Wayland compositor、默认 ArkTS 窗口 host，以及 API 1.1 的公共服务边界。

[公共服务 API 与当前能力](docs/api/service-api.md) 说明 start/stop、真实端点、客户端环境、窗口字符串 ID、事件和输入契约。当前实际可启动 Wayland；X11/both 在后端迁入前明确报 unsupported-protocol。

主机验证：`make -C tests test`。HAR 根入口保持 Index.ets，原有 HgsSurface、HgsWindowPage、HgsWindowHostService、HgsAppWindowAbility 继续导出，新增 HgsService、HgsSession、HgsStartError 及其类型。
