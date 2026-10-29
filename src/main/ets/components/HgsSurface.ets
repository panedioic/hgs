// hgs/src/main/ets/components/HgsSurface.ets
import { hilog } from '@kit.PerformanceAnalysisKit';
import hgs from 'libhgs.so';
import { OH2EVDEV, EVDEV_KEY_ENTER } from '../common/KeyMap';
import { mapMouseButton } from '../common/MouseMap';
import { InputDeviceMapper } from '../common/InputDeviceMapper';

// ── 手势状态机常量 (参考原内容: DesktopLayer / WineWindow / WinePopup 同值) ──
// 原引用形态 DesktopLayer.X / WinePointerMachine.X 在本组件统一为顶部常量。
const DOMAIN: number = 0x0000;             // hilog 日志域
const LONG_PRESS_MS: number = 500;         // 长按判定阈值 → 右键
const DRAG_THRESHOLD: number = 10;         // 物理像素, 区分点击抖动和拖拽
const DOUBLE_TAP_MS: number = 400;         // 双击时间窗
const DOUBLE_TAP_DIST: number = 30;        // 双击位移容差 (物理像素)
const SCROLL_STEP_PX: number = 40;         // 双指滚动: 物理像素/离散步 (winewayland.drv 只处理 discrete)
const ENTER_SYNTH_WINDOW_MS: number = 1000; // 回车无 down 补发的判定窗口

// hgs 的 XComponent 承载视图: libraryname 'hgs' 使系统在 surface 创建时
// 携带原生对象触发 libhgs.so 的模块 Init (注册 NativeXComponent 回调),
// 像素由原生空 EGL 渲染器 clear+swap 上屏。同进程后续窗口的 XComponent
// (模块已加载, Init 不会再次执行) 经 onLoad → bindXComponent 补挂回调。

@Component
export struct HgsSurface {
  @Prop xcId: string = 'hgs_xcomp';
  // 目标 id 动态解析 (优先于 toplevelId): 桌面 root 等场景 id 是运行时
  // 识别的 (desktop root 首次 commit 后才产生), 不能用静态 @Prop 传 —
  // DesktopWindow/DesktopLayer 传 () => WWM.getDesktopRootId();
  // 单窗口页面 (WineWindow/WinePopup) 不传, 走 toplevelId。
  // 函数类型不能用 @Prop (V1 限制), 用普通成员: 调用方在 struct 初始化器
  // 里赋值, 首次构造生效 (与三个生命周期回调同款约定)。
  resolveId?: () => number;
  // 内建 controller: 生命周期事件转交三个回调 (id 经 resolveId/toplevelId 解析)
  private controller: HgsSurfaceController = new HgsSurfaceController(
    () => this.resolveId ? this.resolveId() : this.toplevelId,
    (id, sid) => this.onSurfaceCreatedCallback(sid, id),
    (id, sid, w, h) => this.onSurfaceChangedCallback(sid, id, w, h),
    (id) => this.onSurfaceDestroyedCallback(id));
  @Prop toplevelId: number = 0;
  @Prop mouseMode: string = 'window';
  // 悬浮窗态
  @Prop floating: boolean = false;
  // 触摸模拟鼠标
  @Prop private touchpadOn: boolean = false;
  // rawDelta 按输入设备类型缩放 (鼠标/触控板灵敏度差 5~10 倍)
  @Prop scaleRawDelta: boolean = false;
  // 回车无 down 补发 (软键盘/IME 吞 KeyDown 只派 KeyUp 的兜底)
  @Prop enterSynth: boolean = true;
  // 键盘事件目标 id (<=0 不处理键盘; popup 焦点留在父窗口)
  @Prop keyboardTarget: number = 0;
  // 日志前缀
  @Prop tag: string = 'HgsSurface';
  // ── 原生通道占位开关 (计划: surface 生命周期与输入全部改为纯 C++ 实现,
  //    即 XComponent libraryname 触发 NativeXComponent 回调的 blender 模式;
  //    目前两者均为占位, 默认走 ArkTS controller + ArkUI 事件通道) ──
  // true: surface 生命周期由 native 回调驱动 (libraryname 注册通道), 组件
  //       内建 controller 不再转发生命周期
  @Prop useNativeSurfaceCallback: boolean = false;
  // true: 输入事件由 native 侧注册的回调处理 (指针/键盘/滚轮直接进 C++),
  //       ArkUI 的 onTouch/onMouse/onKeyEvent 转发关闭
  @Prop useNativeInputCallback: boolean = false;

  // ── surface 生命周期回调 (宿主注入; 组件内建的 controller 只负责把
  // XComponentController 生命周期事件转交到这里)。
  // 函数类型不能用 @Prop (V1 装饰器限制), 用普通成员 —— 初始化一次后不再变。──
  private onSurfaceCreatedCallback: (surfaceId: string, id: number) => void = (_s: string, _i: number) => {};
  private onSurfaceChangedCallback: (surfaceId: string, id: number, w: number, h: number) => void =
    (_s: string, _i: number, _w: number, _h: number) => {};
  private onSurfaceDestroyedCallback: (id: number) => void = (_i: number) => {};

