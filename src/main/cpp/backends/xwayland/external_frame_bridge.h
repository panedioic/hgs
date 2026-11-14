#pragma once

#include "runtime/session.h"
#include "graphics/external_texture_surface.h"
#include "include/hgs/external_frames.h"
#include <chrono>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace hgs::service {
class X11ExternalFrameBridge {
public:
    X11ExternalFrameBridge(ExternalFrameProvider& provider, uint64_t periodNs);
    ~X11ExternalFrameBridge();

    hgs_x11_external_result Present(uint32_t xid, uint32_t pid, uint64_t generation, NativeWindow* output);
    void Close(uint64_t generation, bool unmapped);

private:
    using Clock = std::chrono::steady_clock;
    struct Binding {
        ExternalFrameTarget target;
        std::unique_ptr<ExternalTextureSurface> surface;
        Clock::time_point started{}, retryAt{};
        unsigned errors = 0;
        bool presented = false;
    };
    void PublishFrame(uint64_t token, Clock::time_point now);
    void Refresh(Clock::time_point now);
    void Release(Binding& binding);

    ExternalFrameProvider& provider_;
    uint64_t periodNs_;
    Clock::time_point nextQuery_{}, lastQuery_{}, frameWindowStart_{};
    uint64_t frames_ = 0;
    std::vector<ExternalFrameTarget> targets_;
    std::unordered_map<uint64_t, Binding> bindings_;
    std::unordered_set<uint64_t> retired_;
};
}
