# 公共图形服务 API 1.3

这一版提供窗口控制、XIM 与外部帧接入。getApiVersion() 返回 0x10003。旧 NAPI、数值枚举和默认窗口宿主入口继续可用。启用 X11 需按文档准备 ARM64 SDK 和键盘资源。[X11 构建和接入](../integration/x11.md)

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

宿主需准备 runtimeDir 的父目录。创建目录、监听 socket 和注册逻辑输出由 start 完成，不需要先有 NativeWindow、客户端或首帧。X11 模式显式传入资源路径，服务自动分配 display 和认证文件。可为 X11 会话配置 [XIM](../integration/x11.md#xim-输入法)。

### 默认窗口宿主

在 EntryAbility 中调用 `HgsWindowHostService.init(this.context)`，获得 WindowStage 后调用 `HgsWindowHostService.startService(stage, options)`。options 接受 `protocols: 'wayland' | 'x11' | 'both'`，默认使用独立窗口、原生输入，返回同一套 HgsSession 和客户端环境。客户端退出后监听继续，宿主通过 `HgsWindowHostService.stopService()` 显式结束会话。

- 使用 Wayland 时，宿主在 module.json5 注册继承 HAR `HgsAppWindowAbility` 的 multiton Ability，在页面列表注册调用 `HgsWindowPage` 的包装页。包装页使用 `@Entry({ useSharedStorage: true })` 接收该实例的窗口 ID。默认名字为 `HgsAppWindowAbility` 与 `pages/HgsWindowPage`，可在 init 时指定。
- 使用 X11 时，宿主注册调用 `HgsServiceWindowPage` 的包装页，默认是 `pages/HgsServiceWindow`，可通过 `servicePageUrl` 修改。入口用 `@Entry({ useSharedStorage: true })` 接收宿主传入的窗口 ID 和 epoch。

```ts
import { HgsServiceWindowPage } from 'hgs';

@Entry({ useSharedStorage: true })
@Component
struct X11WindowPage {
  build() {
    HgsServiceWindowPage()
  }
}
```

旧的 `HgsWindowHostService.start(...)` 仍提供 Wayland 兼容入口。自管窗口或无窗口宿主可直接使用 `HgsService.start(...)`。

`HgsWindowHostService.setWaylandWindowScale(scale)` 更新之后创建的 Wayland 窗口的逻辑像素到 OHOS 窗口像素的比例。每个窗口在创建时记录自己的比例，后续调整几何时沿用该值。

`composition: 'preserve'` 或缺省值保留宿主预设；`per-window`、`desktop-root` 分别显式设置独立窗口和桌面画布。output 的 width/height 缺省为 0，不覆盖宿主已经计算的尺寸；显式尺寸需同时大于 0。inputBackend 支持 auto/arkts/native。

## 生命周期与错误

- 一个进程只有一个 Runtime。`HgsService.create()` 返回同一门面，v1 startServer/stopServer 和旧 SessionControl.Stop 接入相同生命周期。
- 同配置重复 start 返回原会话、原 epoch；运行中改变配置返回 `config-conflict`，原端点继续服务。
- 请求未编入协议会在启动后端前失败；已经启动的多后端若有后续失败，按逆序停止并撤销端点。主机假后端测试覆盖双后端回滚，最小宿主提供对应的设备用例。
- stop 等待后端线程退出，销毁客户端，清理自身端点、窗口及输入归属。没有窗口或最后一个客户端断连本身不会停止服务。
- `HgsStartError` 保留 code/stage；原生结果额外包含错误 message 和服务快照。运行期查询没有可用端点时报 not-ready；旧 session 的环境、窗口及输入操作在原生锁内校验 epoch。
- ArkTS start/stop 通过异步 NAPI 任务依序执行。X11 配置包含 XWM 就绪等待时限。停止排在已提交的启动任务之后。

旧 startServer 默认仍写 XDG_RUNTIME_DIR/WAYLAND_DISPLAY。新 start 默认不写。两种方式均通过明确的绝对路径创建监听 fd，不再依赖进程中既有的 XDG_RUNTIME_DIR。客户端环境根据实际就绪端点返回；Wayland 返回 XDG_RUNTIME_DIR、WAYLAND_DISPLAY，并要求移除 DISPLAY 和继承的 WAYLAND_SOCKET。

## 窗口、事件与输入

session.windows() 返回拥有自身数据的快照。公共 id 是不透明字符串，不能转为 number。epoch、clientId、nativeId、objectGeneration、surfaceGeneration 也使用字符串，避免 64 位整数精度丢失。legacyId 仅用于接回旧 surface/default-host 接口。

窗口身份包含 epoch、backendId、protocol、连接身份、原生对象和对象代次。同一进程的不同 Wayland 连接可以有相同协议对象编号。X11 按资源 ID 槽区分有存活窗口对象的客户端代次。原生对象复用和服务重启均产生新的公共 id，旧 id 不能命中新窗口。

onEvent 返回订阅 token，offEvent 退订。事件包含拥有自身存储的 window/detail、epoch、serial、origin。serial 在 Runtime 内串行分配和发布。C++ 及 ArkTS 的订阅者异常会被隔离，不阻止其余订阅者。

C++ 订阅在后端线程或控制线程回调，不得在回调内同步调用 start/stop；应投递到控制线程。NAPI 转为 TSFN 交给 ArkTS。已经排队的 JS 事件可能在 stop 返回后到达，宿主 UI 应依据 epoch 和所属窗口 id 丢弃迟到操作；stop 不等待任意宿主 UI 工作完成。

sendInput 使用明确的公共 windowId。键和按钮的释放跟随对应按下的窗口，未知或过期目标会被拒绝。pointer 默认使用窗口内容坐标，X11 可启用 normalizedPosition 按实际 surface 换算。rawDelta 是相对位移，scroll 的轴为 0/1。

`session.controlWindow(id, control)` 接受 focus、blur、resize 与 close。窗口宿主根据 OHOS 焦点变化更新协议焦点。切换目标时，服务先向前一个后端投递 blur，并清除其按键和按钮归属。resize 的宽高使用逻辑像素。close 请求发送给客户端，窗口销毁由后续协议事件确认。返回值表示命令已入队。

## 当前能力边界

- LegacyWaylandBackend 复用原有协议、渲染器和输入；没有整体搬动旧 compositor。
- Wayland popup 继续由原宿主按父窗口事件呈现，`capabilities.popupIdentity` 为 false。X11 popup 有独立 ID，`parentId` 按已观察到的 X11 transient 关系或同连接的菜单归属解析。
- X11 使用 `session.bindSurface`，快照中的 surfaceGeneration 标识当前绑定，framePresented 表示当前绑定已提交首帧。Wayland 继续通过旧 surface 接口绑定。
- Wayland 的 `wl_data_device_manager` 提供 GTK 初始化 seat 所需的最小协议实现。应用间数据传输和跨协议剪贴板桥接尚未实现。

## C++ 与测试

公共头位于 `src/main/cpp/include/hgs/`；旧 api/ 类型通过 forwarding header 保留。实现按 core/、runtime/、backends/wayland/、bindings/napi/ 和 platform/unix/ 分组，公共头不引入 NAPI、OHOS、Wayland 或 EGL 类型。

`hgs::service::DefaultRuntime()` 返回与旧入口共享的 Runtime。Config 的 output/composition/inputBackend/setProcessEnvironment 属于服务层；legacyWaylandFeatures 只用于旧配置兼容。

```bash
make -C tests test
```

该命令使用本机 C++17 和 libwayland-server.so.0，实际创建本地 Unix socket。受限沙箱需允许本地监听后才能运行第二个测试。测试覆盖连接/对象复用、旧 epoch、结构化失败、幂等与冲突、失败回滚、输入配对、事件所有权，以及真实 wl_display 在缺失/错误进程环境下监听、端点冲突、迟到清理和重启。