  // ── 基础回调函数，后续更换为 native 实现 ────────────────────────────────────────────────────
  private touchPending: boolean = false;
  private isDragging: boolean = false;
  private touchStartTime: number = 0;
  private touchStartPx: number = 0;
  private touchStartPy: number = 0;
  private pendingTl: number = 0;
  private pendingPx: number = 0;
  private pendingPy: number = 0;
  private mouseLeftActive: boolean = false;
  private motionLogCounter: number = 0;
  // 双击检测 (目标须一致 — 桌面模式两次点击落在不同窗口不算双击)
  private lastTapTime: number = 0;
  private lastTapPx: number = 0;
  private lastTapPy: number = 0;
  private lastTapTl: number = 0;
  // 双指滚动 (中点位移累积 → 离散步, winewayland.drv 只处理 discrete)
  private twoFingerActive: boolean = false;
  private twoFingerTl: number = 0;
  private twoFingerLastMidX: number = 0;
  private twoFingerLastMidY: number = 0;
  private twoFingerAccumX: number = 0;
  private twoFingerAccumY: number = 0;
  private static readonly SCROLL_STEP_PX: number = 40;
  private scrollLogCounter: number = 0;
  // 桌面模式鼠标捕获-配对 (Press 记目标, Release 用配对目标 — 跨窗口按下抬起)
  private leftBtnTl: number = 0;
  private rightBtnTl: number = 0;
  private midBtnTl: number = 0;
  private hoverLogCounter: number = 0;
  // 回车补发时间戳 (enterSynth)
  private lastEnterDownTs: number = 0;
  private static readonly ENTER_SYNTH_WINDOW_MS: number = 1000;
  // ── 触摸模拟鼠标 (触控板模式, 悬浮条开关; InputSettingsService 镜像) ──
  // 虚拟光标 (物理 px) + 指示点位置 (vp) + 屏幕尺寸缓存
  private vcX: number = 0;
  private vcY: number = 0;
  private vcInit: boolean = false;
  private cursorVpX: number = -100;
  private cursorVpY: number = -100;
  private scrWPx: number = 0;
  private scrHPx: number = 0;
  private cursorSpeed: number = 1.0;
  // XComponent 物理像素尺寸 (onAreaChange)
  private surfacePxW: number = 1;
  private surfacePxH: number = 1;
  private tpDown: boolean = false;
  private tpDownX: number = 0;   // Down 时触摸点 (vp)
  private tpDownY: number = 0;
  private tpLastX: number = 0;
  private tpLastY: number = 0;
  private tpDownTime: number = 0;
  private tpMoved: boolean = false;
  private tpDragArmed: boolean = false;  // 双击后按住: 拖拽预备
  private tpDragging: boolean = false;
  private tpDragTl: number = 0;
  private tpLastTapTime: number = 0;
  private tpLastTapVcX: number = 0;
  private tpLastTapVcY: number = 0;
  private tpTwoFinger: boolean = false;
  private tpTwoFingerLastMidX: number = 0;
  private tpTwoFingerLastMidY: number = 0;
  private tpScrollAccumX: number = 0;
  private tpScrollAccumY: number = 0;

