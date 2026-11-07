#include "platform/unix/socket_listener.h"
#include <wayland-server-core.h>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
using hgs::platform::SocketListener;
int Connect(const std::string& path) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0); assert(fd >= 0);
    sockaddr_un addr{}; addr.sun_family = AF_UNIX;
    std::strcpy(addr.sun_path, path.c_str());
    int rc = connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)); close(fd); return rc;
}
int main() {
    char dir[] = "/tmp/hgs-endpoint-XXXXXX"; assert(mkdtemp(dir));
    std::string path = std::string(dir) + "/wayland-test";
    std::string error;
    unsetenv("XDG_RUNTIME_DIR"); setenv("WAYLAND_DISPLAY", "host-sentinel", 1);
    SocketListener one, competing;
    assert(one.Open(path, error));
    auto* display = wl_display_create(); assert(display);
    assert(wl_display_add_socket_fd(display, one.TakeFd()) == 0);
    assert(Connect(path) == 0);
    assert(!getenv("XDG_RUNTIME_DIR") && std::string(getenv("WAYLAND_DISPLAY")) == "host-sentinel");
    assert(!competing.Open(path, error)); assert(Connect(path) == 0);
    struct stat st{}; assert(stat(path.c_str(), &st) == 0);
    wl_display_destroy_clients(display); wl_display_destroy(display); one.Reset();
    assert(lstat(path.c_str(), &st) < 0);
    setenv("XDG_RUNTIME_DIR", "/definitely-not-the-endpoint", 1);
    assert(one.Open(path, error));
    assert(Connect(path) == 0); assert(std::string(getenv("XDG_RUNTIME_DIR")) == "/definitely-not-the-endpoint");
    one.Reset();
    { std::ofstream f(path); f << "foreign file"; }
    assert(!one.Open(path, error));
    std::ifstream file(path); std::string content; getline(file, content); assert(content == "foreign file"); file.close();
    unlink(path.c_str());
    // Existing active Unix servers without our lock are also protected.
    int external = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0); assert(external >= 0);
    sockaddr_un addr{}; addr.sun_family = AF_UNIX; strcpy(addr.sun_path, path.c_str());
    assert(bind(external, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0); assert(listen(external, 4) == 0);
    assert(!one.Open(path, error)); assert(Connect(path) == 0);
    close(external);
    // Same-user stale socket can be reclaimed while holding the endpoint lock.
    assert(one.Open(path, error)); one.Reset();
    // 迟到的清理不能删除被替换的新文件。
    assert(one.Open(path, error));
    const std::string moved = path + ".old"; assert(rename(path.c_str(), moved.c_str()) == 0);
    { std::ofstream f(path); f << "replacement"; }
    one.Reset();
    std::ifstream replacement(path); getline(replacement, content); assert(content == "replacement"); replacement.close();
    unlink(path.c_str()); unlink(moved.c_str());
    unlink((path + ".lock").c_str()); assert(rmdir(dir) == 0);
    std::cout << "socket_listener_test: real wl_display, absent/wrong environment, collision and restart PASS\n";
}
