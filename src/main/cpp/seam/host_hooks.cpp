// hgs/src/main/cpp/seam/host_hook.h — 生命周期通知接缝实现
#include "host_hooks.h"
#include "symbol_resolver.h"

namespace hgs {
namespace seam {

namespace {

NullHostLifecycleHooks g_nullHooks;

// 宿主 (libentry.so) 的钩子注入变量 Hgs_hostHooks 的运行时解析结果。
// 不缓存失败 (宿主可能稍后注入), 解析成功后缓存指针位置。
HostLifecycleHooks **ResolveSlot()
{
    static HostLifecycleHooks **slot = nullptr;
    if (slot == nullptr) {
        slot = static_cast<HostLifecycleHooks **>(ResolveHostSymbol("Hgs_hostHooks"));
    }
    return slot;
}

} // namespace

HostLifecycleHooks &GetHostLifecycleHooks()
{
    HostLifecycleHooks **slot = ResolveSlot();
    if (slot != nullptr && *slot != nullptr) {
        return **slot;
    }
    return g_nullHooks;
}

} // namespace seam
} // namespace hgs