  // ────── 触摸板模式 ────────────────────────────────────────────────────────────
  private handleTouchpad(ev: TouchEvent): void {
    // 物理鼠标合成 touch 旁路 (与直接触控同一守卫; onMouse Press 里经
    // onGestureReset 钩子清态)
    if (this.isMouseLeftActive()) return;
    this.ensureScreenPx();

    // 双指: 滚动作用于光标处 (触控板语义), 并打断单指/拖拽
    if (ev.touches.length >= 2) {
      let midX = 0, midY = 0;
      for (let i = 0; i < ev.touches.length; i++) {
        midX += vp2px(ev.touches[i].x);
        midY += vp2px(ev.touches[i].y);
      }
      midX /= ev.touches.length;
      midY /= ev.touches.length;
      if (!this.tpTwoFinger) {
        if (this.tpDragging && this.tpDragTl > 0) {
          hgs.sendPointerEvent(this.tpDragTl, MouseAction.Release,
            Math.round(this.vcX), Math.round(this.vcY), mapMouseButton(1));
        }
        this.tpDown = false;
        this.tpDragging = false;
        this.tpDragTl = 0;
        this.tpDragArmed = false;
        this.tpTwoFinger = true;
        this.tpTwoFingerLastMidX = midX;
        this.tpTwoFingerLastMidY = midY;
        this.tpScrollAccumX = 0;
        this.tpScrollAccumY = 0;
      } else {
        this.tpScrollAccumY += this.tpTwoFingerLastMidY - midY;
        this.tpScrollAccumX += midX - this.tpTwoFingerLastMidX;
        this.tpTwoFingerLastMidX = midX;
        this.tpTwoFingerLastMidY = midY;
        const tl = this.tlAtCursor();
        if (tl > 0) {
          while (Math.abs(this.tpScrollAccumY) >= SCROLL_STEP_PX) {
            const sign = this.tpScrollAccumY > 0 ? 1 : -1;
            hgs.sendScrollEvent(tl, 0, sign, SCROLL_STEP_PX,
              Math.round(this.vcX), Math.round(this.vcY));
            this.tpScrollAccumY -= sign * SCROLL_STEP_PX;
          }
          while (Math.abs(this.tpScrollAccumX) >= SCROLL_STEP_PX) {
            const sign = this.tpScrollAccumX > 0 ? 1 : -1;
            hgs.sendScrollEvent(tl, 1, sign, SCROLL_STEP_PX,
              Math.round(this.vcX), Math.round(this.vcY));
            this.tpScrollAccumX -= sign * SCROLL_STEP_PX;
          }
        }
      }
      return;
    }
    if (this.tpTwoFinger) {
      this.tpTwoFinger = false;
      return;
    }

    const t = ev.changedTouches[0];
    if (!t) return;

    if (ev.type === TouchType.Down) {
      this.tpDown = true;
      this.tpMoved = false;
      this.tpDownX = t.x;
      this.tpDownY = t.y;
      this.tpLastX = t.x;
      this.tpLastY = t.y;
      this.tpDownTime = Date.now();
      // 双击后按住 → 拖拽预备 (标准触控板 drag 手势: 第二次 Down 且光标没怎么挪)
      const dvc = Math.abs(this.vcX - this.tpLastTapVcX) + Math.abs(this.vcY - this.tpLastTapVcY);
      this.tpDragArmed = this.tpLastTapTime > 0
        && (Date.now() - this.tpLastTapTime) < DOUBLE_TAP_MS
        && dvc < DOUBLE_TAP_DIST * 2;
    } else if (ev.type === TouchType.Move) {
      if (!this.tpDown) return;
      const dxPx = vp2px(t.x - this.tpLastX) * this.cursorSpeed;
      const dyPx = vp2px(t.y - this.tpLastY) * this.cursorSpeed;
      this.tpLastX = t.x;
      this.tpLastY = t.y;
      if (dxPx == 0 && dyPx == 0) return;
      if (!this.tpMoved) {
        const moved = Math.abs(vp2px(t.x - this.tpDownX)) + Math.abs(vp2px(t.y - this.tpDownY));
        if (moved >= DRAG_THRESHOLD) {
          this.tpMoved = true;
        }
      }
      this.vcX = Math.min(Math.max(this.vcX + dxPx, 0), this.scrWPx);
      this.vcY = Math.min(Math.max(this.vcY + dyPx, 0), this.scrHPx);
      this.syncCursorIndicator();
      if (this.tpDragArmed && !this.tpDragging && this.tpMoved) {
        // 拖拽开始: 在光标处按下左键
        this.tpDragging = true;
        this.tpDragTl = this.tlAtCursor();
        if (this.tpDragTl > 0) {
          hgs.raiseToplevel(this.tpDragTl);
          hgs.sendPointerEvent(this.tpDragTl, MouseAction.Press,
            Math.round(this.vcX), Math.round(this.vcY), mapMouseButton(1));
        }
      }
      const moveTl = this.tpDragging ? this.tpDragTl : this.tlAtCursor();
      if (moveTl > 0) {
        hgs.sendPointerEvent(moveTl, MouseAction.Move,
          Math.round(this.vcX), Math.round(this.vcY), 0);
      }
    } else if (ev.type === TouchType.Up) {
      if (!this.tpDown) return;
      this.tpDown = false;
      if (this.tpDragging) {
        if (this.tpDragTl > 0) {
          hgs.sendPointerEvent(this.tpDragTl, MouseAction.Release,
            Math.round(this.vcX), Math.round(this.vcY), mapMouseButton(1));
        }
        this.tpDragging = false;
        this.tpDragTl = 0;
        this.tpDragArmed = false;
        this.tpLastTapTime = 0;
        return;
      }
      this.tpDragArmed = false;
      if (!this.tpMoved) {
        const elapsed = Date.now() - this.tpDownTime;
        if (elapsed >= LONG_PRESS_MS) {
          this.clickAtCursor(mapMouseButton(2));
          this.tpLastTapTime = 0;
        } else {
          this.clickAtCursor(mapMouseButton(1));
          this.tpLastTapTime = Date.now();
          this.tpLastTapVcX = this.vcX;
          this.tpLastTapVcY = this.vcY;
        }
      }
    } else if (ev.type === TouchType.Cancel) {
      this.resetTouchpadState();
    }
  }

