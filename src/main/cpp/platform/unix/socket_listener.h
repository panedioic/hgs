#pragma once
#include <string>
#include <cstdint>
namespace hgs::platform {
// Owns pathname/lock; after TakeFd, wl_display owns the listening descriptor.
class SocketListener {
public:
    ~SocketListener();
    bool Open(const std::string& path, std::string& error);
    int TakeFd();
    void Reset();
private:
    int fd_ = -1, lock_ = -1;
    std::string path_;
    uint64_t device_ = 0, inode_ = 0;
};
}
