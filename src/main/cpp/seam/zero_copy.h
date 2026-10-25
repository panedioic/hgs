// ============================================================================
// seam/zero_copy — WineHua 图形后端与 hgs 合成器域之间的唯一原生边界
// (方案 B 边界契约的原生部分, 与 docs/architecture/contracts.md C11 对应)
//
// 规则:
//   - 合成器/渲染器代码只允许 include 本头文件, 不得 include
//     graphics/graphics_broker.h 等 WineHua 应用域头文件
//   - 宿主 (entry) 在装配期调用 SetZeroCopyBackend() 注入实现
//     (GraphicsBroker 适配器); 未注入时为 NullZeroCopyBackend:
//     ZC 关闭, 合成器自动运行在 wl_shm CPU 路径 — 与 GraphicsBroker
//     的 shm fallback 语义一致, 是完整可用的降级形态
//   - ZeroCopySurfaceInfo 字段与 entry graphics_broker.h 的同名 POD
//     布局一致; 任何一侧改动必须同步 (契约)
// ============================================================================
#pragma once

#include <cstdint>
#include <native_window/external_window.h>
#include <vector>

namespace hgs {
namespace seam {

struct ZeroCopySurfaceInfo
{
    uint64_t surfaceKey = 0;
    uint32_t clientPid = 0;
    uint32_t surfaceId = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t serial = 0;
    bool attached = false;
    bool vulkan = false;
};

// 合成器/渲染器视角的零拷贝后端。语义与 entry GraphicsBroker 对应方法一致。
class ZeroCopyBackend
{
public:
    virtual ~ZeroCopyBackend() = default;

    // ZC 传输是否激活 (对应 broker: GetState().active == Virgl)
    virtual bool IsZeroCopyActive() const = 0;
    // 会话是否处于 Vulkan present 模式 (DXVK/VKD3D → Venus)
    virtual bool IsVulkanPresentMode() const = 0;
    // 拉取当前可挂载的 ZC surface 列表 (渲染线程 ~100ms 轮询)
    virtual bool QueryZeroCopySurfaces(std::vector<ZeroCopySurfaceInfo> &surfaces) const = 0;
    // 把 OH_NativeImage 消费者的 producer 窗口挂给 GPU 后端 (宿主经
    // surfaceId 还原窗口后转交 virgl/venus presenter)
    virtual bool AttachZeroCopyTarget(uint64_t surfaceKey, OHNativeWindow *producerWindow,
                                      uint64_t framePeriodNs) = 0;
    virtual void DetachZeroCopyTarget(uint64_t surfaceKey) = 0;
    virtual void SetZeroCopyFramePeriod(uint64_t surfaceKey, uint64_t framePeriodNs) = 0;
    // .ready marker 落盘 (zc_bridge 的发布/回退协议, contract C4)。
    // 时序纪律: 先注册 compositor key 再发布 ready; 回退先撤 ready。
    virtual void SetZeroCopySurfaceReady(uint64_t surfaceKey, bool ready) = 0;
};

// 空后端: 全部关闭, 方法均为安全 no-op
class NullZeroCopyBackend : public ZeroCopyBackend
{
public:
    bool IsZeroCopyActive() const override { return false; }
    bool IsVulkanPresentMode() const override { return false; }
    bool QueryZeroCopySurfaces(std::vector<ZeroCopySurfaceInfo> &) const override { return false; }
    bool AttachZeroCopyTarget(uint64_t, OHNativeWindow *, uint64_t) override { return false; }
    void DetachZeroCopyTarget(uint64_t) override {}
    void SetZeroCopyFramePeriod(uint64_t, uint64_t) override {}
    void SetZeroCopySurfaceReady(uint64_t, bool) override {}
};

// 进程级后端访问: 弱符号 Hgs_zeroCopyBackend (registry.h) 由宿主注入,
// 未注入时返回 NullZeroCopyBackend (ZC 关闭, wl_shm 路径)。
ZeroCopyBackend &GetZeroCopyBackend();

} // namespace seam
} // namespace hgs
