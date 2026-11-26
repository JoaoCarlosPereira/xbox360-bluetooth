#include "xbox360_usbd.h"
#include "../transport/bt_usb_transport.h"

#ifdef __XSDK__
/*
 * Xbox 360 Kernel HAL implementation (Camada 1)
 * Built with xenosworks/xsdk (xsdk_add_plugin, -msoft-float -mno-altivec).
 */
#include <xsdk/plugin.h>
#include "../manager/bt_trace.h"
#define xsdk_plugin_log bt_trace
#include <string.h>
#include <stddef.h>

typedef x360_usb_device_descriptor_t *(*usbd_get_device_desc_fn)(void *dev_handle);
typedef x360_usb_endpoint_descriptor_t *(*usbd_get_ep_desc_fn)(void *dev_handle, int index, int xfer_type, int dir);
typedef NTSTATUS (*usbd_add_dev_complete_fn)(void *dev_handle, int status);
typedef NTSTATUS (*usbd_remove_dev_complete_fn)(void *dev_handle);
typedef NTSTATUS (*usbd_open_default_ep_fn)(void *dev_handle, uint32_t *endpoint);
typedef NTSTATUS (*usbd_open_ep_fn)(void *dev_handle, int xfer_type, int ep_addr,
                                    int max_pkt_len, int interval, uint32_t *endpoint);
typedef void (*usbd_queue_async_xfer_fn)(void *dev_handle, void *trb);
typedef void (*usbd_close_ep_fn)(void *dev_handle, uint32_t *endpoint);

static usbd_get_device_desc_fn     p_UsbdGetDeviceDescriptor    = NULL;
static usbd_get_ep_desc_fn         p_UsbdGetEndpointDescriptor  = NULL;
static usbd_add_dev_complete_fn    p_UsbdAddDeviceComplete      = NULL;
static usbd_remove_dev_complete_fn p_UsbdRemoveDeviceComplete   = NULL;
static usbd_open_default_ep_fn     p_UsbdOpenDefaultEndpoint    = NULL;
static usbd_open_ep_fn             p_UsbdOpenEndpoint           = NULL;
static usbd_queue_async_xfer_fn    p_UsbdQueueAsyncTransfer     = NULL;
static usbd_close_ep_fn            p_UsbdQueueCloseEndpoint     = NULL;
static usbd_close_ep_fn            p_UsbdQueueCloseDefaultEp    = NULL;

/* Retail 17559: register/remove own the two link fields, and dispatch invokes
 * the callbacks at +0x0C, +0x10 and +0x14 (see docs/xbox360-usb.md).
 */
typedef struct usb_driver_17559 usb_driver_17559_t;
struct usb_driver_17559 {
    uint32_t links[2];
    uint32_t reserved;
    void (*add)(void *device);
    void (*remove)(void *device);
    uint32_t (*match)(const uint8_t *candidate, const x360_usb_device_descriptor_t *device,
                      const usb_driver_17559_t *driver);
    void *optional_event;
};

_Static_assert(offsetof(usb_driver_17559_t, add) == 0x0C, "USB add callback offset");
_Static_assert(offsetof(usb_driver_17559_t, remove) == 0x10, "USB remove callback offset");
_Static_assert(offsetof(usb_driver_17559_t, match) == 0x14, "USB match callback offset");
_Static_assert(offsetof(usb_driver_17559_t, optional_event) == 0x18, "USB event callback offset");
_Static_assert(sizeof(usb_driver_17559_t) == 0x1C, "USB object layout");

typedef struct {
    void *device;
    x360_usb_control_trb_t control;
    x360_usb_trb_t interrupt;
    x360_usb_trb_t bulk_in;
    x360_usb_trb_t bulk_out;
    x360_usb_trb_t close;
    bt_usb_transport_t transport;
    uint8_t command[HCI_MAX_CMD_SIZE];
    uint8_t bulk_rx[512];
    uint8_t acl_ring[8][512];
    uint16_t acl_ring_n[8];
    uint32_t acl_prod;
    uint32_t acl_cons;
    uint8_t bulk_tx[516];
    uint32_t bulk_in_asked;
    uint32_t bulk_report;
    uint32_t in_handle;
    uint32_t out_handle;
    uint8_t acl_q[4][516];
    uint16_t acl_q_len[4];
    uint8_t acl_q_head;
    uint8_t acl_q_tail;
    uint8_t acl_q_count;
    size_t event_length;
    bool control_pending;
    bool bulk_out_pending;
    bool removing;
} adapter_t;

