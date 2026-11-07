# 独立 Wayland 客户端真机验证

`service_client.c` 仅依赖标准 Wayland/xdg-shell，不调用 Wine、HGS 或 BetterSH 私有 API。它接受一个日志路径参数；480×320 内容区有蓝绿两半，顶部绿色按钮切换最大化，红色按钮关闭。协议回调立即写入日志，供验证键鼠配对、尺寸协商与断连后重连。

本次在 BetterSH 冻结源码副本中将其编为 `libhgs_p1_client.so`，注册为 `hgs-p1-client` 命令。使用 OHOS ARM64 clang、项目 libwayland-client.so 和 wayland-scanner 从固定 xdg-shell XML 生成的 client header/protocol C；编译选项为 `--target=aarch64-linux-ohos -D_GNU_SOURCE -std=c11 -O2 -fPIC -shared -Wall -Wextra`。示例命令：

```sh
hgs-p1-client "$HOME/hgs-p1-client-1.log"
```

依次点击内容区、输入 A、点击最大化、恢复、关闭，日志应有 KEY 30 的按下/释放、对应 BUTTON、480×320 与最大化尺寸的 CONFIGURE/FRAME 及 EXIT。默认 host 的坐标和系统缩放由宿主负责。关闭后再运行一个客户端，应获得同一服务 epoch 下新的公共 windowId/clientId/objectGeneration；最后一个窗口关闭不应停止监听。

客户端绑定 seat 至多 v5，因此为 pointer frame/source/stop/discrete 都提供回调。不要将缺少已协商事件回调造成的测试客户端退出解释为 compositor 产品回归。

本次设备记录和安装包哈希见 WineHua 文档 `docs/architecture/hgs-p1/README.md` 及其指向的证据目录。BetterSH 安装使用保留数据的覆盖方式，release 构建通过 build_hap.sh 自查，并使用既有 debug/JIT Profile 本地签名。
