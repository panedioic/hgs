import hgsNapi, { HgsServiceInput } from 'libhgs.so';
import { OH2EVDEV } from '../common/KeyMap';
import { mapMouseButton } from '../common/MouseMap';

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
  @Prop interactive: boolean = false;
  private boundSurface: string = '';
  private widthVp: number = 0;
  private heightVp: number = 0;
  private pointerX: number = 0;
  private pointerY: number = 0;
  private keys: Set<number> = new Set();
  private buttons: Set<number> = new Set();
  private touchId: number = -1;
  private controller: HgsWindowSurfaceController = new HgsWindowSurfaceController(
    (surfaceId: string, attach: boolean): void => {
      if (!attach && this.boundSurface === surfaceId) {
        this.releaseInput();
        this.boundSurface = '';
      }
      const queued = hgsNapi.serviceBindSurface(this.windowId, surfaceId, attach, this.epoch);
      if (attach && queued) this.boundSurface = surfaceId;
      else if (attach) hgsNapi.LOGE(`HGS Surface bind rejected for ${this.windowId}`);
    });

  private send(event: HgsServiceInput): boolean {
    return hgsNapi.serviceSendInput(this.windowId, event, this.epoch);
  }

  private acceptingInput(): boolean {
    return this.interactive && this.boundSurface.length > 0;
  }

  private updatePosition(x: number, y: number): boolean {
    if (this.widthVp <= 0 || this.heightVp <= 0 || !Number.isFinite(x) || !Number.isFinite(y)) return false;
    this.pointerX = Math.max(0, Math.min(1, x / this.widthVp));
    this.pointerY = Math.max(0, Math.min(1, y / this.heightVp));
    return true;
  }

  private pointer(action: number, button: number, fromMouse: boolean): boolean {
    return this.send({
      kind: 'pointer', action, button, fromMouse, normalizedPosition: true,
      x: this.pointerX, y: this.pointerY
    });
  }

  private handleMouse(event: MouseEvent): void {
    if (!this.acceptingInput() || event.source === SourceType.TouchScreen ||
      this.touchId >= 0 || !this.updatePosition(event.x, event.y)) return;
    if (event.action === MouseAction.Move) {
      this.pointer(3, 0, true);
      return;
    }
    const button = mapMouseButton(event.button);
    if (!button) return;
    if (event.action === MouseAction.Press && !this.buttons.has(button)) {
      if (this.pointer(1, button, true)) this.buttons.add(button);
    } else if (event.action === MouseAction.Release && this.buttons.has(button)) {
      if (this.pointer(2, button, true)) this.buttons.delete(button);
    }
  }

  private handleTouch(event: TouchEvent): void {
    if (!this.acceptingInput() || event.source === SourceType.Mouse || this.buttons.size > 0) return;
    const button = mapMouseButton(MouseButton.Left);
    if (event.type === TouchType.Cancel) {
      if (this.touchId >= 0) this.pointer(2, button, false);
      this.touchId = -1;
      return;
    }
    for (const point of event.changedTouches) {
      if (event.type === TouchType.Down && this.touchId < 0 && this.updatePosition(point.x, point.y)) {
        if (this.pointer(1, button, false)) this.touchId = point.id;
        return;
      }
      if (point.id !== this.touchId || !this.updatePosition(point.x, point.y)) continue;
      if (event.type === TouchType.Move) this.pointer(3, 0, false);
      else if (event.type === TouchType.Up && this.pointer(2, button, false)) this.touchId = -1;
      return;
    }
  }

  private handleScroll(event?: AxisEvent): void {
    if (!event || !this.acceptingInput() || !this.updatePosition(event.x, event.y)) return;
    const values = [event.getVerticalAxisValue(), event.getHorizontalAxisValue()];
    values.forEach((value: number, axis: number): void => {
      if (Number.isFinite(value) && value !== 0) {
        this.send({
          kind: 'scroll', axis, value, normalizedPosition: true,
          x: this.pointerX, y: this.pointerY
        });
      }
    });
  }

  private handleKey(event: KeyEvent): void {
    if (!this.acceptingInput()) return;
    const evdevCode = OH2EVDEV[event.keyCode] ?? 0;
    if (!evdevCode) return;
    event.stopPropagation();
    const pressed = event.type === KeyType.Down;
    if (pressed === this.keys.has(evdevCode)) return;
    if (this.send({ kind: 'key', evdevCode, pressed })) {
      if (pressed) this.keys.add(evdevCode);
      else this.keys.delete(evdevCode);
    }
  }

  private releaseInput(): void {
    this.keys.forEach((evdevCode: number): void => {
      this.send({ kind: 'key', evdevCode, pressed: false });
    });
    this.keys.clear();
    this.buttons.forEach((button: number): void => { this.pointer(2, button, true); });
    this.buttons.clear();
    if (this.touchId >= 0) this.pointer(2, mapMouseButton(MouseButton.Left), false);
    this.touchId = -1;
  }

  build() {
    XComponent({ id: this.windowId, type: XComponentType.SURFACE, controller: this.controller })
      .width('100%')
      .height('100%')
      .focusable(this.interactive)
      .defaultFocus(this.interactive)
      .focusOnTouch(this.interactive)
      .onAreaChange((_old: Area, area: Area): void => {
        this.widthVp = area.width as number;
        this.heightVp = area.height as number;
      })
      .onKeyEvent((event: KeyEvent): void => this.handleKey(event))
      .onMouse((event: MouseEvent): void => this.handleMouse(event))
      .onTouch((event: TouchEvent): void => this.handleTouch(event))
      .onAxisEvent((event?: AxisEvent): void => this.handleScroll(event))
      .onBlur((): void => this.releaseInput())
  }
}
