// surface 生命周期由 NativeXComponent 回调在原生层驱动
// (XComponent libraryname:'hgs' → OH_NATIVE_XCOMPONENT_OBJ),
// 不再有 create/destroy 导出; 非 Init 触发的 XComponent (同进程
// 第二个窗口) 经此绑定回调。
export const bindXComponent: (xcomp: object) => void;

// ── 自 entry libentry.so 迁入的合成器/输入/渲染器 NAPI 面 (方案 B1b) ──
export const setStateCallback: (cb: (state: string) => void) => void;
export const startServer: (sockPath: string) => boolean;
export const setToplevelCallback: (cb: (id: number, event: string, data: string) => void) => void;
export const setImeCallback: (cb: (active: number, x: number, y: number, w: number, h: number) => void) => void;
export const sendImeCommit: (text: string) => void;
export const sendImePreedit: (text: string, start: number, end: number) => void;
export const imeBackspace: () => void;
export const getCurrentToplevelId: () => number;
export const setPendingToplevel: (id: number) => void;
export const cancelPendingToplevel: (id: number) => void;
export const destroyToplevel: (id: number) => void;
export const sendToplevelClose: (id: number) => void;
export const createRenderer: (toplevelId: number, surfaceId: BigInt) => void;
export const resizeRenderer: (toplevelId: number, width: number, height: number) => void;
export const destroyRenderer: (toplevelId: number) => void;
export const setOutputSize: (w: number, h: number) => void;
export const setDisplayScale: (scale: number) => void;
export const setDesktopMode: (enabled: boolean) => void;
export const sendPointerEvent: (toplevelId: number, action: number, px: number, py: number, button: number, rawDeltaX?: number, rawDeltaY?: number, fromMouse?: boolean) => void;
export const sendKeyEvent: (toplevelId: number, evdevCode: number, pressed: boolean) => void;
export const sendScrollEvent: (toplevelId: number, axis: number, value: number, scrollStep: number, px: number, py: number) => void;
export const registerHostWindow: (windowId: number) => void;
export const dumpWindowLayout: (displayId?: number) => void;
export const setPointerLockCallback: (cb: (locked: boolean, toplevelId: number) => void) => void;
export const notifyToplevelResize: (toplevelId: number, w: number, h: number, resizing?: boolean) => void;
export const notifyToplevelResizeEnd: (toplevelId: number) => void;
export const findToplevelAt: (px: number, py: number) => number;
export const raiseToplevel: (toplevelId: number) => void;
export const setToplevelVisible: (toplevelId: number, visible: boolean) => void;