static adapter_t adapter;
static x360_acl_rx_fn acl_rx;
/* These counters are sampled by the plugin thread; USB callbacks do no file I/O. */
static uint32_t adapter_status;
static uint32_t diagnostic_error;
static uint32_t matches;
static uint32_t wrong_matches;   /* wireless-class HCI interface with another VID/PID */
static uint32_t match_calls;     /* every call from the kernel selector, accepted or not */
static uint32_t match_reason;
static uint32_t match_seen;
static uint32_t adapter_removed; /* 1 after the UGREEN left, cleared on the next add */

static void set_status(uint32_t status, uint32_t error) {
    __atomic_store_n(&diagnostic_error, error, __ATOMIC_RELAXED);
    __atomic_store_n(&adapter_status, status, __ATOMIC_RELEASE);
}

uint32_t x360_usbd_adapter_status(void) {
    return __atomic_load_n(&adapter_status, __ATOMIC_ACQUIRE);
}

uint32_t x360_usbd_wrong_adapter_hits(void) {
    return __atomic_load_n(&wrong_matches, __ATOMIC_RELAXED);
}

bool x360_usbd_adapter_removed(void) {
    return __atomic_load_n(&adapter_removed, __ATOMIC_ACQUIRE) != 0;
}

void x360_usbd_log_changes(void) {
    static uint32_t last_status = UINT32_MAX;
    static uint32_t last_matches;
    static uint32_t last_wrong;
    const uint32_t wrong = x360_usbd_wrong_adapter_hits();
    if (wrong != last_wrong) {
        xsdk_plugin_log("usb: refused wireless-class adapter with another VID/PID hits=%u", wrong);
        last_wrong = wrong;
    }
    const uint32_t status = x360_usbd_adapter_status();
    const uint32_t n = __atomic_load_n(&matches, __ATOMIC_RELAXED);
    if (n != last_matches) {
        xsdk_plugin_log("usb: matched UGREEN 2B89:8761 HCI interface hits=%u", n);
        last_matches = n;
    }
    static uint32_t last_calls;
    const uint32_t calls = __atomic_load_n(&match_calls, __ATOMIC_ACQUIRE);
    if (calls != last_calls) {
        xsdk_plugin_log("usb: selector calls=%u reason=%u seen=%08X", calls,
                        __atomic_load_n(&match_reason, __ATOMIC_RELAXED),
                        __atomic_load_n(&match_seen, __ATOMIC_RELAXED));
        last_calls = calls;
    }
    if (status != last_status) {
        xsdk_plugin_log("usb: adapter stage=%u detail=%08X", status,
                        __atomic_load_n(&diagnostic_error, __ATOMIC_RELAXED));
        last_status = status;
    }
}

static int32_t control_done(void *trb, int32_t status);
static int32_t interrupt_done(void *trb, int32_t status);
static int32_t bulk_in_done(void *trb, int32_t status);
static int32_t bulk_out_done(void *trb, int32_t status);

static void acl_push(const uint8_t *data, uint32_t n) {
    if (!data || !n) return;
    if (n > sizeof adapter.acl_ring[0]) n = sizeof adapter.acl_ring[0];
    const uint32_t prod = adapter.acl_prod;
    const uint32_t cons = __atomic_load_n(&adapter.acl_cons, __ATOMIC_ACQUIRE);
    if (prod - cons >= 8u) {
        /* O mais novo substitui o último ainda não lido. O consumidor está 8 atrás. */
        const uint32_t slot = (prod - 1u) & 7u;
        memcpy(adapter.acl_ring[slot], data, n);
        adapter.acl_ring_n[slot] = (uint16_t)n;
        return;
    }
    const uint32_t slot = prod & 7u;
    memcpy(adapter.acl_ring[slot], data, n);
    adapter.acl_ring_n[slot] = (uint16_t)n;
    __atomic_store_n(&adapter.acl_prod, prod + 1u, __ATOMIC_RELEASE);
}

