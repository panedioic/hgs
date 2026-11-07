#include "x11_endpoint.h"
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace hgs::service {
namespace {
bool WritePrivate(const std::string& path, const void* data, size_t size) {
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    const auto* remaining = static_cast<const char*>(data);
    while (size) {
        const ssize_t written = write(fd, remaining, size);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) break;
        size -= static_cast<size_t>(written);
        remaining += written;
    }
    const int status = close(fd);
    return size == 0 && status == 0;
}

bool ReadKeymap(const std::string& path, std::vector<char>& data) {
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return false;
    struct stat info{};
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 4 || info.st_size > 1024 * 1024) {
        close(fd);
        return false;
    }
    data.resize(info.st_size);
    size_t offset = 0;
    while (offset < data.size()) {
        const ssize_t count = read(fd, data.data() + offset, data.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        offset += count;
    }
    close(fd);
    return offset == data.size() && std::memcmp(data.data(), "\x0f" "mkx", 4) == 0;
}

bool WriteAuthority(const std::string& path, const std::string& display) {
    char cookie[16];
    if (getentropy(cookie, sizeof(cookie)) != 0) return false;
    // Xauthority 采用网络字节序。FamilyWild 允许标准本地 Xlib 找到此
    // display 的 cookie。记录仅在本次会话的 0700 目录内可读。
    std::string record("\xff\xff", 2);
    const auto add = [&](const char* bytes, size_t size) {
        record.push_back(static_cast<char>(size >> 8));
        record.push_back(static_cast<char>(size & 255));
        record.append(bytes, size);
    };
    add("", 0);
    add(display.data(), display.size());
    constexpr char method[] = "MIT-MAGIC-COOKIE-1";
    add(method, sizeof(method) - 1);
    add(cookie, sizeof(cookie));
    return WritePrivate(path, record.data(), record.size());
}
}

void X11Endpoint::CloseListener() {
    if (listener_ >= 0) close(listener_);
    listener_ = -1;
}

X11Endpoint::~X11Endpoint() {
    CloseListener();
    if (!stagedKeymap_.empty()) unlink(stagedKeymap_.c_str());
    if (!authority_.empty()) unlink(authority_.c_str());
    if (!directory_.empty()) rmdir(directory_.c_str());
}

bool X11Endpoint::Open(const Config& config, Error& error) {
    const auto absolute = [](const std::string& path) {
        return !path.empty() && path.front() == '/' &&
               path.find('\0') == std::string::npos && path.find('\n') == std::string::npos;
    };
    if (!absolute(config.runtimeDir) || !absolute(config.x11.xkbRoot) || !absolute(config.x11.keymapPath) ||
        config.x11.startupTimeoutMs < 100 || config.x11.startupTimeoutMs > 30000) {
        error = {ErrorCode::InvalidConfig, "x11-config", "X11 requires absolute resource paths and a timeout of 100–30000 ms"};
        return false;
    }
    std::vector<char> keymap;
    if (access((config.x11.xkbRoot + "/rules/evdev").c_str(), R_OK) != 0 ||
        !ReadKeymap(config.x11.keymapPath, keymap)) {
        error = {ErrorCode::InvalidConfig, "x11-resources", "XKB rules or a valid precompiled keymap are missing"};
        return false;
    }
    if (mkdir(config.runtimeDir.c_str(), 0700) < 0 && errno != EEXIST) {
        error = {ErrorCode::BackendFailed, "x11-runtime-dir", std::strerror(errno)};
        return false;
    }
    std::string path = config.runtimeDir + "/hgs-x11-XXXXXX";
    if (!mkdtemp(path.data())) {
        error = {ErrorCode::BackendFailed, "x11-runtime-dir", std::strerror(errno)};
        return false;
    }
    directory_ = std::move(path);

    // 使用 Xlib 的标准 abstract AF_UNIX 名。bind 原子占位，无需删除
    // 既有 socket 或依赖 /tmp、XDG_RUNTIME_DIR。
    for (int number = 0; number <= 32; ++number) {
        const std::string name = "/tmp/.X11-unix/X" + std::to_string(number);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::memcpy(addr.sun_path + 1, name.data(), name.size());
        const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            error = {ErrorCode::BackendFailed, "x11-listener", std::strerror(errno)};
            return false;
        }
        const auto size = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + name.size());
        if (bind(fd, reinterpret_cast<const sockaddr*>(&addr), size) == 0) {
            if (listen(fd, 32) == 0) {
                listener_ = fd;
                displayNumber_ = number;
                address_ = "@" + name;
                break;
            }
        }
        const int failure = errno;
        close(fd);
        if (failure != EADDRINUSE) {
            error = {ErrorCode::BackendFailed, "x11-listener", std::strerror(failure)};
            return false;
        }
    }
    if (listener_ < 0) {
        error = {ErrorCode::BackendFailed, "x11-listener", "no free local X11 display"};
        return false;
    }
    authority_ = directory_ + "/Xauthority";
    stagedKeymap_ = directory_ + "/server-" + std::to_string(displayNumber_) + ".xkm";
    if (!WritePrivate(stagedKeymap_, keymap.data(), keymap.size()) ||
        !WriteAuthority(authority_, std::to_string(displayNumber_))) {
        error = {ErrorCode::BackendFailed, "x11-resources", "could not prepare the private keymap and authority files"};
        return false;
    }
    return true;
}
}
