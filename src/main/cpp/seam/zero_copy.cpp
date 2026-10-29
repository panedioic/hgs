// hgs/src/main/cpp/seam/zero_copy.h — 零拷贝接缝实现
#include "zero_copy.h"
#include "symbol_resolver.h"

namespace hgs {
namespace seam {

namespace {

NullZeroCopyBackend g_nullBackend;

// 宿主 (libentry.so) 的注入变量 Hgs_zeroCopyBackend 的运行时解析结果
ZeroCopyBackend **ResolveSlot()
{
    static ZeroCopyBackend **slot = nullptr;
    if (slot == nullptr) {
        slot = static_cast<ZeroCopyBackend **>(ResolveHostSymbol("Hgs_zeroCopyBackend"));
    }
    return slot;
}

} // namespace

ZeroCopyBackend &GetZeroCopyBackend()
{
    ZeroCopyBackend **slot = ResolveSlot();
    if (slot != nullptr && *slot != nullptr) {
        return **slot;
    }
    return g_nullBackend;
}

} // namespace seam
} // namespace hgs