static void acl_drain(void) {
    uint32_t cons = adapter.acl_cons;
    uint32_t prod = __atomic_load_n(&adapter.acl_prod, __ATOMIC_ACQUIRE);
    while (cons != prod && acl_rx) {
        const uint32_t slot = cons & 7u;
        uint16_t n = adapter.acl_ring_n[slot];
        uint8_t local[512];
        if (n > sizeof local) n = sizeof local;
        memcpy(local, adapter.acl_ring[slot], n);
        cons++;
        __atomic_store_n(&adapter.acl_cons, cons, __ATOMIC_RELEASE);
        acl_rx(local, n);
        prod = __atomic_load_n(&adapter.acl_prod, __ATOMIC_ACQUIRE);
    }
}

static void arm_bulk_in(void) {
    if (adapter.removing || !adapter.in_handle || !p_UsbdQueueAsyncTransfer) return;
    adapter.bulk_in.endpoint = adapter.in_handle;
    adapter.bulk_in.buffer = adapter.bulk_rx;
    adapter.bulk_in.length = sizeof adapter.bulk_rx;
    adapter.bulk_in_asked = sizeof adapter.bulk_rx;
    adapter.bulk_in.flags = 1;
    adapter.bulk_in.callback = (uint32_t)(uintptr_t)bulk_in_done;
    adapter.bulk_in.savedEndpoint = adapter.in_handle;
    p_UsbdQueueAsyncTransfer(adapter.device, &adapter.bulk_in);
}

static void pump_bulk_out(void) {
    while (adapter.acl_q_count && adapter.out_handle && !adapter.removing) {
        if (__atomic_load_n(&adapter.bulk_out_pending, __ATOMIC_ACQUIRE)) return;
        const uint16_t n = adapter.acl_q_len[adapter.acl_q_head];
        memcpy(adapter.bulk_tx, adapter.acl_q[adapter.acl_q_head], n);
        adapter.acl_q_head = (uint8_t)((adapter.acl_q_head + 1u) % 4u);
        adapter.acl_q_count--;
        adapter.bulk_out.endpoint = adapter.out_handle;
        adapter.bulk_out.buffer = adapter.bulk_tx;
        adapter.bulk_out.length = n;
        adapter.bulk_out.flags = 1;
        adapter.bulk_out.callback = (uint32_t)(uintptr_t)bulk_out_done;
        adapter.bulk_out.savedEndpoint = adapter.out_handle;
        __atomic_store_n(&adapter.bulk_out_pending, true, __ATOMIC_RELEASE);
        p_UsbdQueueAsyncTransfer(adapter.device, &adapter.bulk_out);
    }
}

static void open_bulk_pipes(void) {
    const bt_usb_device_info_t *dev = &adapter.transport.dev;
    if (!dev->has_bulk_in || !dev->has_bulk_out) {
        __atomic_store_n(&adapter.bulk_report, 2u, __ATOMIC_RELEASE);
        return;
    }
    const NTSTATUS in = p_UsbdOpenEndpoint(adapter.device, BT_USB_EP_TYPE_BULK,
        dev->bulk_in.bEndpointAddress, dev->bulk_in.wMaxPacketSize, 0, &adapter.bulk_in.endpoint);
    const NTSTATUS out = p_UsbdOpenEndpoint(adapter.device, BT_USB_EP_TYPE_BULK,
        dev->bulk_out.bEndpointAddress, dev->bulk_out.wMaxPacketSize, 0, &adapter.bulk_out.endpoint);
    /* The open itself runs on the USB completion. The line is printed by the plugin thread. */
    adapter.in_handle = NT_SUCCESS(in) ? adapter.bulk_in.endpoint : 0;
    adapter.out_handle = NT_SUCCESS(out) ? adapter.bulk_out.endpoint : 0;
    __atomic_store_n(&adapter.bulk_report,
                     NT_SUCCESS(in) && NT_SUCCESS(out) ? 1u : (0x80000000u | (uint32_t)in),
                     __ATOMIC_RELEASE);
    if (adapter.in_handle && adapter.out_handle) arm_bulk_in();
}

void x360_usbd_service(void) {
    const uint32_t report = __atomic_exchange_n(&adapter.bulk_report, 0, __ATOMIC_ACQ_REL);
    if (report == 1u)
        xsdk_plugin_log("usb: bulk endpoints open in=%08X out=%08X",
                        adapter.in_handle, adapter.out_handle);
    else if (report == 2u)
        xsdk_plugin_log("usb: bulk endpoints absent");
    else if (report)
        xsdk_plugin_log("usb: bulk open failed status=%08X", report);
    acl_drain();
    pump_bulk_out();
}

