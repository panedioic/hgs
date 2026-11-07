#include <AbilityKit/native_child_process.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
int FdByName(const NativeChildProcess_Args& args, const char* name) {
    for (auto* node = args.fdList.head; node; node = node->next) {
        if (node->fdName && std::strcmp(node->fdName, name) == 0) return node->fd;
    }
    return -1;
}
}

extern "C" __attribute__((visibility("default"))) void Main(NativeChildProcess_Args args) {
    const int listenFd = FdByName(args, "listen");
    const int waylandFd = FdByName(args, "wayland");
    const int xwmFd = FdByName(args, "xwm");
    const int readyFd = FdByName(args, "ready");
    const int controlFd = FdByName(args, "control");
    if (listenFd < 0 || waylandFd < 0 || xwmFd < 0 || readyFd < 0 || controlFd < 0) return;

    std::istringstream params(args.entryParams ? args.entryParams : "");
    std::string display, runtimeDir, xkbRoot, authority;
    if (!std::getline(params, display) || !std::getline(params, runtimeDir) ||
        !std::getline(params, xkbRoot) || !std::getline(params, authority) ||
        display.empty() || runtimeDir.empty() || xkbRoot.empty() || authority.empty()) return;

    if (dup2(controlFd, STDERR_FILENO) < 0) return;
    setvbuf(stderr, nullptr, _IONBF, 0);
    fprintf(stderr, "Xwayland child entered\n");

    // 父进程停止或退出时，这一条专用通道也会关闭。
    std::thread([controlFd] {
        char byte;
        while (read(controlFd, &byte, 1) < 0 && errno == EINTR) {}
        _exit(0);
    }).detach();

    setenv("WAYLAND_SOCKET", std::to_string(waylandFd).c_str(), 1);
    setenv("XDG_RUNTIME_DIR", runtimeDir.c_str(), 1);
    setenv("XKB_CONFIG_ROOT", xkbRoot.c_str(), 1);
    std::vector<std::string> options{
        "Xwayland", display, "-rootless", "-nolisten", "tcp", "-auth", authority,
        "-extension", "MIT-SHM", "-xkbdir", xkbRoot, "-shm",
        "-listenfd", std::to_string(listenFd), "-displayfd", std::to_string(readyFd),
        "-wm", std::to_string(xwmFd)
    };
    std::vector<char*> argv;
    for (auto& option : options) argv.push_back(option.data());
    argv.push_back(nullptr);

    void* image = dlopen("libxwayland_ohos.so", RTLD_NOW | RTLD_LOCAL);
    if (!image) {
        fprintf(stderr, "Xwayland load failed: %s\n", dlerror());
        _exit(1);
    }
    using Entry = int (*)(int, char**, char**);
    auto entry = reinterpret_cast<Entry>(dlsym(image, "main"));
    if (!entry) {
        fprintf(stderr, "Xwayland entry symbol is missing\n");
        _exit(1);
    }
    fprintf(stderr, "Starting Xwayland display %s\n", display.c_str());
    const int result = entry(static_cast<int>(options.size()), argv.data(), environ);
    _exit(result);
}
