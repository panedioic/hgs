import hgs from 'libhgs.so';

// hgs 的 XComponent 承载视图: libraryname 'hgs' 使系统在 surface 创建时
// 携带原生对象触发 libhgs.so 的模块 Init (注册 NativeXComponent 回调),
// 像素由原生空 EGL 渲染器 clear+swap 上屏。同进程后续窗口的 XComponent
// (模块已加载, Init 不会再次执行) 经 onLoad → bindXComponent 补挂回调。

@Component
export struct EmptyEglView {
  private controller: XComponentController = new XComponentController();

  build() {
    XComponent({
      id: 'hgs_canvas',
      type: XComponentType.SURFACE,
      libraryname: 'hgs',
      controller: this.controller,
    })
      .width('100%')
      .height('100%')
      .onLoad((event?: object) => {
        if (event) {
          hgs.bindXComponent(event);
        }
      })
  }
}
