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
//   4. EntryAbility.onCreate: HgsWindowHostService.start({ socketPath, ... })
//      — 封装 hgs.startServer, 默认 inputBackend=Native / PerToplevel /
//      dataDevice=true (可用 options 覆盖)

import { window, display } from '@kit.ArkUI';
import { common, Want } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import hgs, { HgsEndpoint, HgsServiceOptions, HgsServiceWindow, HgsStartConfig,
  HgsWindowControl } from 'libhgs.so';
import { HgsService, HgsSession } from '../api/HgsService';
import { HgsServiceWindowHost } from './HgsServiceWindowHost';

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
  servicePageUrl?: string; // 默认 'pages/HgsServiceWindow' (X11 包装页)
  windowScale?: number;   // 逻辑尺寸 → OHOS 窗口尺寸; 默认 1, BetterSH 可设 2
  decorVisible?: boolean; // OHOS 系统标题栏可见性; 默认 false
  /**
   * 窗口模式选择。
   * false (默认): 系统默认模式 + 系统原生 maximize/restore/setFullScreen —
   *   有系统动画、支持真全屏 (游戏); "启动即最大化"由 pre-show resize 压制
   *   (真机验证: 最大化会话后强停重启仍以普通态打开)。
   * true: stage 声明 FLOATING-only, host 用几何方式 (moveTo+resize) 自管
   *   最大化 — 几何完全确定, 但无系统动画、无法真全屏, 且要求 API 15/20
   *   的 setSupportedWindowModes (不支持时回退系统默认, 此时几何最大化
   *   会被 WMS 吞掉: max→restore→max 第三次失效, 真机实测)。
   */
  floatingOnly?: boolean;
}

// start() 的封装参数: 只暴露窗口 host 场景需要的字段, 其余走 startServer
// 配置对象。默认 Native 输入 + PerToplevel + dataDevice (BetterSH/HarmonyBox
// 的最小接入形态); WineHua 等宿主可显式覆盖。
export interface HgsWindowHostStartOptions {
  socketPath: string;
  outputWidth?: number;        // >0 才下发 (0 = 不修改)
  outputHeight?: number;
  inputBackend?: number;       // InputBackend; 默认 2 (Native)
  composition?: number;        // CompositionMode; 默认 1 (PerToplevel)
  dataDevice?: boolean;        // 默认 true
  winehuaToplevel?: boolean;   // 默认 false
  setProcessEnvironment?: boolean;  // 默认 false (宿主用 ClientEnvironment 传子进程)
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
  state: number;     // 当前 HgsWindowState (最大化/全屏时几何由状态决定)
  normalW: number;   // 最近一次 Normal 态的客户端内容宽 (逻辑 px)
  normalH: number;
  preMaxX: number;   // 最大化前的窗口左上角 (物理 px, 还原用)
  preMaxY: number;
  hasPreMax: boolean;
  hasStateBeenMax: boolean;  // 系统模式下是否进过最大化 (Normal→restore 的依据)
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

  /**
   * 启动内嵌 Wayland server 的封装 (须先 init)。
   * 默认: inputBackend=Native / composition=PerToplevel / dataDevice=true /
   * setProcessEnvironment=false — 即窗口 host 的最小可用配置;
   * 传入 string 时等价于只给 socketPath (其余用默认)。
   */
  static start(options: string | HgsWindowHostStartOptions): boolean {
    return HgsWindowHostService.getInstance().start(options);
  }

  /** Start the selected listeners and attach their per-window presentation to this stage. */
  static async startService(stage: window.WindowStage, options: HgsServiceOptions): Promise<HgsSession> {
    return HgsWindowHostService.getInstance().startService(stage, options);
  }

  static async stopService(): Promise<void> {
    const host = HgsWindowHostService.getInstance();
    await HgsService.create().stop();
    host.serviceHost?.stop();
    host.serviceHost = null;
    host.serviceSession = null;
    host.serviceStage = null;
  }

  static shutdown(): void {
    HgsWindowHostService.getInstance().windows.clear();
  }

  private ctx: common.UIAbilityContext | null = null;
  private options: HgsWindowHostOptions = {};
  private windows: Map<number, WindowRecord> = new Map();
  private serviceSession: HgsSession | null = null;
  private serviceHost: HgsServiceWindowHost | null = null;
  private serviceStage: window.WindowStage | null = null;

  private init(ctx: common.UIAbilityContext, options?: HgsWindowHostOptions): void {
    this.ctx = ctx;
    this.options = options ?? {};
    hgs.setWindowCallback((event) => {
      this.onEvent(event as WindowEventLike);
    });
    hgs.LOGI(`[${TAG}] init: ability=${this.abilityName()} page=${this.pageUrl()} scale=${this.scale().toFixed(2)}`);
  }

