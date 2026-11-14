#include <AbilityKit/native_child_process.h>
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>

// Parameters come from the host's Wayland session and its staged GTK resources.
extern "C" __attribute__((visibility("default"))) void Main(NativeChildProcess_Args args) {
    std::istringstream params(args.entryParams ? args.entryParams : "");
    std::string runtimeDir, display, log, xkbRoot, fontConfig;
    if (!std::getline(params, runtimeDir) || !std::getline(params, display) ||
        !std::getline(params, log) || !std::getline(params, xkbRoot) ||
        !std::getline(params, fontConfig)) return;
    if (!freopen(log.c_str(), "w", stdout) || !freopen(log.c_str(), "a", stderr)) return;
    setvbuf(stdout, nullptr, _IOLBF, 0);

    unsetenv("DISPLAY");
    unsetenv("WAYLAND_SOCKET");
    setenv("XDG_RUNTIME_DIR", runtimeDir.c_str(), 1);
    setenv("WAYLAND_DISPLAY", display.c_str(), 1);
    setenv("GDK_BACKEND", "wayland", 1);
    setenv("XKB_CONFIG_ROOT", xkbRoot.c_str(), 1);
    setenv("FONTCONFIG_FILE", fontConfig.c_str(), 1);

    void* gtk = dlopen("libgtksmoke.so", RTLD_NOW | RTLD_GLOBAL);
    if (!gtk) {
        fprintf(stderr, "Cannot load GTK smoke: %s\n", dlerror());
        return;
    }
    const auto run = reinterpret_cast<int (*)()>(dlsym(gtk, "smoke_main"));
    if (!run) {
        fprintf(stderr, "Cannot find GTK smoke entry: %s\n", dlerror());
        return;
    }
    const int status = run();
    printf("GTK EXIT %d\n", status);
}
