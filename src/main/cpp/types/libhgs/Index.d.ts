// ============================================================================
// libhgs.so NAPI 导出声明 (阶段 1 P1.1, 对齐 docs/architecture/hgs-exported-api.md)
//
// 稳定性分组: Stable / @experimental (Wine 或手柄扩展) / @deprecated / Internal。
// 旧名 (createRenderer 等) 运行时仍可用并转发新实现, 首次调用打
// [HGS-DEPRECATED] 日志, 阶段 3 删除。
//
// 枚举数值契约 (单一来源 = C++ api/hgs_types.h / api/hgs_events.h, 勿重排):
//   CompositionMode:    0=Unspecified 1=PerToplevel 2=DesktopRoot
//   InputBackend:       0=Auto 1=ArkTS 2=Native
//   PointerAction:      1=Press 2=Release 3=Move
//   ToplevelEventType:  0=Unknown 1=Created 2=Destroyed 3=PopupHide 4=PopupMove
//     5=PopupShow 6=PopupResize 7=Argb 8=Resize 9=Surface 10=Title 11=Limits
//     12=Maximized 13=Unmaximized 14=Fullscreen 15=Unfullscreen 16=Minimized
//     17=Restored 18=MoveStart 19=MoveEnd 20=Modal 21=DesktopRoot
// ============================================================================

// ── startServer 配置对象 (Stable) ──────────────────────────────────────────
// 字段缺省 / 0 = 不修改宿主现状 (output 尺寸、composition 等)。
export interface HgsFeatureConfig {
  shm?: boolean;
  subsurface?: boolean;
  viewporter?: boolean;
  xdgPopup?: boolean;
  dataDevice?: boolean;       // wl_data_device_manager (GTK seat/clipboard)
  relativePointer?: boolean;  // zwp_relative_pointer/warp/constraints (Wine)
  textInput?: boolean;        // zwp_text_input_v3 (IME)
  externalSurface?: boolean;  // zero-copy / VirGL / Venus provider
  winehuaToplevel?: boolean;  // WineHua 专用协议
}

export interface HgsOutputConfig {
  width?: number;    // 0 = 不修改
  height?: number;
  scale?: number;    // 0 = 不修改
  refreshMilliHz?: number;
}

export interface HgsStartConfig {
  socketPath: string;             // 绝对路径且带 basename
  output?: HgsOutputConfig;
  composition?: number;           // CompositionMode
  inputBackend?: number;          // InputBackend
  features?: HgsFeatureConfig;
  setProcessEnvironment?: boolean; // 默认 true = 沿用 setenv 旧行为
}

// ── surface (XComponent 生命周期, Stable) ─────────────────────────────────
// surface 生命周期由 NativeXComponent 回调在原生层驱动 (XComponent
// libraryname:'hgs' → OH_NATIVE_XCOMPONENT_OBJ)。
// 页面 onLoad 传本窗口 toplevelId: 下一个/已存在的未绑定 surface 认领它,
// 绑定原生输入 + 原生 renderer (backend=Native 时生效)。
// (W2b 改签名: 旧 (xcomp, toplevelId?) 依赖 onLoad 事件 napi_unwrap,
//  本 SDK 实测 unwrap failed — 见 graphics/egl_surface.cpp 注释)
export const bindXComponent: (toplevelId: number) => void;
/**
 * 多窗口兜底: 用 ArkTS XComponentController 的 surfaceId 建渲染器。
 * 同进程第二个窗口起, 原生 OnSurfaceCreated 早于 HGS 的回调注册触发 (事件
 * 丢失), native 路径拿不到 OHNativeWindow — 由 controller 的
 * onSurfaceCreated 调这里补建。原生路径已建过 renderer 时是 no-op (返回 true)。
 * HgsSurface 在 inputBackend=Native 时自动调用。
 */
export const attachSurfaceFallback: (toplevelId: number, surfaceId: bigint) => boolean;
/**
 * 重试 OH_NativeXComponent 回调注册 (napi register 函数只在 import 时跑一次,
 * 那时 XComponent 还没创建; onLoad 时刻重试 unwrap 才能拿到原生对象)。
 * HgsSurface.onLoad 自动调用; 返回 true = 已注册。
 */
export const regXComponentCallback: (force?: boolean) => boolean;

