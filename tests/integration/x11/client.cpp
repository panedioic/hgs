#include <AbilityKit/native_child_process.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <poll.h>
#include <sstream>
#include <string>

extern "C" __attribute__((visibility("default"))) void Main(NativeChildProcess_Args args) {
    std::istringstream params(args.entryParams ? args.entryParams : "");
    std::string display, auth, log, title;
    if (!std::getline(params, display) || !std::getline(params, auth) ||
        !std::getline(params, log) || !std::getline(params, title)) return;
    FILE* report = fopen(log.c_str(), "w");
    if (!report) return;
    setvbuf(report, nullptr, _IOLBF, 0);

    setenv("XAUTHORITY", "/dev/null", 1);
    if (Display* anonymous = XOpenDisplay(display.c_str())) {
        fprintf(report, "ERROR unauthenticated connection accepted\n");
        XCloseDisplay(anonymous);
        fclose(report);
        return;
    }
    fprintf(report, "AUTH_REJECTED\n");
    setenv("XAUTHORITY", auth.c_str(), 1);
    Display* connection = XOpenDisplay(display.c_str());
    if (!connection) {
        fprintf(report, "ERROR authorized connection failed\n");
        fclose(report);
        return;
    }

    constexpr int width = 320, height = 240;
    constexpr unsigned long color = 0x2060c0;
    Window window = XCreateSimpleWindow(connection, DefaultRootWindow(connection), 20, 20,
                                        width, height, 0, 0, color);
    XStoreName(connection, window, title.c_str());
    XSelectInput(connection, window, ExposureMask | StructureNotifyMask | KeyPressMask |
                                    KeyReleaseMask | ButtonPressMask | ButtonReleaseMask);
    XMapWindow(connection, window);
    XFlush(connection);
    fprintf(report, "CONNECTED\n");

    bool drawn = false, press = false, release = false, buttonDown = false, buttonUp = false;
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < end && !(drawn && press && release && buttonDown && buttonUp)) {
        if (!XPending(connection)) {
            pollfd events{ConnectionNumber(connection), POLLIN, 0};
            if (poll(&events, 1, 100) <= 0) continue;
            if (events.revents & (POLLERR | POLLHUP)) break;
            if (!XPending(connection)) continue;
        }
        XEvent event{};
        XNextEvent(connection, &event);
        if (event.type == Expose && !drawn) {
            GC gc = XCreateGC(connection, window, 0, nullptr);
            XSetForeground(connection, gc, color);
            XFillRectangle(connection, window, gc, 0, 0, width, height);
            XImage* image = XGetImage(connection, window, 10, 10, 1, 1, AllPlanes, ZPixmap);
            drawn = image && XGetPixel(image, 0, 0) == color;
            if (image) XDestroyImage(image);
            XFreeGC(connection, gc);
            fprintf(report, "FRAME %s\n", drawn ? "OK" : "BAD");
        } else if (event.type == KeyPress || event.type == KeyRelease) {
            const auto sym = XLookupKeysym(&event.xkey, 0);
            fprintf(report, "KEY %s %lu\n", event.type == KeyPress ? "DOWN" : "UP", sym);
            if (sym == XK_a) {
                if (event.type == KeyPress) press = true;
                else release = true;
            }
        } else if (event.type == ButtonPress || event.type == ButtonRelease) {
            fprintf(report, "BUTTON %s %u\n", event.type == ButtonPress ? "DOWN" : "UP",
                    event.xbutton.button);
            if (event.xbutton.button == 1) {
                if (event.type == ButtonPress) buttonDown = true;
                else buttonUp = true;
            }
        }
    }

    XDestroyWindow(connection, window);
    XCloseDisplay(connection);
    fprintf(report, "%s\n", drawn && press && release && buttonDown && buttonUp ? "PASS" : "FAIL");
    fclose(report);
}
