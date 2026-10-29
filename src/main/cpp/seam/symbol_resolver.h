// hgs/src/main/cpp/seam/symbol_resolver — 跨 so 符号的运行时查询 (hgs 侧)
//
// 两级: dlsym(RTLD_DEFAULT) → dlopen(对方 soname, RTLD_NOLOAD) 取已加载
// 句柄再 dlsym。NOLOAD 不加载模块, 只寻址; 查询失败不缓存 (模块可能稍后
// 加载)。背景见 registry.h 头注释。
#pragma once

namespace hgs {
namespace seam {

// 查宿主 (libentry.so) 导出的符号; 未找到返回 nullptr
void *ResolveHostSymbol(const char *name);

} // namespace seam
} // namespace hgs