void x360_usbd_set_acl_rx(x360_acl_rx_fn fn) {
    acl_rx = fn;
}

int x360_usbd_acl_tx(const uint8_t *data, uint16_t len) {
    if (!data || !len || len > sizeof adapter.bulk_tx || adapter.removing ||
        !adapter.out_handle || adapter.acl_q_count >= 4u) {
        static uint32_t logged;
        const uint32_t why = !adapter.out_handle ? 1u : (adapter.acl_q_count >= 4u ? 2u : 3u);
        if (why != logged) {
            logged = why;
            xsdk_plugin_log("usb: acl tx refused why=%u out=%08X queued=%u len=%u",
                            why, adapter.out_handle, adapter.acl_q_count, len);
        }
        return -1;
    }
    memcpy(adapter.acl_q[adapter.acl_q_tail], data, len);
    adapter.acl_q_len[adapter.acl_q_tail] = len;
    adapter.acl_q_tail = (uint8_t)((adapter.acl_q_tail + 1u) % 4u);
    adapter.acl_q_count++;
    pump_bulk_out();
    return 0;
}

static int submit_control(void *ctx, uint8_t type, uint8_t request, uint16_t value,
                          uint16_t index, const uint8_t *data, uint16_t length) {
    adapter_t *a = ctx;
    if (a->removing || a->control_pending || length > sizeof a->command) return -1;
    if (length) memcpy(a->command, data, length);
    a->control.packet = (x360_usb_setup_packet_t){
        type, request, x360_bswap16(value), x360_bswap16(index), x360_bswap16(length)
    };
    a->control.trb.buffer = length ? a->command : NULL;
    a->control.trb.length = length;
    a->control.trb.flags = 1;
    a->control.trb.callback = (uint32_t)(uintptr_t)control_done;
    a->control.trb.savedEndpoint = a->control.trb.endpoint;
    a->control_pending = true;
    p_UsbdQueueAsyncTransfer(a->device, &a->control);
    return 0;
}

static int submit_interrupt(void *ctx, uint8_t *buffer, uint16_t length) {
    adapter_t *a = ctx;
    if (a->removing) return -1;
    a->interrupt.buffer = buffer;
    a->interrupt.length = length;
    memset(buffer, 0, length);
    a->interrupt.flags = 1;
    a->interrupt.callback = (uint32_t)(uintptr_t)interrupt_done;
    a->interrupt.savedEndpoint = a->interrupt.endpoint;
    p_UsbdQueueAsyncTransfer(a->device, &a->interrupt);
    return 0;
}

static void consume_event(void) {
    if (adapter.control_pending || adapter.removing || !adapter.event_length) return;
    const size_t length = adapter.event_length;
    adapter.event_length = 0;
    bt_usb_transport_on_intr_in(&adapter.transport, adapter.interrupt.buffer, length);
    if (adapter.transport.state == BT_INIT_READY) {
        const hci_local_version_info_t *v = &adapter.transport.local_ver;
        set_status(BT_USB_STAGE_READY, ((uint32_t)v->manufacturer << 16) | v->hci_version);
    } else if (adapter.transport.state == BT_INIT_ERROR) {
        set_status(BT_USB_STAGE_ERROR, (uint32_t)adapter.transport.last_error);
    } else if (adapter.transport.state >= BT_INIT_WAIT_LOCAL_VER) {
        set_status(BT_USB_STAGE_RESPONDED, (uint32_t)adapter.transport.state);
    }
}

static int32_t control_done(void *trb, int32_t status) {
    (void)trb;
    adapter.control_pending = false;
    if (adapter.removing) return 0;
    if (status != 0) {
        set_status(BT_USB_STAGE_ERROR, (uint32_t)status);
        return 0;
    }

    if (adapter.transport.state == BT_INIT_IDLE) {
        const bt_usb_ep_info_t *ep = &adapter.transport.dev.intr_in;
        const NTSTATUS opened = p_UsbdOpenEndpoint(adapter.device, BT_USB_EP_TYPE_INTERRUPT,
            ep->bEndpointAddress, ep->wMaxPacketSize, ep->bInterval, &adapter.interrupt.endpoint);
        if (!NT_SUCCESS(opened)) {
            set_status(BT_USB_STAGE_ERROR, (uint32_t)opened);
        } else {
            set_status(BT_USB_STAGE_HCI, 0);
            open_bulk_pipes();
            const int rc = bt_usb_transport_start_init(&adapter.transport);
            if (rc != 0) set_status(BT_USB_STAGE_ERROR, (uint32_t)rc);
        }
    } else {
        consume_event();
    }
    return 0;
}