// attachSurface 返回 generation (0 = 失败) — 后续 resize/detach 回传以
// 防旧窗口事件误杀新 renderer; 传 0 = 通配 (兼容旧调用)。
export const attachSurface: (toplevelId: number, surfaceId: bigint) => number;
export const resizeSurface: (toplevelId: number, width: number, height: number, generation?: number) => boolean;
export const detachSurface: (toplevelId: number, generation?: number) => boolean;

/** @deprecated 用 attachSurface；返回值忽略 generation。阶段 3 删除。 */
export const createRenderer: (toplevelId: number, surfaceId: BigInt) => void;
/** @deprecated 用 resizeSurface(id, w, h)。阶段 3 删除。 */
export const resizeRenderer: (toplevelId: number, width: number, height: number) => void;
/** @deprecated 用 detachSurface(id)。阶段 3 删除。 */
export const destroyRenderer: (toplevelId: number) => void;

// ── runtime (Stable) ──────────────────────────────────────────────────────
// startServer 同时接受 string (旧调用, 其余配置缺省) 与 HgsConfig 对象。
export const startServer: (config: string | HgsStartConfig) => boolean;
export const stopServer: () => void;
export const isRunning: () => boolean;
export const getWaylandDisplayName: () => string;   // 未启动/无效路径返回空串
export const getClientEnvironment: () => string[];  // ["XDG_RUNTIME_DIR=...", "WAYLAND_DISPLAY=..."]
export const getApiVersion: () => number;           // major<<16 | minor
export const getLastError: () => string;            // 最近一次失败原因, 空串 = 无错误
export const setOutputSize: (w: number, h: number) => void;
/** @deprecated 长期 no-op；阶段 2 由 HgsStartConfig.output.scale 取代。 */
export const setDisplayScale: (scale: number) => void;
export const setDesktopMode: (enabled: boolean) => void;
export const getDesktopRootId: () => number;
export const setInputBackend: (mode: number) => void;  // InputBackend
export const getInputBackend: () => number;

// ── 事件回调 (Stable) ─────────────────────────────────────────────────────
export const setStateCallback: (cb: (state: string) => void) => void;
// typed 契约: type = ToplevelEventType 数值 (见文件头), json 原样透传。
export const setToplevelEventCallback: (cb: (id: number, type: number, json: string) => void) => void;
// 字符串契约 (旧): 与 setToplevelEventCallback 互斥 — 后注册者覆盖前者。
export const setToplevelCallback: (cb: (id: number, event: string, data: string) => void) => void;

// ── toplevel (Stable) ─────────────────────────────────────────────────────
export const takePendingToplevel: () => number;  // 出队; 空 → 0
/** @deprecated 用 takePendingToplevel (原名实为出队语义)。阶段 3 删除。 */
export const getCurrentToplevelId: () => number;
export const setPendingToplevel: (id: number) => void;
export const cancelPendingToplevel: (id: number) => void;
export const destroyToplevel: (id: number) => void;  // 本地收尾 (renderer + pointer lock 兜底)
export const closeToplevel: (id: number) => void;    // xdg_toplevel.send_close
/** @deprecated 用 closeToplevel。阶段 3 删除。 */
export const sendToplevelClose: (id: number) => void;
export const notifyToplevelResize: (toplevelId: number, w: number, h: number, resizing?: boolean) => void;
/** @deprecated 用 notifyToplevelResize(id, 0, 0, false)。阶段 3 删除。 */
export const notifyToplevelResizeEnd: (toplevelId: number) => void;
export const findToplevelAt: (px: number, py: number) => number;
export const raiseToplevel: (toplevelId: number) => void;
export const setToplevelVisible: (toplevelId: number, visible: boolean) => void;

// ── 输入转发 (Stable) ─────────────────────────────────────────────────────
// action: PointerAction (1=Press 2=Release 3=Move); axis: 0=垂直 1=水平
export const sendPointerEvent: (toplevelId: number, action: number, px: number, py: number, button: number, rawDeltaX?: number, rawDeltaY?: number, fromMouse?: boolean) => void;
export const sendKeyEvent: (toplevelId: number, evdevCode: number, pressed: boolean) => void;
export const sendScrollEvent: (toplevelId: number, axis: number, value: number, scrollStep: number, px: number, py: number) => void;

// ── 窗口管理器 (阶段 2 W2a, Stable) ──────────────────────────────────────
export enum HgsWindowState { Normal = 0, Minimized = 1, Maximized = 2, Fullscreen = 3 }

