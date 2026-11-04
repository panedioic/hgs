#pragma once
// ============================================================================
// hgs_export.h — HGS 公共符号可见性标注 (阶段 0: 空宏)。
//
// 阶段 0 不收紧 -fvisibility: seam/* 仍依赖跨 so 的 dlsym 双向符号解析
// (默认可见性), 提前 hidden 会先破坏 WineHua 的 host_hooks/zero_copy 链路。
// 阶段 1 拆完 seam (IExternalSurfaceProvider) 后:
//   1) 本宏改为 __attribute__((visibility("default")))
//   2) CMake 开 CXX_VISIBILITY_PRESET hidden + VISIBILITY_INLINES_HIDDEN
//   3) seam 符号单独用 HGS_SEAM_API 标注
// 详见 docs/architecture/hgs-exported-api.md §3.2 / §10.4。
// ============================================================================

#define HGS_API