static int32_t interrupt_done(void *trb, int32_t status) {
    (void)trb;
    if (adapter.removing) return 0;
    if (status != 0 || adapter.interrupt.length == 0 ||
        adapter.interrupt.length > sizeof adapter.transport.evt_rx_chunk) {
        set_status(BT_USB_STAGE_ERROR, status ? (uint32_t)status : 0xE0010001u);
        return 0;
    }
    adapter.event_length = adapter.interrupt.length;
    consume_event();
    return 0;
}

static int bulk_in_depth;
static int bulk_in_again;

static int32_t bulk_in_done(void *trb, int32_t status) {
    (void)trb;
    if (adapter.removing) return 0;
    if (status == 0) {
        uint32_t n = adapter.bulk_in.length;
        if (n == 0 || n > adapter.bulk_in_asked) n = adapter.bulk_in_asked;
        acl_push(adapter.bulk_rx, n);
    }
    /* Se a fila USB completar na hora, não reentra no mesmo TRB sem um novo pedido. */
    if (bulk_in_depth) {
        bulk_in_again = 1;
        return 0;
    }
    bulk_in_depth = 1;
    do {
        bulk_in_again = 0;
        arm_bulk_in();
    } while (bulk_in_again && !adapter.removing);
    bulk_in_depth = 0;
    return 0;
}

static int32_t bulk_out_done(void *trb, int32_t status) {
    (void)trb;
    (void)status;
    __atomic_store_n(&adapter.bulk_out_pending, false, __ATOMIC_RELEASE);
    return 0;
}

static void remember_match(uint32_t reason, uint32_t seen) {
    __atomic_store_n(&match_reason, reason, __ATOMIC_RELAXED);
    __atomic_store_n(&match_seen, seen, __ATOMIC_RELAXED);
    __atomic_add_fetch(&match_calls, 1, __ATOMIC_RELEASE);
}

static uint32_t match_ugreen(const uint8_t *candidate, const x360_usb_device_descriptor_t *device,
                             const usb_driver_17559_t *driver) {
    (void)driver;
    /* reason: 1 null, 2 not an interface, 3 not interface 0, 4 not HCI,
     * 5 bad device descriptor, 6 other VID/PID, 7 already attached, 0 accepted. */
    uint32_t seen = 0;
    uint32_t reason = 1;
    if (candidate && candidate[0] >= sizeof(x360_usb_interface_descriptor_t)) {
        seen = (uint32_t)candidate[1] | ((uint32_t)candidate[5] << 8) |
               ((uint32_t)candidate[6] << 16) | ((uint32_t)candidate[7] << 24);
        reason = 2;
        if (candidate[1] == 4) {
            reason = 3;
            if (candidate[2] == 0 && candidate[3] == 0) {
                reason = 4;
                if (bt_usb_is_hci_interface(candidate[5], candidate[6], candidate[7])) {
                    reason = 5;
                    if (device && device->bLength >= sizeof *device && device->bDescriptorType == 1) {
                        const uint16_t vid = x360_bswap16(device->idVendor);
                        const uint16_t pid = x360_bswap16(device->idProduct);
                        seen = ((uint32_t)vid << 16) | pid;
                        if (vid != 0x2B89 || pid != 0x8761) {
                            __atomic_add_fetch(&wrong_matches, 1, __ATOMIC_RELAXED);
                            reason = 6;
                        } else if (adapter.device) {
                            reason = 7;
                        } else {
                            __atomic_add_fetch(&matches, 1, __ATOMIC_RELAXED);
                            remember_match(0, seen);
                            return 1;
                        }
                    }
                }
            }
        }
    }
    remember_match(reason, seen);
    return 0;
}

