#pragma once

#include "include/hgs/service_types.h"

namespace hgs::service {
class X11Endpoint {
public:
    X11Endpoint() = default;
    ~X11Endpoint();
    X11Endpoint(const X11Endpoint&) = delete;
    X11Endpoint& operator=(const X11Endpoint&) = delete;

    bool Open(const Config& config, Error& error);
    void CloseListener();
    int Listener() const { return listener_; }
    int DisplayNumber() const { return displayNumber_; }
    std::string Display() const { return ":" + std::to_string(displayNumber_); }
    const std::string& Address() const { return address_; }
    const std::string& Directory() const { return directory_; }
    const std::string& Authority() const { return authority_; }

private:
    int listener_ = -1;
    int displayNumber_ = -1;
    std::string address_, directory_, authority_, stagedKeymap_;
};
}
