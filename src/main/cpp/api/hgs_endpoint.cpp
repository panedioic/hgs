// hgs/src/main/cpp/api/hgs_endpoint.cpp — SplitSocketPath/BuildClientEnvironment
// 纯标准库实现 (阶段 0 P0.2)。行为与 WaylandServer::Start 内部拆分同源
// (wayland_server.cpp: find_last_of('/') → dir/name), 差异仅本模块契约化:
// 校验绝对路径/尾斜杠, 根级路径 directory 规范化为 "/"。

#include "hgs_endpoint.h"

namespace hgs {

SocketEndpoint SplitSocketPath(const std::string& socketPath) {
    SocketEndpoint ep;
    if (socketPath.find('\0') != std::string::npos) return ep;
    if (socketPath.empty()) return ep;          // 空串
    if (socketPath.front() != '/') return ep;   // 相对路径
    if (socketPath.back() == '/') return ep;    // 尾斜杠 (basename 为空)

    const auto pos = socketPath.find_last_of('/');
    // 首字符是 '/', pos 必然存在; pos+1 起 basename 非空 (上面已排除尾斜杠)
    ep.displayName = socketPath.substr(pos + 1);
    if (ep.displayName == "." || ep.displayName == "..") return {};
    if (ep.displayName.empty()) return ep;      // 防御 (理论上不可达)
    ep.directory = (pos == 0) ? std::string("/") : socketPath.substr(0, pos);
    ep.valid = true;
    return ep;
}

std::vector<std::string> BuildClientEnvironment(const std::string& socketPath) {
    std::vector<std::string> env;
    const SocketEndpoint ep = SplitSocketPath(socketPath);
    if (!ep.valid) return env;
    env.push_back("XDG_RUNTIME_DIR=" + ep.directory);
    env.push_back("WAYLAND_DISPLAY=" + ep.displayName);
    return env;
}

} // namespace hgs
