# 公共图形服务 API 1.1

这一版建立统一服务边界并接回现有 Wayland 后端。`getApiVersion()` 返回 `0x10001`；旧 NAPI、数值枚举和默认窗口 host 保留。X11 后端将在 P2 迁入，当前请求 `x11` 或 `both` 返回 `unsupported-protocol`，不会悄悄改成 Wayland。

## ArkTS 使用方式

```ts
import { HgsService, HgsStartError } from 'hgs';

const service = HgsService.create();
const session = await service.start({
  protocols: 'wayland',
  runtimeDir: `${context.filesDir}/hgs`,
  wayland: { socketName: 'wayland-hgs-0' },
  composition: 'per-window',
  inputBackend: 'native',
  setProcessEnvironment: false
});
const environment = session.clientEnvironment('wayland');
// 宿主为子进程合并 environment.set，并移除 environment.unset 中的变量。
// 当前 Wayland 无 inheritedFds；服务自身不启动 Wine、shell 或其他客户端。

await service.stop();
```

宿主需准备 runtimeDir 的父目录。创建目录、监听 socket 和注册逻辑输出由 start 完成，不需要先有 NativeWindow、客户端或首帧。资源准备、X11 display 自动分配、XIM 及其就绪等待留给 P2，不把这些尚未实现的选项放进接口。

默认窗口 host 仍使用 `HgsWindowHostService.init(context, options)` 订阅旧窗口管理器。它不是 HgsService 的强制依赖。使用新 start 时无需再调用默认 host 的 start；若调用新旧两个入口，必须是相同配置，否则收到冲突。宿主原生 host 与默认 ArkTS host 仍不能同时创建同一个窗口。

`composition: 'preserve'` 或缺省值保留宿主预设；`per-window`、`desktop-root` 分别显式设置独立窗口和桌面画布。output 的 width/height 缺省为 0，不覆盖宿主已经计算的尺寸；显式尺寸需同时大于 0。inputBackend 支持 auto/arkts/native。

## 生命周期与错误

- 一个进程只有一个 Runtime。`HgsService.create()` 返回同一门面，v1 startServer/stopServer 和旧 SessionControl.Stop 接入相同生命周期。
- 同配置重复 start 返回原会话、原 epoch；运行中改变配置返回 `config-conflict`，原端点继续服务。
- 请求未编入协议会在启动后端前失败；已经启动的多后端若有后续失败，按逆序停止并撤销端点。双后端回滚目前通过独立假后端测试，不代表真实 X11 已迁入。
- stop 等待后端线程退出，销毁客户端，清理自身端点、窗口及输入归属。没有窗口或最后一个客户端断连本身不会停止服务。
- `HgsStartError` 保留 code/stage；原生结果额外包含错误 message 和服务快照。运行期查询没有可用端点时报 not-ready；旧 session 的环境、窗口及输入操作在原生锁内校验 epoch。
- ArkTS start/stop 返回 Promise。P1 的 Wayland 注册过程在 NAPI 调用中同步完成；还未提供异步启动超时或任意线程的取消能力。P2 的 NCP 就绪编排需要继续完善这一点。

旧 startServer 默认仍写 XDG_RUNTIME_DIR/WAYLAND_DISPLAY。新 start 默认不写。两种方式均通过明确的绝对路径创建监听 fd，不再依赖进程中既有的 XDG_RUNTIME_DIR。客户端环境根据实际就绪端点返回；Wayland 返回 XDG_RUNTIME_DIR、WAYLAND_DISPLAY，并要求移除 DISPLAY 和继承的 WAYLAND_SOCKET。

## 窗口、事件与输入

session.windows() 返回拥有自身数据的快照。公共 id 是不透明字符串，不能转为 number。epoch、clientId、nativeId、objectGeneration、surfaceGeneration 也使用字符串，避免 64 位整数精度丢失。legacyId 仅用于接回旧 surface/default-host 接口。

窗口身份包含 epoch、backendId、protocol、连接身份、原生对象和对象代次。连接身份不是 PID；同一进程的不同 Wayland 连接可以有相同协议对象编号。原生对象复用和服务重启均产生新的公共 id，旧 id 不能命中新窗口。

onEvent 返回订阅 token，offEvent 退订。事件包含拥有自身存储的 window/detail、epoch、serial、origin。serial 在 Runtime 内串行分配和发布。C++ 及 ArkTS 的订阅者异常会被隔离，不阻止其余订阅者。

C++ 订阅在后端线程或控制线程回调，不得在回调内同步调用 start/stop；应投递到控制线程。NAPI 转为 TSFN 交给 ArkTS。已经排队的 JS 事件可能在 stop 返回后到达，宿主 UI 应依据 epoch 和所属窗口 id 丢弃迟到操作；stop 不等待任意宿主 UI 工作完成。

sendInput 使用明确的公共 windowId。键和按钮的释放跟随对应按下的窗口，未知/过期目标不回退到另一个后端。pointer 的 x/y 是内容物理像素，拖动捕获期间调用方仍应提供捕获窗口坐标；rawDelta 是相对位移，scroll 的轴为 0/1。跨后端焦点仲裁和坐标变换由 P4 实施，capabilities.crossBackendFocus 当前为 false。

## 当前能力边界

- LegacyWaylandBackend 复用原有协议、渲染器和输入；没有整体搬动旧 compositor。
- 新 registry 桥接旧 toplevel 生命周期和状态。旧 popup 通知仍带在父窗口事件中，capabilities.popupIdentity=false；独立 popup 身份、parent/transient 完整关系与默认 host 的新 ID 接入尚未迁移。
- surface 绑定仍通过旧兼容接口；新快照 surfaceGeneration=0 表示这层桥接尚未提供绑定代次，不代表客户端一定没有 surface。
- 旧 FeatureConfig 只为 v1 配置一致性保留，协议注册行为没有在本阶段重写；新 ArkTS API 没有宣称可切换这些协议 global。
- X11 的 DISPLAY/XAUTHORITY/XMODIFIERS、fd 和资源由 P2 后端提供并验收，不能把 mock 后端的环境测试当成真实 X11 能力。

## C++ 与测试

公共头位于 `src/main/cpp/include/hgs/`；旧 api/ 类型通过 forwarding header 保留。实现按 core/、runtime/、backends/wayland/、bindings/napi/ 和 platform/unix/ 分组，公共头不引入 NAPI、OHOS、Wayland 或 EGL 类型。

`hgs::service::DefaultRuntime()` 返回与旧入口共享的 Runtime。Config 的 output/composition/inputBackend/setProcessEnvironment 属于服务层；legacyWaylandFeatures 只用于旧配置兼容。

```bash
make -C tests test
```

该命令使用本机 C++17 和 libwayland-server.so.0，实际创建本地 Unix socket。受限沙箱需允许本地监听后才能运行第二个测试。测试覆盖连接/对象复用、旧 epoch、结构化失败、幂等与冲突、失败回滚、输入配对、事件所有权，以及真实 wl_display 在缺失/错误进程环境下监听、端点冲突、迟到清理和重启。
