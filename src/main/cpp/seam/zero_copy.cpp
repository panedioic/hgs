#include "zero_copy.h"
#include "registry.h"

namespace hgs {
namespace seam {

namespace {
// 默认空后端: 宿主未注入时合成器运行在 wl_shm CPU 路径 (完整可用形态)
NullZeroCopyBackend g_nullBackend;
} // namespace

ZeroCopyBackend &GetZeroCopyBackend()
{
    if (Hgs_zeroCopyBackend != nullptr) {
        return *Hgs_zeroCopyBackend;
    }
    return g_nullBackend;
}

} // namespace seam
} // namespace hgs