  private async startService(stage: window.WindowStage, options: HgsServiceOptions): Promise<HgsSession> {
    if (!this.ctx) throw new Error('HgsWindowHostService.init must precede startService');
    if (options.composition === 'desktop-root') throw new Error('The default window host requires per-window composition');
    const config: HgsServiceOptions = {
      runtimeDir: options.runtimeDir,
      protocols: options.protocols ?? 'wayland',
      composition: 'per-window',
      inputBackend: options.inputBackend ?? 'native',
      setProcessEnvironment: options.setProcessEnvironment ?? false,
    };
    if (options.wayland !== undefined) config.wayland = options.wayland;
    if (options.x11 !== undefined) config.x11 = options.x11;
    if (options.output !== undefined) config.output = options.output;
    const session = await HgsService.create().start(config);
    if (this.serviceSession?.epoch === session.epoch && this.serviceStage === stage) return session;
    this.serviceHost?.stop();
    this.serviceHost = null;
    this.serviceStage = stage;
    this.serviceSession = session;
    if (session.endpoints.some((endpoint: HgsEndpoint): boolean => endpoint.protocol === 'x11')) {
      this.serviceHost = new HgsServiceWindowHost(session, stage,
        this.options.servicePageUrl ?? 'pages/HgsServiceWindow', this.scale());
      this.serviceHost.start();
    }
    return session;
  }

  /**
   * 启动 Wayland server (封装 hgs.startServer)。
   * 须先 init: 窗口事件要落到 UIAbilityContext 才能 startAbility; 未 init
   * 直接失败, 避免 server 起来但没有窗口承载。
   */
  start(options: string | HgsWindowHostStartOptions): boolean {
    if (!this.ctx) {
      hgs.LOGE(`[${TAG}] start before init — 先调用 HgsWindowHostService.init(ctx)`);
      return false;
    }
    const opts: HgsWindowHostStartOptions =
      typeof options === 'string' ? { socketPath: options } : options;

    const config: HgsStartConfig = {
      socketPath: opts.socketPath,
      composition: opts.composition ?? 1,    // PerToplevel
      inputBackend: opts.inputBackend ?? 2,  // Native
      features: {
        dataDevice: opts.dataDevice ?? true,
        winehuaToplevel: opts.winehuaToplevel ?? false,
      },
      setProcessEnvironment: opts.setProcessEnvironment ?? false,
    };
    const w = opts.outputWidth ?? 0;
    const h = opts.outputHeight ?? 0;
    if (w > 0 && h > 0) {
      config.output = { width: w, height: h };
    }

    const ok = hgs.startServer(config);
    if (ok) {
      hgs.LOGI(`[${TAG}] startServer OK socket=${opts.socketPath} backend=${config.inputBackend} composition=${config.composition}`);
    } else {
      hgs.LOGE(`[${TAG}] startServer FAILED socket=${opts.socketPath}`);
    }
    return ok;
  }

  abilityName(): string {
    return this.options.abilityName ?? 'HgsAppWindowAbility';
  }

  pageUrl(): string {
    return this.options.pageUrl ?? 'pages/HgsWindowPage';
  }

  /** 逻辑尺寸 → OHOS 窗口尺寸缩放 (HgsAppWindowAbility 显示前 resize 用) */
  windowScale(): number {
    const s = this.options.windowScale ?? 1;
    return s > 0 ? s : 1;
  }

  ownsServiceWaylandWindow(toplevelId: number, epoch: string): boolean {
    return this.serviceSession?.epoch === epoch && this.windows.has(toplevelId);
  }

  /** 系统默认模式 (false) 或 FLOATING-only 几何自管 (true); 默认 false */
  floatingOnly(): boolean {
    return this.options.floatingOnly ?? false;
  }

  private scale(): number {
    return this.windowScale();
  }

