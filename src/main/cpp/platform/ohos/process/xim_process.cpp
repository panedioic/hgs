#include "xim_process.h"
#include <AbilityKit/native_child_process.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

int hgs_spawn_xim(const char* display, const char* authority, const char* serverName) {
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair) != 0) return -1;
    struct stat original{};
    if (fstat(pair[1], &original) != 0) {
        close(pair[0]);
        close(pair[1]);
        return -1;
    }

    std::string params = std::string(display) + '\n' + authority + '\n' + serverName;
    NativeChildProcess_Fd node{};
    node.fd = pair[1];
    node.fdName = const_cast<char*>("xim");
    NativeChildProcess_Args args{};
    args.entryParams = params.data();
    args.fdList.head = &node;
    NativeChildProcess_Options options{};
    options.isolationMode = NCP_ISOLATION_MODE_NORMAL;
    int32_t pid = -1;
    const auto status = OH_Ability_StartNativeChildProcess("libhgs_xim_child.so:Main", args, options, &pid);
    // NCP may have closed the temporary descriptor during IPC.
    struct stat current{};
    if (fstat(pair[1], &current) == 0 && current.st_dev == original.st_dev && current.st_ino == original.st_ino)
        close(pair[1]);
    if (status == 0) return pair[0];
    close(pair[0]);
    return -1;
}
