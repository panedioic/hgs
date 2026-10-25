import hgs from 'libhgs.so';

// hgs 的 XComponent 承载视图 (helloworld 第二步):
// surface 创建后交给 libhgs.so 的空 EGL 渲染器 clear+swap 上屏。
// 接口形状对齐 entry DesktopWindow 的 XComponentController 用法,
// 后续 GL 迁移时替换为真正的 createRenderer。

export class HgsSurfaceController extends XComponentController {
  private rendererId: number = 0;

  bind(rendererId: number): void {
    this.rendererId = rendererId;
  }

  onSurfaceCreated(surfaceId: string): void {
    hgs.createEmptyRenderer(this.rendererId, BigInt(surfaceId));
  }

  onSurfaceDestroyed(): void {
    if (this.rendererId !== 0) {
      hgs.destroyEmptyRenderer(this.rendererId);
    }
  }
}

@Component
export struct EmptyEglView {
  @Prop rendererId: number = 1;
  private controller: HgsSurfaceController = new HgsSurfaceController();

  aboutToAppear(): void {
    this.controller.bind(this.rendererId);
  }

  build() {
    XComponent({ id: `hgs_egl_${this.rendererId}`, type: XComponentType.SURFACE, controller: this.controller })
      .width('100%')
      .height('100%')
  }
}
