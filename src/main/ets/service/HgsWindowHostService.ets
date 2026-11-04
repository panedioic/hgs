// hgs/src/main/ets/service/HgsWindowHostService.ets
//
// 默认 ArkTS 窗口 host (阶段 2 W2b): 消费 HGS WindowEvent, 用 startAbility
// 为每个 toplevel 建 OHOS 窗口, 并把 title/几何/状态/可见性落到 window API。
//
// 设计 (phase0 §15.5 修订): host.CreateWindow 在 core 侧是唯一创建路径;
// 该服务是事件驱动实现 — Created 建 Ability, 其余事件调 window API。
// IWindowHost (C++ 接口) 留给原生 host (W2c), 两者不要同时启用。
//
// 宿主接入:
//   1. module.json5 注册 HgsAppWindowAbility 子类 (launchType: multiton)
//   2. main_pages.json 注册 pages/HgsWindowPage 包装页
//   3. EntryAbility.onCreate: HgsWindowHostService.init(this.context, {...})
//   4. startServer config: inputBackend=Native

import { window } from '@kit.ArkUI';
import { common, Want } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import hgs from 'libhgs.so';

const TAG = 'HGS_WinHost';

// HgsWindowEventType (与 C++ api/hgs_window_types.h 一致, 勿重排)
const EV_CREATED = 1;
const EV_DESTROYED = 2;
const EV_TITLE_CHANGED = 3;
const EV_GEOMETRY_CHANGED = 4;
const EV_STATE_CHANGED = 5;
const EV_FOCUS_CHANGED = 6;
const EV_VISIBLE_CHANGED = 7;
const EV_CLOSE_REQUESTED = 8;
const EV_MOVE_REQUESTED = 9;
const EV_RESIZE_REQUESTED = 10;
const EV_RAISE_REQUESTED = 15;

// HgsWindowState
const ST_NORMAL = 0;
const ST_MINIMIZED = 1;
const ST_MAXIMIZED = 2;
const ST_FULLSCREEN = 3;

export interface HgsWindowHostOptions {
  abilityName?: string;   // 默认 'HgsAppWindowAbility'
  pageUrl?: string;       // 默认 'pages/HgsWindowPage' (宿主 main_pages.json 注册)
  windowScale?: number;   // 逻辑尺寸 → OHOS 窗口尺寸; 默认 1, BetterSH 可设 2
}

interface WindowSpecLike {
  id: number;
  pid: number;
  appId: string;
  title: string;
  x: number;
  y: number;
  width: number;
  height: number;
  visible: boolean;
  state: number;
}

interface GeometryLike {
  x: number;
  y: number;
  width: number;
  height: number;
}

interface WindowEventLike {
  type: number;
  id: number;
  spec: WindowSpecLike;
  geometry: GeometryLike;
  state: number;
  focused: boolean;
  edges: number;
}

// Want.parameters 的显式接口 — ArkTS 对象字面量必须对应 class/interface
// (实际传给 Want 时用 Record 字面量, 见 createWindow)

interface WindowRecord {
  win: window.Window | null;
  ctx: common.UIAbilityContext | null;
  spec: WindowSpecLike | null;
  lastW: number;
  lastH: number;
}

export class HgsWindowHostService {
  private static instance: HgsWindowHostService | null = null;

  static getInstance(): HgsWindowHostService {
    if (!HgsWindowHostService.instance) {
      HgsWindowHostService.instance = new HgsWindowHostService();
    }
    return HgsWindowHostService.instance;
  }

  /** 宿主 EntryAbility.onCreate 调用 (幂等)。 */
  static init(ctx: common.UIAbilityContext, options?: HgsWindowHostOptions): void {
    HgsWindowHostService.getInstance().init(ctx, options);
  }

  static shutdown(): void {
    HgsWindowHostService.getInstance().windows.clear();
  }

  private ctx: common.UIAbilityContext | null = null;
  private options: HgsWindowHostOptions = {};
  private windows: Map<number, WindowRecord> = new Map();

  private init(ctx: common.UIAbilityContext, options?: HgsWindowHostOptions): void {
    this.ctx = ctx;
    this.options = options ?? {};
    hgs.setWindowCallback((event) => {
      this.onEvent(event as WindowEventLike);
    });
    hgs.LOGI(`[${TAG}] init: ability=${this.abilityName()} page=${this.pageUrl()} scale=${this.scale().toFixed(2)}`);
  }