  /** HgsAppWindowAbility 在 getMainWindowSync 后调用, 注册实际 window 句柄。 */
  attachWindow(toplevelId: number, win: window.Window, ctx: common.UIAbilityContext): void {
    const rec = this.ensure(toplevelId);
    rec.win = win;
    rec.ctx = ctx;
    if (this.serviceSession) this.observeWaylandHost(toplevelId, rec, win);
    if (rec.spec) {
      rec.state = rec.spec.state;
      if (rec.spec.state === ST_NORMAL) {
        rec.normalW = rec.spec.width;
        rec.normalH = rec.spec.height;
      }
    }
    const applyInitial = (): void => {
      if (!rec.spec || !rec.win) return;
      this.applyTitle(rec, rec.spec.title);
      // 顺序红线: 先恢复 Normal 态再 resize — 系统会记住 Ability 上次的
      // 最大化布局, 直接 resize 会被最大化态吞掉 (窗口"一开始就最大化")
      this.applyState(rec, rec.state);
      // 最大化/全屏时尺寸已由 applyState 定好; 再按 spec 内容尺寸 resize
      // 会把窗口从整屏缩回去 (与几何事件同理)
      if (rec.state === ST_NORMAL) {
        this.applyGeometry(rec, rec.spec.width, rec.spec.height);
      }
      this.applyVisible(rec, rec.spec.visible);
      // 隐藏 OHOS 系统标题栏 (wayland 客户端自绘装饰; API20+, 不支持则忽略)
      try {
        rec.win.setWindowDecorVisible(this.options.decorVisible ?? false);
      } catch (e) {
        hgs.LOGW(`[${TAG}] setWindowDecorVisible: ${JSON.stringify(e)}`);
      }
      try {
        if (this.options.decorVisible) {
          win.setWindowTitleButtonVisible(true, true, true);
        } else {
          win.setWindowTitleButtonVisible(false, false, false);
        }
      } catch (e) {
        hgs.LOGW(`[${TAG}] setWindowTitleButtonVisible: ${JSON.stringify(e)}`);
      }
    };
    applyInitial();
    // 窗口刚 attach 时处于状态迁移期, title/resize/decor 可能报 1300002
    // (window state abnormal) — 延迟重试一次 (applyGeometry 有 lastW/H 去重,
    // 重复调用不会形成反馈环)
    setTimeout((): void => {
      applyInitial();
    }, 300);
    hgs.LOGI(`[${TAG}] attach tl=${toplevelId} win=${win.getWindowProperties().id}`);
  }

