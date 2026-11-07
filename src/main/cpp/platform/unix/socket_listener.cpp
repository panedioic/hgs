#include "socket_listener.h"
#include "api/hgs_endpoint.h"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
namespace hgs::platform {
SocketListener::~SocketListener() { Reset(); }
int SocketListener::TakeFd() { int fd = fd_; fd_ = -1; return fd; }
void SocketListener::Reset() {
    if (fd_ >= 0) close(fd_);
    fd_ = -1;
    struct stat st{};
    if (!path_.empty() && lstat(path_.c_str(), &st) == 0 &&
        static_cast<uint64_t>(st.st_dev) == device_ && static_cast<uint64_t>(st.st_ino) == inode_)
        unlink(path_.c_str());
    path_.clear(); device_ = inode_ = 0;
    // Keep the lock inode: unlinking a flock file creates a lock-stealing race.
    if (lock_ >= 0) close(lock_);
    lock_ = -1;
}
bool SocketListener::Open(const std::string& path, std::string& error) {
    Reset();
    const auto ep = SplitSocketPath(path);
    sockaddr_un addr{};
    if (!ep.valid || path.size() >= sizeof(addr.sun_path)) { error = "invalid or overlong Unix socket path"; return false; }
    auto fail = [&](const char* stage) { error = std::string(stage) + ": " + strerror(errno); Reset(); return false; };
    if (mkdir(ep.directory.c_str(), 0700) < 0 && errno != EEXIST) return fail("mkdir runtime directory");
    lock_ = open((path + ".lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock_ < 0) return fail("open endpoint lock");
    struct stat lockStat{};
    if (fstat(lock_, &lockStat) < 0 || !S_ISREG(lockStat.st_mode) || lockStat.st_uid != geteuid()) {
        errno = EACCES; return fail("invalid endpoint lock");
    }
    if (flock(lock_, LOCK_EX | LOCK_NB) < 0) return fail("endpoint already locked");
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    struct stat st{};
    if (lstat(path.c_str(), &st) == 0) {
        if (!S_ISSOCK(st.st_mode) || st.st_uid != geteuid()) { errno = EEXIST; return fail("endpoint belongs to another object"); }
        int probe = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (probe < 0) return fail("probe socket");
        int rc = connect(probe, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        int saved = errno; close(probe); errno = saved;
        if (rc == 0 || errno != ECONNREFUSED) { errno = EADDRINUSE; return fail("endpoint is serving clients"); }
        struct stat current{};
        if (lstat(path.c_str(), &current) < 0 || current.st_dev != st.st_dev || current.st_ino != st.st_ino) {
            errno = EAGAIN; return fail("endpoint changed during stale probe");
        }
        if (unlink(path.c_str()) < 0) return fail("remove stale owned endpoint");
    } else if (errno != ENOENT) return fail("inspect endpoint");
    fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd_ < 0) return fail("socket");
    if (bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) return fail("bind endpoint");
    if (lstat(path.c_str(), &st) < 0) return fail("inspect bound endpoint");
    path_ = path; device_ = st.st_dev; inode_ = st.st_ino;
    if (chmod(path.c_str(), 0600) < 0) return fail("chmod endpoint");
    if (listen(fd_, 128) < 0) return fail("listen endpoint");
    error.clear();
    return true;
}
}
