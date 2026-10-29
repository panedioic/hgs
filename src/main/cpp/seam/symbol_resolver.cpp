// hgs/src/main/cpp/seam/zero_copy.h — 跨 so 符号查询实现
#include "symbol_resolver.h"

#include <dlfcn.h>

namespace hgs {
namespace seam {

void *ResolveHostSymbol(const char *name)
{
    void *sym = dlsym(RTLD_DEFAULT, name);
    if (sym != nullptr) {
        return sym;
    }
    // NAPI so 通常以独立命名空间加载, RTLD_DEFAULT 看不到 — 用 NOLOAD
    // 取已加载的 libentry.so 句柄再查 (不触发加载)
    void *handle = dlopen("libentry.so", RTLD_NOW | RTLD_NOLOAD);
    if (handle != nullptr) {
        sym = dlsym(handle, name);
    }
    return sym;
}

} // namespace seam
} // namespace hgs
