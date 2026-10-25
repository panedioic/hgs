#include "host_hooks.h"
#include "registry.h"

namespace hgs {
namespace seam {

namespace {
NullHostLifecycleHooks g_nullHooks;
} // namespace

HostLifecycleHooks &GetHostLifecycleHooks()
{
    if (Hgs_hostHooks != nullptr) {
        return *Hgs_hostHooks;
    }
    return g_nullHooks;
}

} // namespace seam
} // namespace hgs
