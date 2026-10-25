#include "zero_copy.h"

namespace hgs {
namespace seam {

namespace {
// 默认空后端: 宿主未注入时合成器运行在 wl_shm CPU 路径 (完整可用形态)
NullZeroCopyBackend g_nullBackend;
ZeroCopyBackend *g_backend = &g_nullBackend;
} // namespace

ZeroCopyBackend &GetZeroCopyBackend()
{
    return *g_backend;
}

void SetZeroCopyBackend(ZeroCopyBackend *backend)
{
    g_backend = (backend != nullptr) ? backend : &g_nullBackend;
}

} // namespace seam
} // namespace hgs