static void add_ugreen(void *device) {
    const x360_usb_endpoint_descriptor_t *ep = p_UsbdGetEndpointDescriptor(
        device, 0, BT_USB_EP_TYPE_INTERRUPT, BT_USB_DIR_IN);
    if (adapter.device || !ep || ep->bLength < sizeof *ep || ep->bDescriptorType != 5 ||
        !(ep->bEndpointAddress & 0x80)) {
        set_status(BT_USB_STAGE_ERROR, 0xE0010002u);
        p_UsbdAddDeviceComplete(device, STATUS_UNSUCCESSFUL);
        return;
    }
    const uint16_t packet_size = x360_bswap16(ep->wMaxPacketSize) & 0x7FFu;
    if (!packet_size || packet_size > sizeof adapter.transport.evt_rx_chunk) {
        set_status(BT_USB_STAGE_ERROR, 0xE0010003u);
        p_UsbdAddDeviceComplete(device, STATUS_UNSUCCESSFUL);
        return;
    }

    memset(&adapter, 0, sizeof adapter);
    adapter.device = device;
    __atomic_store_n(&adapter_removed, 0, __ATOMIC_RELEASE);
    bt_usb_device_info_t info = {
        .vid = 0x2B89, .pid = 0x8761, .interface_num = 0, .has_intr_in = true,
        .intr_in = { ep->bEndpointAddress, ep->bmAttributes, packet_size, ep->bInterval }
    };
    const x360_usb_endpoint_descriptor_t *bin = p_UsbdGetEndpointDescriptor(
        device, 0, BT_USB_EP_TYPE_BULK, BT_USB_DIR_IN);
    const x360_usb_endpoint_descriptor_t *bout = p_UsbdGetEndpointDescriptor(
        device, 0, BT_USB_EP_TYPE_BULK, BT_USB_DIR_OUT);
    if (bin && bin->bLength >= sizeof *bin && (bin->bEndpointAddress & 0x80u)) {
        const uint16_t mps = x360_bswap16(bin->wMaxPacketSize) & 0x7FFu;
        if (mps) {
            info.has_bulk_in = true;
            info.bulk_in = (bt_usb_ep_info_t){ bin->bEndpointAddress, bin->bmAttributes, mps, 0 };
        }
    }
    if (bout && bout->bLength >= sizeof *bout && !(bout->bEndpointAddress & 0x80u)) {
        const uint16_t mps = x360_bswap16(bout->wMaxPacketSize) & 0x7FFu;
        if (mps) {
            info.has_bulk_out = true;
            info.bulk_out = (bt_usb_ep_info_t){ bout->bEndpointAddress, bout->bmAttributes, mps, 0 };
        }
    }
    const bt_usb_hal_ops_t hal = { &adapter, submit_control, submit_interrupt };
    if (bt_usb_transport_init(&adapter.transport, &info, &hal) != 0) {
        adapter.device = NULL;
        set_status(BT_USB_STAGE_ERROR, 0xE0010004u);
        p_UsbdAddDeviceComplete(device, STATUS_UNSUCCESSFUL);
        return;
    }

    p_UsbdAddDeviceComplete(device, 0);
    const NTSTATUS opened = p_UsbdOpenDefaultEndpoint(device, &adapter.control.trb.endpoint);
    if (!NT_SUCCESS(opened)) {
        set_status(BT_USB_STAGE_ERROR, (uint32_t)opened);
        return;
    }
    set_status(BT_USB_STAGE_CONFIGURING, ((uint32_t)packet_size << 16) | ep->bEndpointAddress);
    if (submit_control(&adapter, 0, 9, 1, 0, NULL, 0) != 0)
        set_status(BT_USB_STAGE_ERROR, 0xE0010005u);
}

static int32_t close_next(void *trb, int32_t status) {
    (void)trb;
    (void)status;
    void *device = adapter.device;
    uint32_t endpoint = adapter.in_handle;
    if (!endpoint) endpoint = adapter.out_handle;
    if (endpoint) {
        if (adapter.in_handle == endpoint) adapter.in_handle = 0;
        else adapter.out_handle = 0;
        adapter.bulk_in.endpoint = 0;
        adapter.bulk_out.endpoint = 0;
        adapter.close.savedEndpoint = endpoint;
        p_UsbdQueueCloseEndpoint(device, &adapter.close.endpoint);
    } else if ((endpoint = adapter.interrupt.endpoint) != 0) {
        adapter.interrupt.endpoint = 0;
        adapter.close.savedEndpoint = endpoint;
        p_UsbdQueueCloseEndpoint(device, &adapter.close.endpoint);
    } else if ((endpoint = adapter.control.trb.endpoint) != 0) {
        adapter.control.trb.endpoint = 0;
        adapter.close.savedEndpoint = endpoint;
        p_UsbdQueueCloseDefaultEp(device, &adapter.close.endpoint);
    } else {
        adapter.device = NULL;
        p_UsbdRemoveDeviceComplete(device);
        set_status(BT_USB_STAGE_WAITING, 0);
    }
    return 0;
}

