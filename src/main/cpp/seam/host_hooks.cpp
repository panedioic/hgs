#include "host_hooks.h"

namespace hgs {
namespace seam {

namespace {
NullHostLifecycleHooks g_nullHooks;
HostLifecycleHooks *g_hooks = &g_nullHooks;
} // namespace

HostLifecycleHooks &GetHostLifecycleHooks()
{
    return *g_hooks;
}

void SetHostLifecycleHooks(HostLifecycleHooks *hooks)
{
    g_hooks = (hooks != nullptr) ? hooks : &g_nullHooks;
}

} // namespace seam
} // namespace hgs
