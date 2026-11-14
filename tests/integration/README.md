# 独立 Wayland 客户端真机验证

`service_client.c` 仅依赖标准 Wayland/xdg-shell，不调用 Wine、HGS 或 BetterSH 私有 API。它接受一个日志路径参数；480×320 内容区有蓝绿两半，顶部绿色按钮切换最大化，红色按钮关闭。协议回调立即写入日志，供验证键鼠配对、尺寸协商与断连后重连。

本次在 BetterSH 冻结源码副本中将其编为 `libhgs_p1_client.so`，注册为 `hgs-p1-client` 命令。使用 OHOS ARM64 clang、项目 libwayland-client.so 和 wayland-scanner 从固定 xdg-shell XML 生成的 client header/protocol C；编译选项为 `--target=aarch64-linux-ohos -D_GNU_SOURCE -std=c11 -O2 -fPIC -shared -Wall -Wextra`。示例命令：

```sh
hgs-p1-client "$HOME/hgs-p1-client-1.log"
```

依次点击内容区、输入 A、点击最大化、恢复、关闭，日志应有 KEY 30 的按下/释放、对应 BUTTON、480×320 与最大化尺寸的 CONFIGURE/FRAME 及 EXIT。默认 host 的坐标和系统缩放由宿主负责。关闭后再运行一个客户端，应获得同一服务 epoch 下新的公共 windowId/clientId/objectGeneration；最后一个窗口关闭不应停止监听。

客户端绑定 seat 至多 v5，因此为 pointer frame/source/stop/discrete 都提供回调。不要将缺少已协商事件回调造成的测试客户端退出解释为 compositor 产品回归。

本次设备记录和安装包哈希见 WineHua 文档 `docs/architecture/hgs-p1/README.md` 及其指向的证据目录。BetterSH 安装使用保留数据的覆盖方式，release 构建通过 build_hap.sh 自查，并使用既有 debug/JIT Profile 本地签名。

## 最小宿主的 GTK 与 Xlib 客户端

- `x11/client.cpp` 是独立 Xlib 探针，验证 Xauthority、绘制、键鼠及客户端退出。配合 `x11/EntryAbility.ets` 覆盖 X11 重连和 both 启动回滚。
- `gtk/child.cpp` 是 GTK 客户端的 NCP 入口。用 OHOS ARM64 C++ 编译为 `libhgs_gtk_probe_child.so`，链接动态加载所需库。用 `scripts/smoke/build_gtk_smoke.sh` 构建 `libgtksmoke.so` 及配套 GTK 依赖，为测试包准备 Fontconfig 配置和字体、键盘资源。GTK 与 HGS 使用一致的 Wayland、XKB 运行库。
- `x11/launcher.cpp` 的 NAPI `launch(params)` 保持 Xlib 探针入口。传入第二个参数 `'wayland'` 可启动 GTK NCP。GTK 的参数由换行分隔，依次为 `clientEnvironment('wayland')` 的 XDG_RUNTIME_DIR、WAYLAND_DISPLAY、日志路径、含 `rules/evdev` 的 XKB 目录和 Fontconfig 文件。Xlib 参数使用 X11 端点和认证文件。

将 `gtk/EntryAbility.ets` 用作独立 HAP 的入口，配好默认宿主的 Wayland multiton Ability 和两个共享 LocalStorage 的包装页。把 `launch(params: string, protocol?: 'wayland'): number` 声明为 `libhgs_x11_probe.so` 的 NAPI 接口，将 Xlib 探针打包为 `libhgs_x11_probe_child.so`，并加入 rawfile `hgs-x11-res.zip` 和 `hgs-gtk-fonts.zip`。字体 zip 在根目录下包含 `fonts/`。用 Want 的 `protocols=wayland|x11|both` 选择测试，缺省 both。检查应用文件目录内的 `hgs-matrix.log`、`hgs-matrix-x11.log` 与 `hgs-matrix-gtk.log`。

独立宿主在 entry 模块声明名为 `HgsAppWindowAbility` 的 multiton Ability，类继承 HGS 的同名 Ability。页面表加入 `pages/Index`、`pages/HgsWindowPage`、`pages/HgsServiceWindow`。后两个包装页分别渲染 HGS 的 `HgsWindowPage`、`HgsServiceWindowPage`，两页均使用 `@Entry({ useSharedStorage: true })`。X11 依赖、HAR 构建参数和资源包生成命令见 [X11 构建和接入](../../docs/integration/x11.md)。

每轮先结束应用进程，再用 `aa start -a EntryAbility -b <测试包名> --ps protocols <模式>` 启动。调试包可用 `hdc file recv -b <测试包名> /data/storage/el2/base/haps/entry/files/hgs-matrix.log <本地路径>` 读取应用私有日志。各模式只检查该轮启动的客户端，未启动的客户端可能留有上轮日志。Xlib 的 `CONNECTED`、`FRAME OK` 分别证明连接和绘制；最终 `PASS` 还要求成对的鼠标和键盘事件。

要手动检查 both，在 Xlib 的 8 秒等待时间内点击 X11 窗口并按 A。GTK 的 `gtksmoke: entry=` 行可以检查真实键入是否到达对应窗口。原 epoch 重连可用 `x11/EntryAbility.ets` 的自动测试复验。