  // ────── 触摸 ──────────────────────────────────────────────────────────────────
  // 触摸总入口: floating 悬浮旁路 → touchpadOn 触控板分流 → 直接触控状态机
  handleNormalTouch(ev: TouchEvent): void {
    if (this.floating) {
      return;
    }
    if (this.touchpadOn) {
      this.handleTouchpad(ev);
      return;
    }
    this.handleNormalTouchInner(ev);
  }

  private handleNormalTouchInner(ev: TouchEvent): void {
    try {
      // Cancel 统一前置处理: 不依赖 touches 数量 (双指滚动中 Cancel 不清理
      // 则 twoFingerActive 滞留, 下一次单指 Down 被 SCROLL-END 分支吞掉)
      if (ev.type === TouchType.Cancel) {
        if (this.isDragging) {
          hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] %{public}s CANCEL-DRAG id=%{public}d px=(%{public}d,%{public}d)',
            this.tag, this.pendingTl, this.pendingPx, this.pendingPy);
          hgs.sendPointerEvent(this.pendingTl, MouseAction.Release,
            this.pendingPx, this.pendingPy, mapMouseButton(1));
        }
        this.touchPending = false;
        this.isDragging = false;
        this.twoFingerActive = false;
        return;
      }
      // 物理鼠标左键按下期间: 忽略所有触摸 (守卫必须前置, 鼠标按住+双指触屏
      // 会误入滚动)
      if (this.mouseLeftActive) return;

