// data_device_stub.cpp — wl_data_device_manager 最小 stub (阶段 P1.4 前置)
//
// 为什么必须有: GTK/GDK 的 wayland 后端把 wl_seat 的设备初始化**挂起**,
// 直到 registry 里同时出现 "wl_compositor" 与 "wl_data_device_manager"
// (gdkdisplay-wayland.c: required_device_manager_globals → SeatAddedClosure
// → seat_added_closure_run 创建 GdkSeat 并 get_pointer/get_keyboard)。
// 没有 data_device_manager global → GdkSeat 永远为 NULL → GDK 永远不调
// get_pointer → 合成器侧 "无 wl_pointer resource" → 所有指针/键盘输入静默
// 丢失 (wlsmoke 手写客户端自己 bind seat, 不受影响, 因此此前未暴露)。
//
// 本 stub: 只创建 global 与两个 resource, 全部 request 为空实现 —
// 足以解除 GDK 的 gate, 让指针/键盘输入通; 完整剪贴板/DnD 仍是 P1.4。
// 注意: 接口内每个 request 都必须有 handler (允许空函数体), 否则
// libwayland 对 NULL handler 的请求会向客户端发送协议错误并断连。

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

#include <hilog/log.h>

#undef LOG_TAG
#define LOG_TAG "WL_DataDevice"

namespace hgs {
namespace data_device {

// ---- wl_data_source: 空实现 (source 的每个 request 都是 no-op) ----
static void ds_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
static void ds_offer(wl_client*, wl_resource*, const char*) {}
static void ds_set_actions(wl_client*, wl_resource*, uint32_t) {}

// 字段顺序与接口声明一致 (offer, destroy, set_actions)
static const struct wl_data_source_interface kDataSourceImpl = {
    .offer = ds_offer,
    .destroy = ds_destroy,
    .set_actions = ds_set_actions,
};

static void dd_release(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
static void dd_start_drag(wl_client*, wl_resource*, wl_resource*, wl_resource*,
                          wl_resource*, uint32_t) {}
static void dd_set_selection(wl_client*, wl_resource*, wl_resource*, uint32_t) {}

// 字段顺序与接口声明一致 (start_drag, set_selection, release)
static const struct wl_data_device_interface kDataDeviceImpl = {
    .start_drag = dd_start_drag,
    .set_selection = dd_set_selection,
    .release = dd_release,
};

// ---- wl_data_device_manager ----
static void ddm_create_data_source(wl_client* client, wl_resource* resource,
                                   uint32_t id) {
    wl_resource* ds = wl_resource_create(
        client, &wl_data_source_interface,
        wl_resource_get_version(resource), id);
    // 无私有数据, destroy fn 传 nullptr (项目内同类资源的既有写法)
    wl_resource_set_implementation(ds, &kDataSourceImpl, nullptr, nullptr);
}

static void ddm_get_data_device(wl_client* client, wl_resource* resource,
                                uint32_t id, wl_resource* /*seat*/) {
    wl_resource* dd = wl_resource_create(
        client, &wl_data_device_interface,
        wl_resource_get_version(resource), id);
    wl_resource_set_implementation(dd, &kDataDeviceImpl, nullptr, nullptr);
}

static const struct wl_data_device_manager_interface kDataDeviceMgrImpl = {
    .create_data_source = ddm_create_data_source,
    .get_data_device = ddm_get_data_device,
};

static void ddm_unbind(wl_resource* /*r*/) {}

static void ddm_bind(wl_client* client, void* /*data*/, uint32_t version,
                     uint32_t id) {
    wl_resource* res = wl_resource_create(
        client, &wl_data_device_manager_interface, version, id);
    wl_resource_set_implementation(res, &kDataDeviceMgrImpl, nullptr, ddm_unbind);
    OH_LOG_INFO(LOG_APP, "[DataDevice] manager bound v=%{public}u", version);
}

}  // namespace data_device
}  // namespace hgs

// 供 WaylandServer::Start 在注册 globals 时调用 (C 链接避免 name mangling
// 差异; 与 Seat::Register 同款调用形态)
extern "C" void HgsRegisterDataDeviceManager(wl_display* display) {
    wl_global_create(display, &wl_data_device_manager_interface, 3,
                     nullptr, hgs::data_device::ddm_bind);
}
