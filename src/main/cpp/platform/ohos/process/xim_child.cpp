#include "xim_process.h"

#include <AbilityKit/native_child_process.h>
#include <xcb-imdkit/encoding.h>
#include <xcb-imdkit/imdkit.h>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <sstream>
#include <sys/socket.h>
#include <unistd.h>

namespace {
struct XimServer {
    int fd;
    xcb_im_input_context_t* focused = nullptr;
    xcb_window_t lastNotice = XCB_WINDOW_NONE;

    bool Notify(uint32_t type, xcb_window_t xid) const {
        const hgs_xim_notice notice{type, xid};
        ssize_t count;
        do { count = send(fd, &notice, sizeof(notice), MSG_NOSIGNAL); } while (count < 0 && errno == EINTR);
        return count == sizeof(notice);
    }

    xcb_window_t ActiveWindow() const {
        if (!focused) return XCB_WINDOW_NONE;
        const auto mask = xcb_im_input_context_get_preedit_attr_mask(focused);
        const auto spot = xcb_im_input_context_get_preedit_attr(focused)->spot_location;
        if (!(mask & XCB_XIM_XNSpotLocation_MASK) || (!spot.x && !spot.y)) return XCB_WINDOW_NONE;
        const auto client = xcb_im_input_context_get_client_window(focused);
        return client ? client : xcb_im_input_context_get_focus_window(focused);
    }

    static void Observe(xcb_im_t* im, xcb_im_client_t*, xcb_im_input_context_t* ic,
                        const xcb_im_packet_header_fr_t* header, void*, void* arg, void* data) {
        auto& server = *static_cast<XimServer*>(data);
        switch (header->major_opcode) {
            case XCB_XIM_SET_IC_FOCUS:
                server.focused = ic;
                break;
            case XCB_XIM_UNSET_IC_FOCUS:
            case XCB_XIM_DESTROY_IC:
                if (server.focused == ic) server.focused = nullptr;
                break;
            case XCB_XIM_FORWARD_EVENT:
                if (ic && arg) xcb_im_forward_event(im, ic, static_cast<xcb_key_press_event_t*>(arg));
                return;
            case XCB_XIM_SET_IC_VALUES:
                break;
            default:
                return;
        }
        const auto xid = server.ActiveWindow();
        if (xid != server.lastNotice && server.Notify(HGS_XIM_FOCUS, xid)) server.lastNotice = xid;
    }

    bool ReadCommit(xcb_im_t* im) const {
        hgs_xim_commit message{};
        ssize_t count;
        do { count = recv(fd, &message, sizeof(message), MSG_TRUNC); } while (count < 0 && errno == EINTR);
        if (count <= 0) return false;
        constexpr auto header = static_cast<ssize_t>(offsetof(hgs_xim_commit, text));
        if (count <= header || count > static_cast<ssize_t>(sizeof(message)) ||
            !message.xid || message.xid != ActiveWindow()) return true;
        size_t length = 0;
        char* compound = xcb_utf8_to_compound_text(message.text, count - header, &length);
        if (compound && length && length <= UINT16_MAX)
            xcb_im_commit_string(im, focused, XCB_XIM_LOOKUP_CHARS, compound, length, 0);
        free(compound);
        return true;
    }
};

void Run(int fd, const std::string& display, const std::string& authority, const std::string& name) {
    if (setenv("XAUTHORITY", authority.c_str(), 1) != 0) return;
    int screenNumber = 0;
    auto* connection = xcb_connect(display.c_str(), &screenNumber);
    if (xcb_connection_has_error(connection)) {
        xcb_disconnect(connection);
        return;
    }
    auto screen = xcb_setup_roots_iterator(xcb_get_setup(connection));
    for (int i = 0; screen.rem && i < screenNumber; ++i) xcb_screen_next(&screen);
    if (!screen.rem) {
        xcb_disconnect(connection);
        return;
    }
    const auto window = xcb_generate_id(connection);
    xcb_create_window(connection, XCB_COPY_FROM_PARENT, window, screen.data->root,
                      0, 0, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen.data->root_visual, 0, nullptr);
    uint32_t style = XCB_IM_PreeditPosition | XCB_IM_StatusNothing;
    xcb_im_styles_t styles{1, &style};
    char compoundText[] = "COMPOUND_TEXT";
    char* encoding = compoundText;
    xcb_im_encodings_t encodings{1, &encoding};
    XimServer server{fd};
    auto* im = xcb_im_create(connection, screenNumber, window, name.c_str(), XCB_IM_ALL_LOCALES,
                             &styles, nullptr, nullptr, &encodings, 0, XimServer::Observe, &server);
    if (!im) {
        xcb_disconnect(connection);
        return;
    }
    xcb_im_set_use_sync_mode(im, false);
    const bool opened = xcb_im_open_im(im);
    if (opened) {
        xcb_compound_text_init();
        xcb_flush(connection);
        if (server.Notify(HGS_XIM_READY, XCB_WINDOW_NONE)) {
            pollfd channels[] = {{fd, POLLIN, 0}, {xcb_get_file_descriptor(connection), POLLIN, 0}};
            bool commitReady = false;
            while (!xcb_connection_has_error(connection)) {
                while (auto* event = xcb_poll_for_event(connection)) {
                    xcb_im_filter_event(im, event);
                    free(event);
                }
                if (commitReady && !server.ReadCommit(im)) break;
                if (xcb_connection_has_error(connection) || xcb_flush(connection) <= 0) break;
                int ready = poll(channels, 2, -1);
                commitReady = false;
                if (ready < 0 && errno == EINTR) continue;
                if (ready <= 0 || (channels[0].revents & (POLLHUP | POLLERR | POLLNVAL)) ||
                    (channels[1].revents & (POLLHUP | POLLERR | POLLNVAL))) break;
                commitReady = (channels[0].revents & POLLIN) != 0;
            }
        }
        if (!xcb_connection_has_error(connection)) xcb_im_close_im(im);
    }
    xcb_im_destroy(im);
    xcb_disconnect(connection);
}
}

extern "C" __attribute__((visibility("default"))) void Main(NativeChildProcess_Args args) {
    for (auto* node = args.fdList.head; node; node = node->next) {
        if (!node->fdName || std::strcmp(node->fdName, "xim") != 0 || node->fd < 0) continue;
        std::istringstream params(args.entryParams ? args.entryParams : "");
        std::string display, authority, name;
        if (std::getline(params, display) && std::getline(params, authority) &&
            std::getline(params, name) && !display.empty() && !authority.empty() && !name.empty()) {
            Run(node->fd, display, authority, name);
        }
        close(node->fd);
        return;
    }
}