      // ── 双指滚动 ──
      if (ev.touches.length >= 2) {
        if (this.touchPending || this.isDragging) {
          if (this.isDragging) {
            hgs.sendPointerEvent(this.pendingTl, MouseAction.Release,
              this.pendingPx, this.pendingPy, mapMouseButton(1));
          }
          this.touchPending = false;
          this.isDragging = false;
        }
        let midX = 0, midY = 0;
        for (let i = 0; i < ev.touches.length; i++) {
          midX += vp2px(ev.touches[i].x);
          midY += vp2px(ev.touches[i].y);
        }
        midX = Math.round(midX / ev.touches.length);
        midY = Math.round(midY / ev.touches.length);
        if (!this.twoFingerActive) {
          this.twoFingerActive = true;
          this.twoFingerTl = this.toplevelId;
          hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] %{public}s SCROLL-START id=%{public}d fingers=%{public}d',
            this.tag, this.twoFingerTl, ev.touches.length);
          this.twoFingerLastMidX = midX;
          this.twoFingerLastMidY = midY;
          this.twoFingerAccumY = 0;
          this.twoFingerAccumX = 0;
          this.scrollLogCounter = 0;
        } else {
          this.twoFingerAccumY += this.twoFingerLastMidY - midY;  // 向上=正=向下滚
          this.twoFingerAccumX += midX - this.twoFingerLastMidX;  // 向右=正=向右滚
          this.twoFingerLastMidX = midX;
          this.twoFingerLastMidY = midY;
          while (Math.abs(this.twoFingerAccumY) >= SCROLL_STEP_PX) {
            const sign = this.twoFingerAccumY > 0 ? 1 : -1;
            hgs.sendScrollEvent(this.twoFingerTl, 0, sign, SCROLL_STEP_PX, midX, midY);
            this.twoFingerAccumY -= sign * SCROLL_STEP_PX;
            if (++this.scrollLogCounter % 5 === 0) {
              hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] %{public}s SCROLL-V id=%{public}d', this.tag, this.twoFingerTl);
            }
          }
          while (Math.abs(this.twoFingerAccumX) >= SCROLL_STEP_PX) {
            const sign = this.twoFingerAccumX > 0 ? 1 : -1;
            hgs.sendScrollEvent(this.twoFingerTl, 1, sign, SCROLL_STEP_PX, midX, midY);
            this.twoFingerAccumX -= sign * SCROLL_STEP_PX;
          }
        }
        return;
      }

      // ── 单指: 恢复双指状态 ──
      if (this.twoFingerActive) {
        this.twoFingerActive = false;
        hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] %{public}s SCROLL-END', this.tag);
        return;
      }

      const t = ev.changedTouches[0];
      if (!t) return;
      const px = Math.round(vp2px(t.x));
      const py = Math.round(vp2px(t.y));

      if (ev.type === TouchType.Down) {
        const tl = this.toplevelId;
        if (tl <= 0) {
          hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] %{public}s TOUCH skipped (no target at %{public}d,%{public}d)', this.tag, px, py);
          return;
        }
        hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] %{public}s TOUCH id=%{public}d px=(%{public}d,%{public}d)', this.tag, tl, px, py);
        this.touchPending = true;
        this.isDragging = false;
        this.touchStartTime = Date.now();
        this.touchStartPx = px;
        this.touchStartPy = py;
        this.pendingTl = tl;
        this.pendingPx = px;
        this.pendingPy = py;
        this.motionLogCounter = 0;

      } else if (ev.type === TouchType.Move) {
        if (!this.touchPending && !this.isDragging) return;
        const dist = Math.abs(px - this.touchStartPx) + Math.abs(py - this.touchStartPy);
        if (this.touchPending && dist >= DRAG_THRESHOLD) {
          // 进入拖拽模式: 补发 Press (Down 时未发); 拖拽打断双击序列
          this.touchPending = false;
          this.isDragging = true;
          this.lastTapTime = 0;
          hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] %{public}s DRAG-START id=%{public}d dist=%{public}d', this.tag, this.pendingTl, dist);
          hgs.sendPointerEvent(this.pendingTl, MouseAction.Press, this.pendingPx, this.pendingPy, mapMouseButton(1));
          hgs.sendPointerEvent(this.pendingTl, MouseAction.Move, px, py, mapMouseButton(1));
        } else if (this.isDragging) {
          // 子窗口承载的标题栏拖动: OHOS startMoving 接管系统拖拽 (触摸路径
          // 必须处理 — 合成 mouse Move 被手势守卫吞掉)
          hgs.sendPointerEvent(this.pendingTl, MouseAction.Move, px, py, 0);
          if (++this.motionLogCounter % 10 === 0) {
            hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] %{public}s DRAG-MOVE id=%{public}d frame=%{public}d',
              this.tag, this.pendingTl, this.motionLogCounter);
          }
        }
        // PENDING 且未超阈值: 等待 Up 判定长按/点击

      } else if (ev.type === TouchType.Up) {
        if (this.isDragging) {
          hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] %{public}s DRAG-END id=%{public}d px=(%{public}d,%{public}d)',
            this.tag, this.pendingTl, px, py);
          hgs.sendPointerEvent(this.pendingTl, MouseAction.Release, px, py, mapMouseButton(1));
          this.isDragging = false;
        } else if (this.touchPending) {
          this.touchPending = false;
          const elapsed = Date.now() - this.touchStartTime;
          if (elapsed >= LONG_PRESS_MS) {
            // 长按 → 右键; 打断双击序列
            const btnRight = mapMouseButton(2);
            this.lastTapTime = 0;
            hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] %{public}s LONGPRESS->RIGHTCLICK id=%{public}d elapsed=%{public}dms', this.tag, this.pendingTl, elapsed);
            hgs.sendPointerEvent(this.pendingTl, MouseAction.Press, this.pendingPx, this.pendingPy, btnRight);
            hgs.sendPointerEvent(this.pendingTl, MouseAction.Release, this.pendingPx, this.pendingPy, btnRight);
          } else {
            // 短按 → 左键 或 双击 (双击只补发第二次)
            const btnLeft = mapMouseButton(1);
            const now = Date.now();
            const dist = Math.abs(this.pendingPx - this.lastTapPx) + Math.abs(this.pendingPy - this.lastTapPy);
            const isDoubleTap = (this.lastTapTime > 0
              && (now - this.lastTapTime) < DOUBLE_TAP_MS
              && dist < DOUBLE_TAP_DIST
              && this.pendingTl === this.lastTapTl);
            if (isDoubleTap) {
              this.lastTapTime = 0;
              hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] %{public}s DOUBLE-TAP id=%{public}d', this.tag, this.pendingTl);
              hgs.sendPointerEvent(this.pendingTl, MouseAction.Press, this.lastTapPx, this.lastTapPy, btnLeft);
              hgs.sendPointerEvent(this.pendingTl, MouseAction.Release, this.lastTapPx, this.lastTapPy, btnLeft);
            } else {
              hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] %{public}s TAP->LEFTCLICK id=%{public}d px=(%{public}d,%{public}d)',
                this.tag, this.pendingTl, this.pendingPx, this.pendingPy);
              hgs.sendPointerEvent(this.pendingTl, MouseAction.Press, this.pendingPx, this.pendingPy, btnLeft);
              hgs.sendPointerEvent(this.pendingTl, MouseAction.Release, this.pendingPx, this.pendingPy, btnLeft);
              this.lastTapTime = now;
              this.lastTapPx = this.pendingPx;
              this.lastTapPy = this.pendingPy;
              this.lastTapTl = this.pendingTl;
            }
          }
        }
      }
    } catch (e) {
      hilog.error(DOMAIN, 'CLICK-PIPE', '[ArkTS] %{public}s TOUCH handler exception: %{public}s', this.tag, JSON.stringify(e));
    }
  }

  // ── 物理鼠标 ──────────────────────────────────────────────────────────────
  onWineMouse(ev: MouseEvent): void {
    if (this.floating) {
        return;
    }
    if (this.mouseMode === 'desktop') {
      this.onMouseDesktop(ev);
    } else {
      this.onMouseWindow(ev);
    }
  }

  // 单窗口直通型 (WineWindow/WinePopup): 真实时序直通 + 手势守卫
  private onMouseWindow(ev: MouseEvent): void {
    const tag = this.tag;
    const id = this.toplevelId;
    if (id <= 0) return;
    // Wine 发起 xdg_toplevel.move → 下一个 MOVE 时 OHOS startMoving 接管
    // (必须优先于手势守卫: move 请求的 Move 被吞则 moveGrabPending 悬挂)
    // 触摸手势进行中: 手指触摸的合成 mouse 事件由 onTouch 状态机负责
    if (ev.action === MouseAction.Move &&
      (this.touchPending || this.isDragging || this.twoFingerActive)) return;
    if (ev.action === MouseAction.Release) {
      // 复位不限定 Left: 跨窗口/失焦释放时左键 Release 到不了本窗口,
      // 只认 Left 会悬挂 mouseLeftActive, 本窗口触控永久失效
      this.mouseLeftActive = false;
    }
    if (ev.button === MouseButton.Left && ev.action === MouseAction.Press) {
      this.mouseLeftActive = true;
      // 竞态: 合成 touch 可能已先进 DRAG 并注入 Press → 补 Release 配对
      if (this.isDragging) {
        hgs.sendPointerEvent(id, MouseAction.Release,
          this.pendingPx, this.pendingPy, mapMouseButton(1));
      }
      this.touchPending = false;
      this.isDragging = false;
      this.twoFingerActive = false;
    }
    const px = vp2px(ev.x);
    const py = vp2px(ev.y);
    const btn = mapMouseButton(ev.button);
    if (ev.action !== MouseAction.Move) {
      hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] %{public}s MOUSE id=%{public}d action=%{public}d px=(%{public}d,%{public}d) btn=%{public}d',
        this.tag, id, ev.action, Math.round(px), Math.round(py), btn);
    }
    let rawDx = ev.rawDeltaX ?? 0;
    let rawDy = ev.rawDeltaY ?? 0;
    if (this.scaleRawDelta) {
      const devId = ev.deviceId ?? 0;
      if (devId > 0) {
        const s = InputDeviceMapper.getInstance().getScale(devId);
        rawDx *= s;
        rawDy *= s;
      }
    }
    hgs.sendPointerEvent(id, ev.action, px, py, btn, rawDx, rawDy, true);
  }

  // 桌面捕获-配对型 (DesktopWindow/DesktopLayer): Press 记目标并置顶,
  // Release 用配对目标 (跨窗口按下抬起); Move 沿用活动按钮目标否则命中
  private onMouseDesktop(ev: MouseEvent): void {
    try {
      if (ev.action !== MouseAction.Move) {
        hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] MOUSE-RAW a=%{public}d btn=%{public}d ts=%{public}d',
          ev.action, ev.button, Math.round(ev.timestamp % 1000000000));
      }
      if (ev.action === MouseAction.Move) {
        if (this.touchPending || this.isDragging || this.twoFingerActive) return;
        const px = Math.round(vp2px(ev.x));
        const py = Math.round(vp2px(ev.y));
        let tl = this.leftBtnTl || this.rightBtnTl || this.midBtnTl;
        if (tl <= 0) {
          tl = this.toplevelId;
          if (tl <= 0) return;
        }
        if (++this.hoverLogCounter % 120 === 0) {
          hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] %{public}s MOUSE-MOVE id=%{public}d n=%{public}d',
            this.tag, tl, this.hoverLogCounter);
        }
        let rawDx = ev.rawDeltaX ?? 0;
        let rawDy = ev.rawDeltaY ?? 0;
        if (this.scaleRawDelta) {
          const devId = ev.deviceId ?? 0;
          if (devId > 0) {
            const s = InputDeviceMapper.getInstance().getScale(devId);
            rawDx *= s;
            rawDy *= s;
          }
        }
        hgs.sendPointerEvent(tl, MouseAction.Move, px, py, 0, rawDx, rawDy);
        return;
      }
      if (ev.action !== MouseAction.Press && ev.action !== MouseAction.Release) return;
      const px = Math.round(vp2px(ev.x));
      const py = Math.round(vp2px(ev.y));
      if (ev.button === MouseButton.Left) {
        const btnLeft = mapMouseButton(1);
        if (ev.action === MouseAction.Press) {
          this.mouseLeftActive = true;
          this.touchPending = false;
          this.isDragging = false;
              const tl = this.toplevelId;
          if (tl <= 0) {
            this.leftBtnTl = 0;
            return;
          }
          this.leftBtnTl = tl;
          hgs.raiseToplevel(tl);
          hgs.sendPointerEvent(tl, MouseAction.Press, px, py, btnLeft, 0, 0, true);
        } else {
          if (this.leftBtnTl <= 0) {
            this.mouseLeftActive = false;
            return;
          }
          hgs.sendPointerEvent(this.leftBtnTl, MouseAction.Release, px, py, btnLeft);
          this.leftBtnTl = 0;
          this.mouseLeftActive = false;
        }
        return;
      }
      if (ev.button !== MouseButton.Right && ev.button !== MouseButton.Middle) return;
      const isRight = ev.button === MouseButton.Right;
      const btnCode = mapMouseButton(isRight ? 2 : 4);
      if (ev.action === MouseAction.Press) {
        const tl = this.toplevelId;
        if (tl <= 0) {
          if (isRight) this.rightBtnTl = 0; else this.midBtnTl = 0;
          return;
        }
        if (isRight) this.rightBtnTl = tl; else this.midBtnTl = tl;
        hgs.raiseToplevel(tl);
        hgs.sendPointerEvent(tl, MouseAction.Press, px, py, btnCode, 0, 0, true);
      } else {
        const btnTl = isRight ? this.rightBtnTl : this.midBtnTl;
        if (btnTl <= 0) return;
        hgs.sendPointerEvent(btnTl, MouseAction.Release, px, py, btnCode);
        if (isRight) this.rightBtnTl = 0; else this.midBtnTl = 0;
      }
    } catch (e) {
      hilog.error(DOMAIN, 'CLICK-PIPE', '[ArkTS] %{public}s MOUSE handler exception: %{public}s', this.tag, JSON.stringify(e));
    }
  }

  // ── 滚轮轴事件 (轴事件不冒泡, 必须挂在命中链顶端的 XComponent 上) ───────────────────────
  onWineAxisEvent(event?: AxisEvent): void {
    if (this.floating) return;  // 悬浮窗态: 输入旁路
    if (!event) return;
    const px = vp2px(event.x);
    const py = vp2px(event.y);
    const id = this.toplevelId;
    if (id <= 0) return;
    const vert = event.getVerticalAxisValue();
    const horiz = event.getHorizontalAxisValue();
    const step = event.scrollStep ?? 0;
    if (vert !== 0) {
      hgs.sendScrollEvent(id, 0, vert, step, px, py);
    }
    if (horiz !== 0) {
      hgs.sendScrollEvent(id, 1, horiz, step, px, py);
    }
  }

  // ── 键盘 ──────────────────────────────────────────────────────────────────
  onWineKeyEvent(ev: KeyEvent): void {
    if (this.floating) return;  // 悬浮窗态: 输入旁路
    const id = this.keyboardTarget;
    if (id <= 0) return;
    ev.stopPropagation();
    const evdevCode = OH2EVDEV[ev.keyCode] ?? 0;
    if (evdevCode === 0) {
      hilog.warn(DOMAIN, 'KBD-PIPE', '[ArkTS] %{public}s KEY unmapped ohosCode=%{public}d', this.tag, ev.keyCode);
      return;
    }
    const pressed = (ev.type === KeyType.Down);
    hilog.info(DOMAIN, 'KBD-PIPE', '[ArkTS] %{public}s KEY id=%{public}d ohos=%{public}d->evdev=%{public}d %{public}s',
      this.tag, id, ev.keyCode, evdevCode, pressed ? 'DOWN' : 'UP');
    // 回车补发: 软键盘/IME 场景系统吞 KeyDown 只派发 KeyUp → 无配对 down 时
    // 补发完整 down+up, 保证 Wine 触发按键
    if (this.enterSynth && evdevCode === EVDEV_KEY_ENTER) {
      if (pressed) {
        this.lastEnterDownTs = Date.now();
      } else if (Date.now() - this.lastEnterDownTs > ENTER_SYNTH_WINDOW_MS) {
        hgs.sendKeyEvent(id, EVDEV_KEY_ENTER, true);
      }
    }
    hgs.sendKeyEvent(id, evdevCode, pressed);
  }

  /** 触控板等外部模式接管时, 清空手势状态机 (防残留状态吞下次触摸) */
  resetGestures(): void {
    this.touchPending = false;
    this.isDragging = false;
    this.twoFingerActive = false;
    this.lastTapTime = 0;
  }

  /** 物理鼠标左键按下中 (触控板等旁路模式判断合成 touch 是否应忽略) */
  isMouseLeftActive(): boolean {
    return this.mouseLeftActive;
  }

  // ── 事件入口 (build 转发目标) ────────────────────────────────────────────────
  onWineTouch(ev: TouchEvent): void {
    if (this.floating) {
      return;
    }
    if (this.touchpadOn) {
      this.handleTouchpad(ev);
      return;
    }
    this.handleNormalTouch(ev);
  }

  // ── 触控板辅助 (handleTouchpad 依赖) ────────────────────────────────────────
  private ensureScreenPx(): void {
    if (this.scrWPx > 0) return;
    this.scrWPx = 1280;
    this.scrHPx = 800;
  }

  private syncCursorIndicator(): void {
    // 指示点为 14vp 圆, position 是左上角坐标 → 减半径使圆心对正光标
    this.cursorVpX = px2vp(this.vcX) - 7;
    this.cursorVpY = px2vp(this.vcY) - 7;
  }

  private tlAtCursor(): number {
    return (hgs.findToplevelAt(Math.round(this.vcX), Math.round(this.vcY)) as number) ?? 0;
  }

  private clickAtCursor(btn: number): void {
    const tl = this.tlAtCursor();
    if (tl <= 0) return;
    hgs.raiseToplevel(tl);
    const x = Math.round(this.vcX);
    const y = Math.round(this.vcY);
    hgs.sendPointerEvent(tl, MouseAction.Press, x, y, btn);
    hgs.sendPointerEvent(tl, MouseAction.Release, x, y, btn);
  }

  private resetTouchpadState(): void {
    this.tpDown = false;
    this.tpMoved = false;
    this.tpDragArmed = false;
    this.tpDragging = false;
    this.tpDragTl = 0;
    this.tpTwoFinger = false;
    this.tpLastTapTime = 0;
    this.cursorVpX = -100;
    this.cursorVpY = -100;
  }

  // ── XComponent Arkts 实现 ────────────────────────────────────────────────────
  build() {
    Stack() {
      if (this.useNativeSurfaceCallback && this.useNativeInputCallback) {
        // 原生通道 (占位, 暂时空实现):
        // surface 生命周期与输入均计划改为纯 C++ 实现 — XComponent 用
        // libraryname 触发 NativeXComponent 回调 (napi_init 的
        // OnSurfaceCreated/Changed/Destroyed 与输入注册), 本组件不再挂
        // controller 与 ArkUI 事件转发。落位前此分支不承载任何窗口。
        XComponent({
          id: this.xcId,
          type: XComponentType.SURFACE,
          libraryname: 'hgs',
        })
          .width('100%')
          .height('100%')
      } else {
        XComponent({
          id: this.xcId,
          type: XComponentType.SURFACE,
          controller: this.controller,
        })
        .width('100%')
        .height('100%')
        .backgroundColor('#00000000')
        .focusable(true)
        .defaultFocus(true)
        .onTouch((event: TouchEvent) => this.onWineTouch(event))
        .onMouse((event: MouseEvent) => this.onWineMouse(event))
        .onAxisEvent((event?: AxisEvent) => this.onWineAxisEvent(event))
        .onKeyEvent((event: KeyEvent) => this.onWineKeyEvent(event))
        .onAreaChange((_o: Area, n: Area) => {
          this.surfacePxW = vp2px(n.width as number);
          this.surfacePxH = vp2px(n.height as number);
          hilog.info(DOMAIN, this.tag, '[Ctrl] onAreaChange xc=%{public}s w=%{public}d h=%{public}d',
            this.xcId, Math.round(this.surfacePxW), Math.round(this.surfacePxH));
        })

        // ── 触控板虚拟光标指示 (不响应触摸, 纯展示) ──
        if (this.touchpadOn) {
          // 描边不用纯白: 白色背景 (记事本等) 下光标会整个消失, 用浅灰保证可辨
          Circle({ width: 14, height: 14 })
            .fill('#66FFFFFF')
            .stroke('#FFB3B3B3')
            .strokeWidth(1.5)
            .position({ x: this.cursorVpX, y: this.cursorVpY })
            .hitTestBehavior(HitTestMode.None)
        }
      }
    }
    .width('100%')
    .height('100%')
  }
}