static void remove_ugreen(void *device) {
    if (device != adapter.device) {
        p_UsbdRemoveDeviceComplete(device);
        return;
    }
    if (adapter.removing) return;
    adapter.removing = true;
    __atomic_store_n(&adapter_removed, 1, __ATOMIC_RELEASE);
    set_status(BT_USB_STAGE_REMOVING, 0);
    adapter.close.callback = (uint32_t)(uintptr_t)close_next;
    close_next(NULL, 0);
}

static usb_driver_17559_t driver = {
    .add = add_ugreen, .remove = remove_ugreen, .match = match_ugreen
};

typedef int (*usbd_void_fn)(void);
typedef void (*mm_free_physical_memory_fn)(uint32_t type, uint32_t address);

/*
 * Retail 17559 USB reset. Bugcheck and double-registration patches match
 * hiddriver360/hiddriver/main.cpp:1534-1540. The power phase and the driver-list
 * wipe were read from the BTU1 capture of this kernel, not from that source:
 * 0x800D8FC8 is a debug stub, and UsbdDriverEntry empties the driver list at
 * 0x800D8E24 before the host comes back. Only called after the layout guard.
 */
#define USBD_BUGCHECK_A        0x800E05E4u
#define USBD_BUGCHECK_B        0x800DD8E0u
#define USBD_DOUBLE_REG_A      0x800D8F00u
#define USBD_DOUBLE_REG_B      0x800D8EF0u
#define USBD_LIST_WIPE_A       0x800D8E24u
#define USBD_LIST_WIPE_B       0x800D8E2Cu
#define USBD_LIST_WIPE_A_INSN  0x916B0000u /* stw r11, 0(r11) */
#define USBD_LIST_WIPE_B_INSN  0x916B0004u /* stw r11, 4(r11) */
#define USBD_POWER_PHASE_17559 0x800D8F10u
#define USBD_POWER_PROLOGUE    0x7D8802A6u /* mflr r12 */
#define USBD_DRIVER_ENTRY      0x800D8D08u
#define USBD_PHYSICAL_PAGE     0x801A8098u
#define XBOX_ORD_MM_FREE_PHYSICAL_MEMORY 189u

static void (*register_driver_fn)(usb_driver_17559_t *);
static HANDLE usb_krnl;

/* r3 and r4 are unused. r5 == 0 stops both host controllers. */
typedef void (*usbd_power_phase_fn)(uint32_t unused0, uint32_t unused1, uint32_t phase);

/* No logging here. bt_trace writes to the USB disk, and a write while the bus
 * is down blocked the 22:00 boot for about 15 seconds. */
static int usbd_enumerate_silent(void) {
    if (*(volatile uint32_t *)USBD_POWER_PHASE_17559 != USBD_POWER_PROLOGUE)
        return -2;
    if (*(volatile uint32_t *)USBD_LIST_WIPE_A != USBD_LIST_WIPE_A_INSN ||
        *(volatile uint32_t *)USBD_LIST_WIPE_B != USBD_LIST_WIPE_B_INSN)
        return -3;

    *(volatile uint32_t *)USBD_BUGCHECK_A = 0x48000018u;
    *(volatile uint32_t *)USBD_BUGCHECK_B = 0x48000018u;
    *(volatile uint32_t *)USBD_DOUBLE_REG_A = 0x60000000u;
    *(volatile uint32_t *)USBD_DOUBLE_REG_B = 0x60000000u;
    /* DriverEntry would unlink every driver, including the one registered below. */
    *(volatile uint32_t *)USBD_LIST_WIPE_A = 0x60000000u;
    *(volatile uint32_t *)USBD_LIST_WIPE_B = 0x60000000u;

    ((usbd_power_phase_fn)(uintptr_t)USBD_POWER_PHASE_17559)(0, 0, 0);

    int page = 1;
    mm_free_physical_memory_fn free_physical = NULL;
    const NTSTATUS resolved = XexGetProcedureAddress(usb_krnl, XBOX_ORD_MM_FREE_PHYSICAL_MEMORY,
                                                     (void **)&free_physical);
    if (NT_SUCCESS(resolved) && free_physical) {
        free_physical(0, *(volatile uint32_t *)USBD_PHYSICAL_PAGE);
        page = 0;
    }

    register_driver_fn(&driver);
    ((usbd_void_fn)(uintptr_t)USBD_DRIVER_ENTRY)();
    return page;
}

