// surface 生命周期由 NativeXComponent 回调在原生层驱动
// (XComponent libraryname:'hgs' → OH_NATIVE_XCOMPONENT_OBJ),
// 不再有 create/destroy 导出; 非 Init 触发的 XComponent (同进程
// 第二个窗口) 经此绑定回调。
export const bindXComponent: (xcomp: object) => void;