  abilityName(): string {
    return this.options.abilityName ?? 'HgsAppWindowAbility';
  }

  pageUrl(): string {
    return this.options.pageUrl ?? 'pages/HgsWindowPage';
  }

  private scale(): number {
    const s = this.options.windowScale ?? 1;
    return s > 0 ? s : 1;
  }

  /** HgsAppWindowAbility 在 getMainWindowSync 后调用, 注册实际 window 句柄。 */
  attachWindow(toplevelId: number, win: window.Window, ctx: common.UIAbilityContext): void {
    const rec = this.ensure(toplevelId);
    rec.win = win;
    rec.ctx = ctx;
    if (rec.spec) {
      this.applyTitle(rec, rec.spec.title);
      this.applyGeometry(rec, rec.spec.width, rec.spec.height);
      this.applyState(rec, rec.spec.state);
      this.applyVisible(rec, rec.spec.visible);
    }
    hgs.LOGI(`[${TAG}] attach tl=${toplevelId} win=${win.getWindowProperties().id}`);
  }

  /** HgsAppWindowAbility.onWindowStageDestroy 调用。 */
  detachWindow(toplevelId: number): void {
    this.windows.delete(toplevelId);
    // 用户直接关 OHOS 窗口时, core 仍持有该窗口 → 回执回滚;
    // core 主动销毁时此调用是幂等 no-op。
    hgs.notifyWindowClosed(toplevelId);
    hgs.LOGI(`[${TAG}] detach tl=${toplevelId}`);
  }

  private ensure(id: number): WindowRecord {
    let rec = this.windows.get(id);
    if (!rec) {
      rec = { win: null, ctx: null, spec: null, lastW: 0, lastH: 0 };
      this.windows.set(id, rec);
    }
    return rec;
  }

  private onEvent(ev: WindowEventLike): void {
    const rec = this.windows.get(ev.id);
    switch (ev.type) {
      case EV_CREATED:
        this.createWindow(ev.id, ev.spec);
        break;
      case EV_DESTROYED: {
        const r = this.windows.get(ev.id);
        if (r && r.ctx) {
          r.ctx.terminateSelf().catch((e: BusinessError) => {
            hgs.LOGW(`[${TAG}] terminateSelf tl=${ev.id}: ${JSON.stringify(e)}`);
          });
        }
        this.windows.delete(ev.id);
        break;
      }
      case EV_TITLE_CHANGED:
        if (rec) {
          if (rec.spec) rec.spec.title = ev.spec.title;
          this.applyTitle(rec, ev.spec.title);
        }
        break;
      case EV_GEOMETRY_CHANGED:
        if (rec) {
          if (rec.spec) {
            rec.spec.width = ev.geometry.width;
            rec.spec.height = ev.geometry.height;
          }
          this.applyGeometry(rec, ev.geometry.width, ev.geometry.height);
        }
        break;
      case EV_STATE_CHANGED:
        if (rec) {
          if (rec.spec) rec.spec.state = ev.state;
          this.applyState(rec, ev.state);
        }
        break;
      case EV_VISIBLE_CHANGED:
        if (rec) {
          if (rec.spec) rec.spec.visible = ev.spec.visible;
          this.applyVisible(rec, ev.spec.visible);
        }
        break;
      case EV_RAISE_REQUESTED:
        if (rec && rec.win) {
          rec.win.show().catch((e: BusinessError) => {
            hgs.LOGW(`[${TAG}] show tl=${ev.id}: ${JSON.stringify(e)}`);
          });
        }
        break;
      case EV_CLOSE_REQUESTED:
        if (rec && rec.ctx) {
          rec.ctx.terminateSelf().catch((e: BusinessError) => {
            hgs.LOGW(`[${TAG}] close tl=${ev.id}: ${JSON.stringify(e)}`);
          });
        }
        break;
      case EV_MOVE_REQUESTED:
        if (rec && rec.win) {
          rec.win.startMoving().catch((e: BusinessError) => {
            hgs.LOGW(`[${TAG}] startMoving tl=${ev.id}: ${JSON.stringify(e)}`);
          });
        }
        break;
      case EV_RESIZE_REQUESTED:
        // W2b: OHOS 无 startResize; 客户端的 resize_req 由 W3 跟手会话
        // (或系统窗口边框) 处理。这里只记录, 不做窗口操作。
        hgs.LOGI(`[${TAG}] resize request tl=${ev.id} edges=${ev.edges} (deferred)`);
        break;
      case EV_FOCUS_CHANGED:
        // 系统窗口焦点由 OHOS 管理; 事件用于逻辑 z-order/任务栏, 不强制改窗口
        break;
      default:
        break;
    }
  }

