#include "napi/native_api.h"
#include "render/egl_renderer.h"

#include <cstdint>
#include <hilog/log.h>
#include <mutex>
#include <unordered_map>

namespace {

std::mutex gMutex;
std::unordered_map<uint64_t, hgs::EmptyEglRenderer *> gRenderers;

// 临时冒烟接口: createEmptyRenderer(id: number, surfaceId: BigInt)。
// 参数约定对齐 entry 的 createRenderer (surfaceId 走 BigInt 防精度丢失)。
napi_value CreateEmptyRenderer(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 2) {
        OH_LOG_ERROR(LOG_APP, "[HGS-NAPI] createEmptyRenderer: need 2 args (id, surfaceId)");
        return nullptr;
    }

    uint32_t id = 0;
    if (napi_get_value_uint32(env, args[0], &id) != napi_ok) {
        OH_LOG_ERROR(LOG_APP, "[HGS-NAPI] createEmptyRenderer: id parse failed");
        return nullptr;
    }

    uint64_t surfaceId = 0;
    bool lossless = true;
    if (napi_get_value_bigint_uint64(env, args[1], &surfaceId, &lossless) != napi_ok) {
        OH_LOG_ERROR(LOG_APP, "[HGS-NAPI] createEmptyRenderer: surfaceId BIGINT parse failed");
        return nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(gMutex);
        auto it = gRenderers.find(id);
        if (it != gRenderers.end()) {
            OH_LOG_WARN(LOG_APP, "[HGS-NAPI] renderer id=%{public}u already exists, destroy first", id);
            delete it->second;
            gRenderers.erase(it);
        }
        hgs::EmptyEglRenderer *r = hgs::EmptyEglRenderer::Create(surfaceId);
        if (r == nullptr) {
            OH_LOG_ERROR(LOG_APP, "[HGS-NAPI] EmptyEglRenderer::Create(surfaceId=%{public}llu) failed",
                         static_cast<unsigned long long>(surfaceId));
            return nullptr;
        }
        gRenderers[id] = r;
        OH_LOG_INFO(LOG_APP, "[HGS-NAPI] createEmptyRenderer id=%{public}u surfaceId=%{public}llu",
                    id, static_cast<unsigned long long>(surfaceId));
    }
    return nullptr;
}

napi_value DestroyEmptyRenderer(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) {
        OH_LOG_ERROR(LOG_APP, "[HGS-NAPI] destroyEmptyRenderer: need 1 arg (id)");
        return nullptr;
    }

    uint32_t id = 0;
    if (napi_get_value_uint32(env, args[0], &id) != napi_ok) {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(gMutex);
    auto it = gRenderers.find(id);
    if (it != gRenderers.end()) {
        delete it->second;
        gRenderers.erase(it);
        OH_LOG_INFO(LOG_APP, "[HGS-NAPI] destroyEmptyRenderer id=%{public}u", id);
    }
    return nullptr;
}

EXTERN_C_START
static napi_value Init(napi_env env, napi_value exports) {
    napi_property_descriptor desc[] = {
        {"createEmptyRenderer", nullptr, CreateEmptyRenderer, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"destroyEmptyRenderer", nullptr, DestroyEmptyRenderer, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
    return exports;
}
EXTERN_C_END

static napi_module hgsModule = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,
    .nm_modname = "hgs",
    .nm_priv = nullptr,
    .reserved = {0},
};

extern "C" __attribute__((constructor)) void RegisterHgsModule(void) {
    napi_module_register(&hgsModule);
}

} // namespace
