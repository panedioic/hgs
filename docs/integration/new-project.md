# 在新项目中使用 HGS 提供窗口

本文以一个 HarmonyOS Stage 应用为例。示例将 HGS 作为源码 HAR 放在项目根目录的 `hgs/` 下，用默认宿主为客户端创建 OHOS 窗口。先接通 Wayland，再按需加入 X11。示例中的宿主模块叫 `entry`，主页面使用工程已有的 `pages/Index`。

## 1. 引入 HAR 和原生库

将 HGS 源码放到 `hgs/`，并固定所用的提交。向宿主根目录的 `build-profile.json5` 的 `modules` 数组追加模块，保留原有的 `entry` 配置：

```json5
{ "name": "hgs", "srcPath": "./hgs" }
```

向 `entry/oh-package.json5` 的 `dependencies` 添加：

```json5
"hgs": "file:../hgs"
```

按宿主 HAP 的 ABI 准备原生依赖。以 ARM64 为例，从宿主根目录执行：

```bash
OHOS_SDK=/absolute/path/to/sdk/default/openharmony NATIVE_ARCH=arm64-v8a \
  bash hgs/scripts/build_all.sh
ohpm install
```

`OHOS_SDK` 应指向包含原生 SDK 的目录。脚本需要 Meson、Ninja、pkg-config 和 CMake 等构建工具，并将要随 HAR 打包的基础库放到 `hgs/libs/arm64-v8a/`。HGS HAR 的 `build-profile.json5` 中 `abiFilters` 要与应用目标一致。该脚本准备基础库，最终由宿主的 Hvigor 构建编译 `libhgs.so` 并生成 HAP。如果应用已经打包了 Wayland 或 xkbcommon 库，应核对版本与 ABI，为每个 SONAME 保留一套兼容实现。

## 2. 注册默认 Wayland 窗口

创建 `entry/src/main/ets/hgsability/HgsAppWindowAbility.ets`：

```ts
import { HgsAppWindowAbility } from 'hgs';

export default class AppWindowAbility extends HgsAppWindowAbility {}
```

向 `entry/src/main/module.json5` 中的 `module.abilities` 追加该 Ability。按宿主模板补齐应用所需的图标、标签和启动窗资源：

```json5
{
  "name": "HgsAppWindowAbility",
  "srcEntry": "./ets/hgsability/HgsAppWindowAbility.ets",
  "launchType": "multiton",
  "exported": false
}
```

创建 `entry/src/main/ets/pages/HgsWindowPage.ets`，把 `pages/HgsWindowPage` 加到 `entry/src/main/resources/base/profile/main_pages.json` 的 `src` 数组：

```ts
import { HgsWindowPage } from 'hgs';

@Entry({ useSharedStorage: true })
@Component
struct WaylandWindowPage {
  build() {
    HgsWindowPage()
  }
}
```

`multiton` 允许各客户端顶层窗口有独立的 Ability 实例。共享 LocalStorage 让页面取得自己的窗口 ID。若应用使用其他 Ability 名或页面路径，可通过 `HgsWindowHostService.init(context, { abilityName, pageUrl })` 指定。

## 3. 等待服务就绪，再启动客户端

在宿主 EntryAbility 中接入服务。以下代码用 WindowStage 的生命周期管理监听。若应用有独立的启动、切换或退出按钮，沿用同一个会话所有者：

```ts
import { UIAbility } from '@kit.AbilityKit';
import { window } from '@kit.ArkUI';
import { HgsStartError, HgsWindowHostService } from 'hgs';

export default class EntryAbility extends UIAbility {
  async onWindowStageCreate(stage: window.WindowStage): Promise<void> {
    await stage.loadContent('pages/Index');
    HgsWindowHostService.init(this.context);

    try {
      const session = await HgsWindowHostService.startService(stage, {
        protocols: 'wayland',
        runtimeDir: this.context.filesDir + '/hgs-runtime',
        wayland: { socketName: 'wayland-hgs-0' },
        setProcessEnvironment: false
      });
      const environment = session.clientEnvironment('wayland');
      // 把 environment 交给项目自己的客户端启动器，再启动客户端。
      console.info('Wayland ready: ' + environment.set['WAYLAND_DISPLAY']);
    } catch (error) {
      if (error instanceof HgsStartError) {
        console.error('HGS ' + error.code + ' at ' + error.stage + ': ' + error.message);
      } else {
        console.error('HGS start failed: ' + String(error));
      }
    }
  }

  async onWindowStageDestroy(): Promise<void> {
    await HgsWindowHostService.stopService();
  }
}
```

`runtimeDir` 必须是绝对路径，且它的父目录应已存在并可写。这里使用应用自己的 `filesDir`。默认窗口宿主使用逐窗口合成，默认启用原生输入。

### 将环境交给启动器

调用方需要用自己的终端、原生子进程入口或 broker 启动图形客户端。对每个客户端：

1. 复制客户端所需的基础环境。
2. 删除 `environment.unset` 列出的变量，再用 `environment.set` 覆盖。
3. 若返回 `inheritedFds`，通过支持 fd 传递的启动器按契约转交。当前的 Wayland 与 X11 端点均返回空列表。
4. 在客户端进程里、初始化工具包前应用这些值。使用 OHOS 原生子进程时，可在宿主的入口参数中传递所需键值和删除列表，由子进程设置自己的环境。

