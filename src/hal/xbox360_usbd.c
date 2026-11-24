#include "xbox360_usbd.h"
#include "../transport/bt_usb_transport.h"

#ifdef __XSDK__
/*
 * Xbox 360 Kernel HAL implementation (Camada 1)
 * Built with xenosworks/xsdk (xsdk_add_plugin, -msoft-float -mno-altivec).
 */
#include <xsdk/kernel.h>
#include <string.h>

typedef x360_usb_device_descriptor_t *(*usbd_get_device_desc_fn)(void *dev_handle);
typedef x360_usb_endpoint_descriptor_t *(*usbd_get_ep_desc_fn)(void *dev_handle, int index, int xfer_type, int dir);
typedef NTSTATUS (*usbd_add_dev_complete_fn)(void *dev_handle, int status);
typedef NTSTATUS (*usbd_remove_dev_complete_fn)(void *dev_handle);
typedef NTSTATUS (*usbd_open_default_ep_fn)(void *dev_handle, uint32_t *endpoint);
typedef NTSTATUS (*usbd_open_ep_fn)(void *dev_handle, int xfer_type, int ep_addr,
                                    int max_pkt_len, int interval, uint32_t *endpoint);
typedef NTSTATUS (*usbd_queue_async_xfer_fn)(void *dev_handle, void *trb);
typedef NTSTATUS (*usbd_close_ep_fn)(void *dev_handle, uint32_t *endpoint);

static usbd_get_device_desc_fn     p_UsbdGetDeviceDescriptor    = NULL;
static usbd_get_ep_desc_fn         p_UsbdGetEndpointDescriptor  = NULL;
static usbd_add_dev_complete_fn    p_UsbdAddDeviceComplete      = NULL;
static usbd_remove_dev_complete_fn p_UsbdRemoveDeviceComplete   = NULL;
static usbd_open_default_ep_fn     p_UsbdOpenDefaultEndpoint    = NULL;
static usbd_open_ep_fn             p_UsbdOpenEndpoint           = NULL;
static usbd_queue_async_xfer_fn    p_UsbdQueueAsyncTransfer     = NULL;
static usbd_close_ep_fn            p_UsbdQueueCloseEndpoint     = NULL;
static usbd_close_ep_fn            p_UsbdQueueCloseDefaultEp    = NULL;

typedef struct {
    void                   *dev_handle;
    x360_usb_control_trb_t  ctrl_trb;
    x360_usb_trb_t          intr_trb;
    bt_usb_transport_t      transport;
    uint8_t                 ctrl_data[260];
} x360_bt_dev_ctx_t;

static x360_bt_dev_ctx_t g_bt_ctx;

static int32_t x360_ctrl_complete_cb(uint32_t dev_handle, int32_t status) {
    (void)dev_handle;
    (void)status;
    return 0;
}

static int32_t x360_intr_complete_cb(uint32_t dev_handle, int32_t status) {
    (void)dev_handle;
    if (status == 0 && g_bt_ctx.intr_trb.buffer && g_bt_ctx.intr_trb.length > 0) {
        /*
         * NOTE: Runs in USBD DPC context (no FPU/VMX allowed).
         * bt_usb_transport_on_intr_in uses only integer/byte operations.
         */
        bt_usb_transport_on_intr_in(&g_bt_ctx.transport,
                                    (const uint8_t *)g_bt_ctx.intr_trb.buffer,
                                    (size_t)g_bt_ctx.intr_trb.length);
    }
    return 0;
}

static int x360_hal_submit_control_out(void *ctx, uint8_t bmRequestType, uint8_t bRequest,
                                       uint16_t wValue, uint16_t wIndex,
                                       const uint8_t *data, uint16_t length) {
    x360_bt_dev_ctx_t *c = (x360_bt_dev_ctx_t *)ctx;
    if (!c || !p_UsbdQueueAsyncTransfer || length > sizeof(c->ctrl_data)) {
        return -1;
    }

    if (length > 0 && data) {
        memcpy(c->ctrl_data, data, length);
    }

    c->ctrl_trb.packet.bmRequestType = bmRequestType;
    c->ctrl_trb.packet.bRequest      = bRequest;
    c->ctrl_trb.packet.wValue        = x360_bswap16(wValue);
    c->ctrl_trb.packet.wIndex        = x360_bswap16(wIndex);
    c->ctrl_trb.packet.wLength       = x360_bswap16(length);
    c->ctrl_trb.trb.buffer           = (length > 0) ? c->ctrl_data : NULL;
    c->ctrl_trb.trb.length           = length;
    c->ctrl_trb.trb.flags            = 1;
    c->ctrl_trb.trb.callback         = (uint32_t)(uintptr_t)x360_ctrl_complete_cb;
    c->ctrl_trb.trb.savedEndpoint    = c->ctrl_trb.trb.endpoint;

    NTSTATUS st = p_UsbdQueueAsyncTransfer(c->dev_handle, &c->ctrl_trb);
    return (st < 0) ? -2 : 0;
}

