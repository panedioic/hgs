#include "xwayland_process.h"
#include <AbilityKit/native_child_process.h>
#include <cerrno>
#include <chrono>
#include <hilog/log.h>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

bool hgs_spawn_xwayland(const char* display, const char* runtimeDir, const char* xkbRoot,
                        const char* authorityPath, int listenFd, int waylandFd, int xwmFd,
                        int readyFd, hgs_xwayland_process* process) {
    int control[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, control) != 0) return false;

    const char* names[] = {"listen", "wayland", "xwm", "ready", "control"};
    const int originals[] = {listenFd, waylandFd, xwmFd, readyFd, control[1]};
    NativeChildProcess_Fd nodes[5]{};
    struct stat identity[5]{};
    size_t count = 0;
    for (; count < 5; ++count) {
        const int fd = fcntl(originals[count], F_DUPFD_CLOEXEC, 3);
        if (fd < 0) break;
        if (fstat(fd, &identity[count]) != 0) {
            close(fd);
            break;
        }
        nodes[count].fd = fd;
        nodes[count].fdName = const_cast<char*>(names[count]);
        if (count) nodes[count - 1].next = &nodes[count];
    }
    close(control[1]);

    int32_t pid = -1;
    bool launched = false;
    if (count == 5) {
        std::string params = std::string(display) + '\n' + runtimeDir + '\n' + xkbRoot + '\n' + authorityPath;
        NativeChildProcess_Args args{};
        args.entryParams = params.data();
        args.fdList.head = nodes;
        NativeChildProcess_Options options{};
        options.isolationMode = NCP_ISOLATION_MODE_NORMAL;
        const auto status = OH_Ability_StartNativeChildProcess(
            "libhgs_xwayland_child.so:Main", args, options, &pid);
        if (status == 0) {
            launched = true;
        } else {
            OH_LOG_ERROR(LOG_APP, "HGS Xwayland NCP start failed: %{public}d", static_cast<int>(status));
        }
    }
    // IPC 完成后释放父进程内的临时副本。部分 NCP 实现已关闭这些 fd，
    // 先检查仍指向本次传递的对象，避免误关已复用的 fd 编号。
    for (size_t i = 0; i < count; ++i) {
        struct stat current{};
        if (fstat(nodes[i].fd, &current) == 0 &&
            current.st_dev == identity[i].st_dev && current.st_ino == identity[i].st_ino) {
            close(nodes[i].fd);
        }
    }
    if (!launched) {
        close(control[0]);
        return false;
    }
    process->pid = pid;
    process->control_fd = control[0];
    return true;
}

void hgs_stop_xwayland(hgs_xwayland_process* process) {
    if (process->control_fd < 0) return;
    // 只通知本次启动的子进程。读端 HUP 由子进程关闭控制 fd 产生。
    shutdown(process->control_fd, SHUT_WR);
    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + std::chrono::seconds(2);
    bool disconnected = false;
    char buffer[256];
    while (Clock::now() < deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        pollfd wait{process->control_fd, POLLIN | POLLHUP, 0};
        const int ready = poll(&wait, 1, static_cast<int>(remaining.count()));
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) break;
        const ssize_t count = recv(process->control_fd, buffer, sizeof(buffer), MSG_DONTWAIT);
        if (count == 0 || (count < 0 && errno == ECONNRESET)) {
            disconnected = true;
            break;
        }
    }
    if (!disconnected) OH_LOG_WARN(LOG_APP, "HGS Xwayland child exit unobserved: pid=%{public}d", process->pid);
    close(process->control_fd);
    process->control_fd = -1;
    process->pid = -1;
}