| 客户端 | 使用服务返回的值 | 连接检查 |
| --- | --- | --- |
| Wayland 或 GTK 的 Wayland 后端 | `XDG_RUNTIME_DIR`、`WAYLAND_DISPLAY` | 目录和 socket 名应能在客户端所在的执行环境中解析。清除旧的 `DISPLAY` 和 `WAYLAND_SOCKET`。 |
| Xlib 或其他 X11 客户端 | `DISPLAY`、`XAUTHORITY`，启用 XIM 时还有 `XMODIFIERS` | 客户端需能连接本地 abstract Unix socket，并读取认证文件。清除旧的 Wayland 变量，以 `unset` 返回值为准。 |

示例只展示服务和窗口接线。实际启动器可参考独立测试宿主的 [ArkTS 入口](../../tests/integration/gtk/EntryAbility.ets) 和 [NCP 启动桥](../../tests/integration/x11/launcher.cpp)。先在同一应用的可达环境中验证客户端。跨应用沙箱能否连接和读取认证文件，需要按实际部署方式验证。

## 4. 加入 X11 或同时监听两种协议

1. 按 [X11 构建说明](x11.md) 准备目标 ABI 的 wlroots/Xwayland SDK 和运行库。为 HAR 的原生 CMake 配置 `-DHGS_BUILD_X11=ON -DHGS_X11_DEPS_ROOT=<SDK 前缀>`。检查 HAP 包含 `libhgs_xwayland_child.so`、`libxwayland_ohos.so` 及其运行依赖；启用 XIM 时另打包其子进程库和依赖。用 `HgsService.create().inspect().capabilities.x11` 检查实际加载的原生库是否具备 X11 能力。
2. 创建 `entry/src/main/ets/pages/HgsServiceWindow.ets`，把 `pages/HgsServiceWindow` 加到页面列表。这是 X11 的包装页。X11-only 可以只注册这一包装页。双协议需要同时注册前面的 Wayland Ability 和页面。自定义 X11 页面路径通过 `init` 的 `servicePageUrl` 设置。
3. 用 `hgs/scripts/x11/pack_resources.sh` 生成 `hgs-x11-res.zip` 并放进宿主 rawfile。启动前解压到应用可写目录，确认有 `hgs-x11/xkb/rules/evdev` 和 `hgs-x11/keymap.xkm`。解包方法可参考上面的独立宿主。

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

将启动参数改为：

```ts
// base 指向包含已解压 hgs-x11/ 的应用私有目录。
const session = await HgsWindowHostService.startService(stage, {
  protocols: 'both', // 只监听 X11 时设为 'x11'
  runtimeDir: this.context.filesDir + '/hgs-runtime',
  wayland: { socketName: 'wayland-hgs-0' },
  x11: {
    xkbRoot: base + '/hgs-x11/xkb',
    keymapPath: base + '/hgs-x11/keymap.xkm',
    startupTimeoutMs: 5000
  },
  setProcessEnvironment: false
});
```

可在 EntryAbility 中从应用设置或 Want 选择 `wayland`、`x11`、`both` 后再传给 `startService`。双协议模式下，分别查询 `session.clientEnvironment('wayland')` 和 `session.clientEnvironment('x11')`，每个客户端使用自己的环境。切换监听模式时，先等待 `stopService()`，再用新配置启动。旧客户端需重新连接，旧会话的窗口 ID 不能复用。

## 5. 验收和自管窗口

| 观察到的现象 | 先检查什么 |
| --- | --- |
| 服务启动失败 | 记录 `HgsStartError.code` 和 `stage`。查编译能力、目录权限、端点占用及 X11 键盘资源。 |
| 服务 ready，客户端连不上 | 确认客户端实际收到的环境和服务端点一致，客户端工具包选择了预期协议，且能访问 socket 和认证文件。 |
| 客户端连接了，OHOS 窗口没出现 | 查看 `HgsService.onEvent()` 和 `session.windows()`，核对 Ability 名、页面路径、共享 LocalStorage 和窗口可见状态。 |
| 窗口出现但没画面或输入 | 先核对 Surface 与窗口身份是否配对。X11 可检查 `framePresented`。再验证点击、键盘按下与释放和窗口焦点。 |
| 关闭最后一个窗口 | 再启动一个客户端，确认同一 epoch 仍可连接。显式 stop 后再次启动，应取得新 epoch。 |

需要自定义承载时，使用 `HgsService.create().start(options)` 启动服务，由宿主消费窗口事件并创建自己的页面。X11 可在页面中使用 `HgsWindowSurface`，传入字符串 `windowId` 和 `epoch`，并用 `interactive: true` 接收输入。自管 XComponent 则在 Surface 创建和销毁时分别调用 `session.bindSurface(id, surfaceId, true)` 和 `session.bindSurface(id, surfaceId, false)`。Wayland 自管页面仍用 `HgsSurface` 与快照中的 `legacyId` 接入现有绑定路径。记录事件订阅 token，在宿主退出时退订，并忽略其他 epoch 的迟到事件。

可使用[独立 GTK 与 Xlib 探针](../../tests/integration/README.md)验证真实建窗、输入与重连。API 参数和错误语义见[公共服务 API](../api/service-api.md)，后端工作原理见[技术架构](../architecture/overview.md)。