static int x360_hal_submit_intr_in(void *ctx, uint8_t *buf, uint16_t max_len) {
    x360_bt_dev_ctx_t *c = (x360_bt_dev_ctx_t *)ctx;
    if (!c || !p_UsbdQueueAsyncTransfer) {
        return -1;
    }

    c->intr_trb.buffer        = buf;
    c->intr_trb.length        = max_len;
    c->intr_trb.flags         = 1;
    c->intr_trb.callback      = (uint32_t)(uintptr_t)x360_intr_complete_cb;
    c->intr_trb.savedEndpoint = c->intr_trb.endpoint;

    NTSTATUS st = p_UsbdQueueAsyncTransfer(c->dev_handle, &c->intr_trb);
    return (st < 0) ? -2 : 0;
}

int x360_usbd_resolve_ordinals(void) {
    HANDLE krnl = 0;
    XexGetModuleHandle("xboxkrnl.exe", &krnl);
    if (!krnl) {
        return -1;
    }

    XexGetProcedureAddress(krnl, XBOX_ORD_USBD_GET_DEVICE_DESCRIPTOR,   (void **)&p_UsbdGetDeviceDescriptor);
    XexGetProcedureAddress(krnl, XBOX_ORD_USBD_GET_ENDPOINT_DESCRIPTOR, (void **)&p_UsbdGetEndpointDescriptor);
    XexGetProcedureAddress(krnl, XBOX_ORD_USBD_ADD_DEVICE_COMPLETE,     (void **)&p_UsbdAddDeviceComplete);
    XexGetProcedureAddress(krnl, XBOX_ORD_USBD_REMOVE_DEVICE_COMPLETE,  (void **)&p_UsbdRemoveDeviceComplete);
    XexGetProcedureAddress(krnl, XBOX_ORD_USBD_OPEN_DEFAULT_ENDPOINT,   (void **)&p_UsbdOpenDefaultEndpoint);
    XexGetProcedureAddress(krnl, XBOX_ORD_USBD_OPEN_ENDPOINT,           (void **)&p_UsbdOpenEndpoint);
    XexGetProcedureAddress(krnl, XBOX_ORD_USBD_QUEUE_ASYNC_TRANSFER,    (void **)&p_UsbdQueueAsyncTransfer);
    XexGetProcedureAddress(krnl, XBOX_ORD_USBD_QUEUE_CLOSE_ENDPOINT,    (void **)&p_UsbdQueueCloseEndpoint);
    XexGetProcedureAddress(krnl, XBOX_ORD_USBD_QUEUE_CLOSE_DEFAULT_EP,  (void **)&p_UsbdQueueCloseDefaultEp);

    if (!p_UsbdGetDeviceDescriptor || !p_UsbdGetEndpointDescriptor ||
        !p_UsbdAddDeviceComplete || !p_UsbdOpenDefaultEndpoint ||
        !p_UsbdOpenEndpoint || !p_UsbdQueueAsyncTransfer) {
        return -2;
    }
    return 0;
}

int x360_usbd_attach_bt_device(void *dev_handle, uint8_t interface_num) {
    if (!dev_handle) {
        return -1;
    }

    memset(&g_bt_ctx, 0, sizeof(g_bt_ctx));
    g_bt_ctx.dev_handle = dev_handle;

    x360_usb_device_descriptor_t *dd = p_UsbdGetDeviceDescriptor(dev_handle);
    x360_usb_endpoint_descriptor_t *ep_intr = p_UsbdGetEndpointDescriptor(
        dev_handle, 0, BT_USB_EP_TYPE_INTERRUPT, BT_USB_DIR_IN);
    if (!dd || !ep_intr) {
        return -2;
    }

    p_UsbdAddDeviceComplete(dev_handle, 0);

    if (p_UsbdOpenDefaultEndpoint(dev_handle, (uint32_t *)&g_bt_ctx.ctrl_trb) < 0) {
        return -3;
    }

    uint16_t intr_mps = x360_bswap16(ep_intr->wMaxPacketSize) & 0x7FFu;
    if (p_UsbdOpenEndpoint(dev_handle,
                           BT_USB_EP_TYPE_INTERRUPT,
                           ep_intr->bEndpointAddress,
                           intr_mps,
                           ep_intr->bInterval,
                           (uint32_t *)&g_bt_ctx.intr_trb) < 0) {
        return -4;
    }

    bt_usb_device_info_t info;
    memset(&info, 0, sizeof(info));
    info.vid           = x360_bswap16(dd->idVendor);
    info.pid           = x360_bswap16(dd->idProduct);
    info.interface_num = interface_num;
    info.has_intr_in   = true;
    info.intr_in.bEndpointAddress = ep_intr->bEndpointAddress;
    info.intr_in.bmAttributes     = ep_intr->bmAttributes;
    info.intr_in.wMaxPacketSize   = intr_mps;
    info.intr_in.bInterval        = ep_intr->bInterval;

    bt_usb_hal_ops_t hal = {
        .ctx = &g_bt_ctx,
        .submit_control_out = x360_hal_submit_control_out,
        .submit_intr_in     = x360_hal_submit_intr_in
    };

    if (bt_usb_transport_init(&g_bt_ctx.transport, &info, &hal) != 0) {
        return -5;
    }
    return bt_usb_transport_start_init(&g_bt_ctx.transport);
}

#endif /* __XSDK__ */