int x360_usbd_resolve_ordinals(void) {
    HANDLE krnl = 0;
    const uint32_t status = XexGetModuleHandle("xboxkrnl.exe", &krnl);
    xsdk_plugin_log("usb: xboxkrnl lookup status=0x%08X found=%u", status, (unsigned)(krnl != 0));
    if (!krnl) return -1;

    void *register_driver = NULL;
    void *unregister_driver = NULL;
#define LOG_EXPORT(ordinal, target) do { \
    const NTSTATUS result = XexGetProcedureAddress(krnl, ordinal, (void **)&target); \
    xsdk_plugin_log("usb: export %u (%s) status=0x%08X found=%u address=%08X", \
                    ordinal, #target, (unsigned)result, (unsigned)(target != NULL), (unsigned)(uintptr_t)target); \
} while (0)

    LOG_EXPORT(XBOX_ORD_USBD_GET_DEVICE_DESCRIPTOR, p_UsbdGetDeviceDescriptor);
    LOG_EXPORT(XBOX_ORD_USBD_GET_ENDPOINT_DESCRIPTOR, p_UsbdGetEndpointDescriptor);
    LOG_EXPORT(XBOX_ORD_USBD_ADD_DEVICE_COMPLETE, p_UsbdAddDeviceComplete);
    LOG_EXPORT(XBOX_ORD_USBD_REMOVE_DEVICE_COMPLETE, p_UsbdRemoveDeviceComplete);
    LOG_EXPORT(XBOX_ORD_USBD_OPEN_DEFAULT_ENDPOINT, p_UsbdOpenDefaultEndpoint);
    LOG_EXPORT(XBOX_ORD_USBD_OPEN_ENDPOINT, p_UsbdOpenEndpoint);
    LOG_EXPORT(XBOX_ORD_USBD_QUEUE_ASYNC_TRANSFER, p_UsbdQueueAsyncTransfer);
    LOG_EXPORT(XBOX_ORD_USBD_QUEUE_CLOSE_ENDPOINT, p_UsbdQueueCloseEndpoint);
    LOG_EXPORT(XBOX_ORD_USBD_QUEUE_CLOSE_DEFAULT_EP, p_UsbdQueueCloseDefaultEp);
    LOG_EXPORT(XBOX_ORD_USBD_REGISTER_DRIVER_OBJECT, register_driver);
    LOG_EXPORT(XBOX_ORD_USBD_UNREGISTER_DRIVER_OBJECT, unregister_driver);
#undef LOG_EXPORT
    if (!p_UsbdGetDeviceDescriptor || !p_UsbdGetEndpointDescriptor ||
        !p_UsbdAddDeviceComplete || !p_UsbdRemoveDeviceComplete || !p_UsbdOpenDefaultEndpoint ||
        !p_UsbdOpenEndpoint || !p_UsbdQueueAsyncTransfer || !p_UsbdQueueCloseEndpoint ||
        !p_UsbdQueueCloseDefaultEp) return -2;

    if (XboxKrnlVersion->build != 17559u || (uintptr_t)register_driver != 0x800D90B0u ||
        (uintptr_t)unregister_driver != 0x800D90F8u ||
        (uintptr_t)p_UsbdQueueAsyncTransfer != 0x800D8278u) {
        xsdk_plugin_log("usb: refusing to register on a different kernel layout");
        return -3;
    }

    set_status(BT_USB_STAGE_WAITING, 0);
    usb_krnl = krnl;
    register_driver_fn = (void (*)(usb_driver_17559_t *))register_driver;
    /* The driver is registered inside x360_usbd_enumerate_ugreen, after Aurora
     * is the running title. Registering here never re-offers the dongle. */
    xsdk_plugin_log("usb: ordinals ready; bus reset waits until Aurora is the running title");
    return 0;
}

void x360_usbd_release_control(void) {
    adapter.control_pending = false;
}

int x360_usbd_enumerate_ugreen(void) {
    if (!register_driver_fn || !usb_krnl) return -1;
    return usbd_enumerate_silent();
}

#endif /* __XSDK__ */
