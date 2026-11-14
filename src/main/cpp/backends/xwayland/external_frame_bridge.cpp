#include "external_frame_bridge.h"
#include "common/display_fps.h"
#include <atomic>
#include <algorithm>
#include <hilog/log.h>

namespace hgs::service {
using namespace std::chrono_literals;

X11ExternalFrameBridge::X11ExternalFrameBridge(ExternalFrameProvider& provider, uint64_t periodNs)
    : provider_(provider), periodNs_(periodNs) {}

X11ExternalFrameBridge::~X11ExternalFrameBridge() {
    for (auto& [generation, binding] : bindings_) Release(binding);
}

void X11ExternalFrameBridge::PublishFrame(uint64_t token, Clock::time_point now) {
    if (frames_++ == 0) frameWindowStart_ = now;
    const auto elapsed = now - frameWindowStart_;
    if (elapsed < 1s) return;
    static std::atomic<uint64_t> sequence{0};
    const double seconds = std::chrono::duration<double>(elapsed).count();
    winehua_display_fps_publish(0, ++sequence, static_cast<double>(frames_ - 1) / seconds, "x11", token);
    frames_ = 0;
}

void X11ExternalFrameBridge::Refresh(Clock::time_point now) {
    if (now < nextQuery_) return;
    nextQuery_ = now + 100ms;
    std::vector<ExternalFrameTarget> current;
    if (!provider_.Query(current)) {
        if (now - lastQuery_ >= 500ms) targets_.clear();
        return;
    }
    lastQuery_ = now;
    targets_ = std::move(current);
    for (auto it = retired_.begin(); it != retired_.end();) {
        const auto token = *it;
        if (std::none_of(targets_.begin(), targets_.end(), [token](const auto& target) { return target.token == token; }))
            it = retired_.erase(it);
        else
            ++it;
    }
}

void X11ExternalFrameBridge::Release(Binding& binding) {
    if (binding.surface) {
        provider_.Unbind(binding.target);
        binding.surface.reset();
    }
    binding.errors = 0;
    binding.presented = false;
}

void X11ExternalFrameBridge::Close(uint64_t generation, bool unmapped) {
    const auto it = bindings_.find(generation);
    if (it == bindings_.end()) return;
    if (unmapped && it->second.target.token) retired_.insert(it->second.target.token);
    Release(it->second);
    bindings_.erase(it);
    // The provider's attached flag may have changed just now.
    nextQuery_ = {};
}

hgs_x11_external_result X11ExternalFrameBridge::Present(
    uint32_t xid, uint32_t pid, uint64_t generation, NativeWindow* output) {
    const auto now = Clock::now();
    Refresh(now);
    const auto matches = [&](const ExternalFrameTarget& target) {
        return target.token && target.protocol == Protocol::X11 && target.nativeId == xid &&
            pid && target.clientPid == pid;
    };
    auto existing = bindings_.find(generation);
    if (existing != bindings_.end() && existing->second.surface) {
        auto& binding = existing->second;
        const auto target = std::find_if(targets_.begin(), targets_.end(), [&](const auto& t) {
            return matches(t) && t.token == binding.target.token && t.vulkan == binding.target.vulkan;
        });
        if (target != targets_.end()) {
            using Result = ExternalTextureSurface::DrawResult;
            // X11 producers deliver their buffer rows in the opposite order
            // from the EGL window viewport.
            const auto result = binding.surface->Draw(true);
            if (result == Result::Presented) {
                PublishFrame(binding.target.token, now);
                binding.errors = 0;
                if (!binding.presented) {
                    OH_LOG_Print(LOG_APP, LOG_WARN, 0, "HGS_X11_GPU",
                                 "Presented first %{public}s frame xid=%{public}u token=%{public}llu",
                                 binding.target.vulkan ? "Vulkan" : "OpenGL", xid,
                                 static_cast<unsigned long long>(binding.target.token));
                    binding.presented = true;
                }
            } else if (result == Result::Retry) {
                ++binding.errors;
            }
            if (binding.errors < 6 && (binding.presented || now - binding.started < 3s))
                return binding.presented ? HGS_X11_EXTERNAL_PRESENTED : HGS_X11_EXTERNAL_OWNED;
            OH_LOG_Print(LOG_APP, LOG_WARN, 0, "HGS_X11_GPU", "Falling back to X11 buffers xid=%{public}u", xid);
        }
        Release(binding);
        binding.retryAt = now + 1s;
        return HGS_X11_EXTERNAL_RELEASED;
    }
    if (existing != bindings_.end() && now < existing->second.retryAt) return HGS_X11_EXTERNAL_IDLE;
    const auto candidate = std::find_if(targets_.begin(), targets_.end(), [&](const auto& t) {
        return matches(t) && !t.attached && t.width && t.height && t.width <= 16384 &&
            t.height <= 16384 && retired_.count(t.token) == 0;
    });
    if (candidate == targets_.end()) return HGS_X11_EXTERNAL_IDLE;
    // A generation may have more than one guest producer. Wait for a unique
    // target instead of switching between swapchains for the same native ID.
    if (std::count_if(targets_.begin(), targets_.end(), matches) != 1) return HGS_X11_EXTERNAL_IDLE;
    auto& binding = bindings_[generation];
    auto surface = std::make_unique<ExternalTextureSurface>();
    if (!surface->Open(output, candidate->width, candidate->height) ||
        !provider_.Bind(*candidate, surface->Producer(), periodNs_)) {
        binding.retryAt = now + 1s;
        return HGS_X11_EXTERNAL_RELEASED;
    }
    binding.target = *candidate;
    binding.surface = std::move(surface);
    binding.started = now;
    OH_LOG_Print(LOG_APP, LOG_WARN, 0, "HGS_X11_GPU", "Bound %{public}s target xid=%{public}u pid=%{public}u",
                 binding.target.vulkan ? "Vulkan" : "OpenGL", xid, pid);
    return HGS_X11_EXTERNAL_OWNED;
}
}
