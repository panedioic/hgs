import hgsNapi from 'libhgs.so';

class HgsWindowSurfaceController extends XComponentController {
  private bind: (surfaceId: string, attach: boolean) => void;

  constructor(bind: (surfaceId: string, attach: boolean) => void) {
    super();
    this.bind = bind;
  }

  onSurfaceCreated(surfaceId: string): void {
    this.bind(surfaceId, true);
  }

  onSurfaceDestroyed(surfaceId: string): void {
    this.bind(surfaceId, false);
  }
}

/** Displays a service window on backends with OHOS Surface support. */
@Component
export struct HgsWindowSurface {
  @Require @Prop windowId: string;
  @Require @Prop epoch: string;
  private controller: HgsWindowSurfaceController = new HgsWindowSurfaceController(
    (surfaceId: string, attach: boolean): void => {
      const queued = hgsNapi.serviceBindSurface(this.windowId, surfaceId, attach, this.epoch);
      if (attach && !queued) hgsNapi.LOGE(`HGS Surface bind rejected for ${this.windowId}`);
    });

  build() {
    XComponent({ id: this.windowId, type: XComponentType.SURFACE, controller: this.controller })
      .width('100%')
      .height('100%')
  }
}