// ============================================================================
// HgsSurfaceController — 组件内建的 XComponentController。
// 生命周期事件 → 宿主注入的三个回调 (id 由组件按 toplevelId 解析后一并
// 回传, 宿主无需再自己持表查询)。
// ============================================================================
class HgsSurfaceController extends XComponentController {
  private getId: () => number;
  private created: (id: number, surfaceId: string) => void;
  private changed: (id: number, surfaceId: string, w: number, h: number) => void;
  private destroyed: (id: number) => void;

  constructor(getId: () => number,
              created: (id: number, surfaceId: string) => void,
              changed: (id: number, surfaceId: string, w: number, h: number) => void,
              destroyed: (id: number) => void) {
    super();
    this.getId = getId;
    this.created = created;
    this.changed = changed;
    this.destroyed = destroyed;
  }

  onSurfaceCreated(surfaceId: string): void {
    const id = this.getId();
    if (id > 0) {
      this.created(id, surfaceId);
    }
  }

  onSurfaceChanged(surfaceId: string, rect: SurfaceRect): void {
    const id = this.getId();
    if (id > 0) {
      this.changed(id, surfaceId, Math.round(rect.surfaceWidth), Math.round(rect.surfaceHeight));
    }
  }

  onSurfaceDestroyed(_surfaceId: string): void {
    const id = this.getId();
    if (id > 0) {
      this.destroyed(id);
    }
  }
}