  private observeWaylandHost(id: number, record: WindowRecord, host: window.Window): void {
    const session = this.serviceSession;
    if (!session) return;
    const control = (request: HgsWindowControl): boolean => {
      if (this.serviceSession !== session || this.windows.get(id) !== record || record.win !== host) return false;
      const snapshot = session.windows().find((entry: HgsServiceWindow): boolean =>
        entry.protocol === 'wayland' && entry.legacyId === id);
      return snapshot !== undefined && session.controlWindow(snapshot.id, request);
    };
    host.on('windowEvent', (event: window.WindowEventType): void => {
      if (event === window.WindowEventType.WINDOW_ACTIVE) control({ kind: 'focus' });
      else if (event === window.WindowEventType.WINDOW_INACTIVE) control({ kind: 'blur' });
    });
    host.on('windowWillClose', async (): Promise<boolean> => control({ kind: 'close' }));
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
      rec = {
        win: null, ctx: null, spec: null, lastW: 0, lastH: 0,
        state: ST_NORMAL, normalW: 0, normalH: 0,
        preMaxX: 0, preMaxY: 0, hasPreMax: false, hasStateBeenMax: false,
      };
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
          // 最大化/全屏时窗口尺寸由状态决定: 客户端收到 configure 后的内容
          // 尺寸只改变渲染拉伸; 若照它 resize, 最大化会被缩到 configure 尺寸
          // (实测 2880x1920 → 2560x1440), 还原又变成"点了没反应"
          if (rec.state === ST_MAXIMIZED || rec.state === ST_FULLSCREEN) break;
          rec.normalW = ev.geometry.width;
          rec.normalH = ev.geometry.height;
          // 系统模式 (floatingOnly=false): 窗口尺寸的驱动方是 WMS — 最大化/
          // 还原/手动拖拽都会改窗口, 客户端内容尺寸只是结果 (XComponent 变
          // 化 → 我们发 configure)。若在这里照结果 resize, 就成反馈环: 还原
          // 动画的中间矩形被当成客户端请求再乘 scale 写回, 窗口会钉死在
          // 2220x1900 (真机实测) 而不是回到 960x814。
          // 几何模式反过来: 客户端尺寸就是驱动方。
          if (this.floatingOnly()) {
            this.applyGeometry(rec, ev.geometry.width, ev.geometry.height);
          }
        }
        break;
      case EV_STATE_CHANGED:
        if (rec) {
          rec.state = ev.state;
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
    if (this.serviceSession) parameters['hgsServiceEpoch'] = this.serviceSession.epoch;
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

  private logRect(rec: WindowRecord, what: string): void {
    if (!rec.win) return;
    try {
      const r = rec.win.getWindowProperties().windowRect;
      hgs.LOGI(`[${TAG}] ${what}: rect=${r.width}x${r.height}@(${r.left},${r.top}) state=${rec.state}`);
    } catch (e) { /* 忽略 */ }
  }

  private applyGeometry(rec: WindowRecord, width: number, height: number): void {
    if (!rec.win || width <= 0 || height <= 0) return;
    const w = Math.max(1, Math.round(width * this.scale()));
    const h = Math.max(1, Math.round(height * this.scale()));
    if (w === rec.lastW && h === rec.lastH) return;  // 去重, 防反馈环
    rec.win.resize(w, h).then((): void => {
      // 成功才记录 — 失败 (如窗口迁移期 1300002) 留待重试再发
      rec.lastW = w;
      rec.lastH = h;
      // resize 的入参单位在 SDK 文档里是 px, 但真机是否有 vp 解释取决于
      // 系统版本 — 这里回读实际矩形, 单位之争以它为准
      try {
        const r = rec.win!.getWindowProperties().windowRect;
        hgs.LOGI(`[${TAG}] resize req=${w}x${h} rect=${r.width}x${r.height}@(${r.left},${r.top})`);
      } catch (e) { /* 忽略 */ }
    }).catch((e: BusinessError) => {
      rec.lastW = 0;
      rec.lastH = 0;
      hgs.LOGW(`[${TAG}] resize: ${JSON.stringify(e)}`);
    });
  }

  private applyState(rec: WindowRecord, state: number): void {
    if (!rec.win) return;
    // 最大化/还原统一走**几何方式** (moveTo + resize), 两种窗口模式都一样。
    // 系统原生 maximize() 在这台设备上是死路 (真机实测):
    //   1. maximize 之后 resize 永久返回 1300002 — 窗口不再是 freeform;
    //   2. restore() 只退出"最大化态", 不恢复矩形: 窗口停在最大化期间的
    //      拉伸尺寸 (~2220x1900), ArkUI 根节点仍是 2880x1920, 内容被裁切。
    // 几何方式下 resize 始终可用, 客户端通过 configure 同步尺寸。
    if (state === ST_MAXIMIZED) {
      rec.hasStateBeenMax = true;
      // 记下最大化前的窗口矩形, 还原时回到原位 (moveTo(0,0) 会把它改掉)。
      // hasPreMax 防重入: attach 后的延迟重试再走一遍时不能把已最大化的
      // 矩形当成"原位"
      if (!rec.hasPreMax) {
        try {
          const r = rec.win.getWindowProperties().windowRect;
          rec.preMaxX = r.left;
          rec.preMaxY = r.top;
          rec.hasPreMax = true;
        } catch (e) {
          hgs.LOGW(`[${TAG}] read rect before maximize: ${JSON.stringify(e)}`);
        }
      }
      try {
        const d = display.getDefaultDisplaySync();
        rec.win.moveTo(0, 0).catch((e: BusinessError) => {
          hgs.LOGW(`[${TAG}] moveTo(0,0): ${JSON.stringify(e)}`);
        });
        rec.win.resize(d.width, d.height).then((): void => {
          rec.lastW = d.width;
          rec.lastH = d.height;
        }).catch((e: BusinessError) => {
          // 失败不记 lastW/H — 留待几何事件或重试再发
          rec.lastW = 0;
          rec.lastH = 0;
          hgs.LOGW(`[${TAG}] maximize resize: ${JSON.stringify(e)}`);
        });
      } catch (e) {
        hgs.LOGW(`[${TAG}] maximize: ${JSON.stringify(e)}`);
      }
      return;
    }
    if (state === ST_NORMAL && rec.hasStateBeenMax && rec.hasPreMax) {
      // 还原: 回到最大化前的位置 + 最近一次 Normal 内容尺寸
      // (spec.width/height 已被最大化期间的 configure 尺寸污染, 不能用)
      rec.win.moveTo(rec.preMaxX, rec.preMaxY).catch((e: BusinessError) => {
        hgs.LOGW(`[${TAG}] restore moveTo: ${JSON.stringify(e)}`);
      });
      rec.hasPreMax = false;
      const w = rec.normalW > 0 ? rec.normalW : (rec.spec?.width ?? 0);
      const h = rec.normalH > 0 ? rec.normalH : (rec.spec?.height ?? 0);
      rec.lastW = 0;  // 最大化期间 WMS 改过矩形, 绕过去重
      rec.lastH = 0;
      this.applyGeometry(rec, w, h);
      return;
    }
    if (state === ST_MINIMIZED) {
      rec.win.minimize().catch((e: BusinessError) => {
        hgs.LOGW(`[${TAG}] minimize: ${JSON.stringify(e)}`);
      });
    } else if (state === ST_FULLSCREEN) {
      rec.win.setFullScreen(true).catch((e: BusinessError) => {
        hgs.LOGW(`[${TAG}] setFullScreen: ${JSON.stringify(e)}`);
      });
    } else {
      // Normal 且没有最大化前矩形 (例如首次进入 Normal): 只同步内容尺寸
      const w = rec.normalW > 0 ? rec.normalW : (rec.spec?.width ?? 0);
      const h = rec.normalH > 0 ? rec.normalH : (rec.spec?.height ?? 0);
      this.applyGeometry(rec, w, h);
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