export enum HgsWindowEventType {
  Unknown = 0,
  Created = 1, Destroyed = 2, TitleChanged = 3,
  GeometryChanged = 4, StateChanged = 5, FocusChanged = 6,
  VisibleChanged = 7, CloseRequested = 8,
  MoveRequested = 9, ResizeRequested = 10,
  PopupShown = 11, PopupMoved = 12, PopupResized = 13, PopupHidden = 14,
  RaiseRequested = 15,
}

export interface HgsWindowSpec {
  id: number;
  pid: number;
  appId: string;
  title: string;
  // 客户端内容几何 (不是 OHOS 窗口尺寸; host 按 windowScale 换算)
  x: number; y: number; width: number; height: number;
  minWidth: number; minHeight: number;
  maxWidth: number; maxHeight: number;
  parentId: number;
  modal: boolean;
  decorated: boolean;
  visible: boolean;
  state: HgsWindowState;
}

export interface HgsWindowGeometry { x: number; y: number; width: number; height: number; }

export interface HgsWindowEvent {
  type: HgsWindowEventType;
  id: number;
  spec: HgsWindowSpec;
  geometry: HgsWindowGeometry;
  state: HgsWindowState;
  focused: boolean;
  edges: number;         // ResizeRequested (xdg_resize_edge 位图)
}

// 事件驱动 host: Created 建窗口 / TitleChanged 设标题 / GeometryChanged 调整 /
// StateChanged 最大化还原 / RaiseRequested 带到前台 / CloseRequested 关闭。
export const setWindowCallback: (cb: (event: HgsWindowEvent) => void) => void;
// host 回执 (窗口系统实际变化后调用, 防 core 状态漂移)
export const notifyWindowGeometry: (id: number, x: number, y: number, w: number, h: number) => void;
export const notifyWindowFocus: (id: number, focused: boolean) => void;
export const notifyWindowState: (id: number, state: HgsWindowState) => void;
export const notifyWindowClosed: (id: number) => void;   // 创建失败/系统关窗回滚
// 查询快照
export const getWindow: (id: number) => HgsWindowSpec | null;
export const listWindows: () => HgsWindowSpec[];
export const getWindowZOrder: () => number[];

// ── Wine 扩展 (@experimental — BetterSH/HarmonyBox 不要使用) ─────────────
export const setPointerLockCallback: (cb: (locked: boolean, toplevelId: number) => void) => void;
export const registerHostWindow: (windowId: number) => void;
export const setImeCallback: (cb: (active: number, x: number, y: number, w: number, h: number) => void) => void;
export const sendImeCommit: (text: string) => void;
export const sendImePreedit: (text: string, start: number, end: number) => void;
export const imeBackspace: () => void;

// ── Gamepad/Controller (@experimental, 手柄扩展; 签名来自 C++ 实现) ──────
export const initGameController: () => number;      // 物理手柄初始化结果
export const cleanupGameController: () => void;
export const isGamepadConnected: () => boolean;
export const getGamepadCount: () => number;
export const setGamepadButtonCallback: (cb: (code: number, pressed: boolean) => void) => void;
export const setGamepadAxisCallback: (cb: (type: number, x: number, y: number) => void) => void;
export const setGamepadDeviceCallback: (cb: (connected: boolean) => void) => void;
export const setGamepadRumbleCallback: (cb: (low: number, high: number, durationMs: number) => void) => void;
export const controllerSetEnabled: (enabled: boolean) => void;
export const controllerSetButton: (source: number, slot: number, button: number, pressed: boolean) => void;
export const controllerSetAxis: (source: number, slot: number, axis: number, value: number) => void;
export const controllerSetHat: (source: number, slot: number, x: number, y: number) => void;
export const controllerResetSource: (source: number) => void;
export const controllerGetState: (slot: number) => object;      // 按键/轴状态对象 (调试用)
export const controllerGetStateText: (slot: number) => string;  // 状态文本 (调试用)
export const controllerStartBridge: (path?: string) => boolean; // 手柄桥 socket
export const controllerStopBridge: () => void;
export const controllerGetSocketPath: () => string;
export const controllerSetOutputMode: (mode: string) => void;
export const controllerGetOutputMode: () => string;

// ── 日志 (logger/logger.cpp: stderr/沙箱文件/hilog 多 sink, Stable) ──────
// tag 语义由调用方拼进 msg (如 "[CLICK-PIPE] ...")
export const LOGI: (msg: string) => void;
export const LOGW: (msg: string) => void;
export const LOGE: (msg: string) => void;

// ── 诊断 (Internal — 产品代码不要依赖) ───────────────────────────────────
export const dumpWindowLayout: (displayId?: number) => void;