  private createWindow(id: number, spec: WindowSpecLike): void {
    if (!this.ctx) {
      hgs.LOGE(`[${TAG}] create tl=${id} without context`);
      hgs.notifyWindowClosed(id);  // 回滚 core 状态
      return;
    }
    const rec = this.ensure(id);
    rec.spec = spec;
    // 与 BetterSH WaymonyService 同款 Want 参数形式 (ArkTS 接受 Record 字面量)
    const parameters: Record<string, Object> = {
      'toplevelId': id,
      'title': spec.title,
      'w': spec.width,
      'h': spec.height,
    };
    const want: Want = {
      bundleName: this.ctx.abilityInfo.bundleName,
      moduleName: this.ctx.abilityInfo.moduleName,
      abilityName: this.abilityName(),
      parameters: parameters,
    };
    this.ctx.startAbility(want).then(() => {
      hgs.LOGI(`[${TAG}] startAbility tl=${id} ${spec.width}x${spec.height}`);
    }).catch((e: BusinessError) => {
      hgs.LOGE(`[${TAG}] startAbility tl=${id} failed: ${JSON.stringify(e)}`);
      this.windows.delete(id);
      hgs.notifyWindowClosed(id);  // 创建失败 → 回滚 core 状态
    });
  }

  private applyTitle(rec: WindowRecord, title: string): void {
    if (!rec.win || !title) return;
    rec.win.setWindowTitle(title).catch((e: BusinessError) => {
      hgs.LOGW(`[${TAG}] setWindowTitle: ${JSON.stringify(e)}`);
    });
  }

  private applyGeometry(rec: WindowRecord, width: number, height: number): void {
    if (!rec.win || width <= 0 || height <= 0) return;
    const w = Math.max(1, Math.round(width * this.scale()));
    const h = Math.max(1, Math.round(height * this.scale()));
    if (w === rec.lastW && h === rec.lastH) return;  // 去重, 防反馈环
    rec.lastW = w;
    rec.lastH = h;
    rec.win.resize(w, h).catch((e: BusinessError) => {
      hgs.LOGW(`[${TAG}] resize: ${JSON.stringify(e)}`);
    });
  }

  private applyState(rec: WindowRecord, state: number): void {
    if (!rec.win) return;
    if (state === ST_MAXIMIZED) {
      rec.win.maximize().catch((e: BusinessError) => {
        hgs.LOGW(`[${TAG}] maximize: ${JSON.stringify(e)}`);
      });
    } else if (state === ST_MINIMIZED) {
      rec.win.minimize().catch((e: BusinessError) => {
        hgs.LOGW(`[${TAG}] minimize: ${JSON.stringify(e)}`);
      });
    } else if (state === ST_FULLSCREEN) {
      rec.win.setFullScreen(true).catch((e: BusinessError) => {
        hgs.LOGW(`[${TAG}] setFullScreen: ${JSON.stringify(e)}`);
      });
    } else {
      // Normal: 退出全屏 + 还原
      rec.win.setFullScreen(false).catch((): void => { /* ignore */ });
      rec.win.restore().catch((e: BusinessError) => {
        hgs.LOGW(`[${TAG}] restore: ${JSON.stringify(e)}`);
      });
    }
  }

  private applyVisible(rec: WindowRecord, visible: boolean): void {
    if (!rec.win) return;
    if (visible) {
      rec.win.show().catch((e: BusinessError) => {
        hgs.LOGW(`[${TAG}] show: ${JSON.stringify(e)}`);
      });
    } else {
      // 本 SDK 的 Window 无 hide(); 用 minimize 表达"不可见" (与 BetterSH
      // 最小化语义一致; 任务栏/桌面模式由宿主策略决定是否另存窗口)
      rec.win.minimize().catch((e: BusinessError) => {
        hgs.LOGW(`[${TAG}] minimize: ${JSON.stringify(e)}`);
      });
    }
  }
}
