#pragma once
// ============================================================================
// hgs_instance.h — HgsInstance 公共门面 (阶段 0, P0.3)。
//
// 出处: docs/architecture/hgs-shared-runtime-phase0.md §4.6/§4.7 与
//       docs/architecture/hgs-exported-api.md §3.3。
//
// 过渡形态: 阶段 0 复用现有单例 (WaylandServer/PluginManager/InputManager/
// PointerExtras), 本类只做契约化转发 + generation 表 + host 适配;
// 阶段 1 状态收口后演进为真正持有实例状态 (phase0 §12)。
// 头文件保持零平台依赖 (仅标准库); 平台/单例依赖只在本头对应的
// hgs_instance.cpp 实现。
//
// 线程契约 (各方法标注处为约定, 阶段 0 无运行时校验):
//   - 主线程/UI 线程: Start/Stop/SetHost/SetOutputSize/SetDesktopMode/
//     Surface 生命周期/宿主窗口操作/pending 队列
//   - NAPI/JS 线程或 native UI 线程: Send* (内部入队, Wayland 线程注入)
//   - IHgsHost 回调在 Wayland 线程触发; 禁止回调内调 Stop() (join 自身)
// ============================================================================

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "hgs_export.h"
#include "hgs_host.h"
#include "hgs_types.h"

namespace hgs {

class HGS_API HgsInstance {
public:
    // 过渡形态: 进程级单例 (函数内 static, 无加载期构造)。
    // 阶段 1 状态收口后引入 Create(config) 真正多实例, 届时弃用本入口。
    static HgsInstance& Instance();

    static constexpr uint32_t ApiVersion() { return HGS_API_VERSION; }

    HgsInstance(const HgsInstance&) = delete;
    HgsInstance& operator=(const HgsInstance&) = delete;

    // -- 生命周期 (主线程/UI 线程) --
    // 顺序: 校验 socketPath → 安装 host 适配 sink → 按 config 下发
    // output/composition (仅非零/非 Unspecified 才下发, 防覆盖宿主预设置)
    // → WaylandServer::Start。
    bool Start(const HgsConfig& config);
    // 幂等; join Wayland 线程。禁止在 IHgsHost 回调内调用。
    void Stop();
    bool IsRunning() const;

    // 最近一次失败原因 (线程安全)。失败路径写入; 成功不清空 (避免误导,
    // hgs-exported-api.md §9)。
    std::string LastError() const;

    std::string WaylandDisplayName() const;              // 无效/未启动返回空
    std::vector<std::string> ClientEnvironment() const;  // XDG_RUNTIME_DIR/WAYLAND_DISPLAY

    // 宿主回调 (主线程, Start 前后均可; host 须活到 Stop 之后, 不接管所有权)。
    // 阶段 1 起 bus 单 sink 由 Start 唯一安装, 本入口是宿主接事件的标准通道
    // (NAPI 层经 NapiHostAdapter 注册; 后注册者覆盖前者)。
    void SetHost(IHgsHost* host);

    // -- 会话配置 (主线程) --
    void SetOutputSize(int32_t w, int32_t h);
    void SetDesktopMode(bool on);
    // 输入后端 (阶段 0 仅记录 Impl 字段; 阶段 1 生效, phase0 §14.6)
    void SetInputBackend(InputBackend backend);
    InputBackend GetInputBackend() const;
    ToplevelId DesktopRootId() const;   // 未出现返回 0

    // -- Surface 生命周期 (UI 线程) --
    // 绑定 XComponent surfaceId (ArkTS controller 路径)。成功返回分配的
    // generation; 失败返回 generation==0。
    // 原生 XComponent 路径 (inputBackend=Native) 不经此入口 —
    // OnSurfaceCreated 直接持 OHNativeWindow*, 由 HGS 内部绑定 (phase0 §14.5)。
    SurfaceGeneration AttachSurface(ToplevelId id, SurfaceToken token);
    // generation 校验: 0 = 通配 (兼容旧 NAPI 路径); 非 0 须与 AttachSurface
    // 返回值一致, 不匹配视为 stale 丢弃 (防旧窗口销毁事件误杀新 renderer)。
    bool ResizeSurface(ToplevelId id, int32_t w, int32_t h, SurfaceGeneration gen = 0);
    // 只释放 surface/renderer, 不触碰 Wayland toplevel 与 pointer lock
    // (= 现 destroyRenderer 语义; toplevel 收尾用 DestroyToplevel)。
    bool DetachSurface(ToplevelId id, SurfaceGeneration gen = 0);

    // -- 输入 (NAPI/JS 或 native UI 线程; 入队后由 Wayland 线程注入) --
    void SendPointerEvent(const PointerEvent& event);
    void SendKeyEvent(const KeyEvent& event);
    void SendScrollEvent(const ScrollEvent& event);

    // -- 宿主窗口操作 (主线程) --
    // 鸿蒙侧 surface 尺寸变化 → configure 通知客户端 (resizing=拖拽中)。
    void NotifyToplevelResize(ToplevelId id, int32_t w, int32_t h, bool resizing);
    void SendToplevelClose(ToplevelId id);        // xdg_toplevel.send_close
    void RaiseToplevel(ToplevelId id);            // userInitiated=true (现 NAPI 语义)
    void SetToplevelVisible(ToplevelId id, bool visible);  // visible=true 附带 restore
    // Wayland toplevel 本地收尾: renderer + pointer lock 兜底释放
    // (= 现 destroyToplevel 语义; 不发 send_close, 那是 SendToplevelClose)。
    void DestroyToplevel(ToplevelId id);
    ToplevelId FindToplevelAt(int32_t x, int32_t y) const;

    // -- 过渡兼容: pending 队列 (阶段 2 迁 host adapter, phase0 §4.6) --
    void SetPendingToplevel(ToplevelId id);
    ToplevelId TakePendingToplevel();             // 出队; 空 → 0 (= 现 getCurrentToplevelId)
    void CancelPendingToplevel(ToplevelId id);

private:
    HgsInstance();
    ~HgsInstance();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hgs
