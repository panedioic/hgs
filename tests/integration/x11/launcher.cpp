#include <AbilityKit/native_child_process.h>
#include <napi/native_api.h>
#include <string>

namespace {
napi_value Launch(napi_env env, napi_callback_info info) {
    napi_value argv[1], result;
    size_t argc = 1, size = 0;
    if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc != 1 ||
        napi_get_value_string_utf8(env, argv[0], nullptr, 0, &size) != napi_ok || size > 4096) {
        napi_throw_type_error(env, nullptr, "expected probe parameters");
        return nullptr;
    }
    std::string params(size + 1, '\0');
    napi_get_value_string_utf8(env, argv[0], params.data(), params.size(), &size);
    NativeChildProcess_Args args{};
    args.entryParams = params.data();
    NativeChildProcess_Options options{};
    options.isolationMode = NCP_ISOLATION_MODE_NORMAL;
    int32_t pid = -1;
    const int status = OH_Ability_StartNativeChildProcess(
        "libhgs_x11_probe_child.so:Main", args, options, &pid);
    napi_create_int32(env, status == 0 ? pid : -status, &result);
    return result;
}
napi_value Init(napi_env env, napi_value exports) {
    napi_property_descriptor launch{"launch", nullptr, Launch, nullptr, nullptr, nullptr, napi_default, nullptr};
    napi_define_properties(env, exports, 1, &launch);
    return exports;
}
napi_module module = {1, 0, nullptr, Init, "hgs_x11_probe", nullptr, {0}};
}
extern "C" __attribute__((constructor)) void RegisterProbe() { napi_module_register(&module); }
