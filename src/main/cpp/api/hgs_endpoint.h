#pragma once
// ============================================================================
// hgs_endpoint.h — Wayland socket 端点拆分 + 客户端环境变量 (阶段 0, P0.2)。
//
// 出处: docs/architecture/hgs-shared-runtime-phase0.md §4.4。
// 纯函数 (仅标准库), 可被 host_tests 直连编译 — SplitSocketPath 的目录/
// 显示名拆分逻辑与 WaylandServer::Start 内部的 find_last_of 手工拆分同源,
// 本模块是其契约化 (测试固化), 供宿主不经 setenv 直接把环境传给子进程。
// ============================================================================

#include <string>
#include <vector>

#include "hgs_export.h"

namespace hgs {

struct HGS_API SocketEndpoint {
    bool valid = false;
    std::string directory;    // → XDG_RUNTIME_DIR (根级 socket 时为 "/")
    std::string displayName;  // → WAYLAND_DISPLAY (basename)
};

// 拆分 socket 路径。规则:
//   - 必须绝对路径 (首字符 '/')
//   - 不允许尾斜杠 (basename 非空)
//   - 根级路径 "/wayland-0" → directory="/", displayName="wayland-0"
// 无效输入返回 valid=false (其余字段为空)。
HGS_API SocketEndpoint SplitSocketPath(const std::string& socketPath);

// 客户端环境变量: {"XDG_RUNTIME_DIR=<dir>", "WAYLAND_DISPLAY=<name>"},
// 顺序固定。路径无效返回空 vector。
// 用法: 宿主把返回值合入子进程环境 (Box64/Wine/GTK/Qt), 替代让 HGS
// setenv 污染宿主进程全局环境 (phase0 §4.2; HgsConfig.setProcessEnvironment)。
HGS_API std::vector<std::string> BuildClientEnvironment(const std::string& socketPath);

} // namespace hgs
