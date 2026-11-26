#include "../hal/xbox360_usbd.h"
#include "../transport/bt_usb_transport.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH 31
#endif
#define BTPROTO_HCI     1
#define SOL_HCI         0
#define HCI_OPT_FILTER  2
#define HCI_CHANNEL_RAW 0

#define HCI_H4_CMD_PKT  0x01u
#define HCI_H4_EVT_PKT  0x04u

struct sockaddr_hci {
    sa_family_t    hci_family;
    unsigned short hci_dev;
    unsigned short hci_channel;
};

struct hci_filter {
    uint32_t type_mask;
    uint32_t event_mask[2];
    uint16_t opcode;
};

typedef struct {
    int      hci_fd;
    uint8_t *rx_buf;
    uint16_t rx_cap;
    bool     rx_armed;
    uint8_t  pending_evt[HCI_MAX_EVT_SIZE];
    size_t   pending_evt_len;
} linux_usb_bridge_ctx_t;

static int bridge_submit_intr_in(void *ctx, uint8_t *buf, uint16_t max_len) {
    linux_usb_bridge_ctx_t *b = (linux_usb_bridge_ctx_t *)ctx;
    b->rx_buf = buf;
    b->rx_cap = max_len;
    b->rx_armed = true;
    return 0;
}

static int bridge_submit_control_out(void *ctx, uint8_t bmRequestType, uint8_t bRequest,
                                     uint16_t wValue, uint16_t wIndex,
                                     const uint8_t *data, uint16_t length) {
    linux_usb_bridge_ctx_t *b = (linux_usb_bridge_ctx_t *)ctx;
    assert(bmRequestType == BT_USB_REQ_TYPE_HCI_CMD);
    assert(bRequest == BT_USB_REQ_HCI_CMD);
    assert(wValue == 0);
    (void)wIndex;

    if (length < HCI_CMD_HDR_SIZE) {
        return -1;
    }

    uint16_t opcode = hci_get_le16(data);

    if (b->hci_fd < 0) {
        static const uint8_t version[] = { 0, 9, 0x34, 0x12, 9, 0x5d, 0, 0x78, 0x56 };
        static const uint8_t address[] = { 0, 1, 2, 3, 4, 5, 6 };
        const uint8_t *params = NULL;
        size_t params_len = 1;
        static const uint8_t success[] = { 0 };
        if (opcode == HCI_OP_RESET) params = success;
        else if (opcode == HCI_OP_READ_LOCAL_VERSION_INFO) {
            params = version;
            params_len = sizeof(version);
        } else if (opcode == HCI_OP_READ_BD_ADDR) {
            params = address;
            params_len = sizeof(address);
        } else return -1;
        b->pending_evt[0] = HCI_EVT_COMMAND_COMPLETE;
        b->pending_evt[1] = (uint8_t)(3 + params_len);
        b->pending_evt[2] = 1;
        hci_put_le16(b->pending_evt + 3, opcode);
        memcpy(b->pending_evt + 5, params, params_len);
        b->pending_evt_len = 5 + params_len;
        return 0;
    }

    /*
     * Do NOT send HCI_Reset to the host's live hci0 so we don't disrupt
     * system Bluetooth state; synthesize a standard Command Complete instead.
     * Forward read-only informational commands (0x1001, 0x1009) to real hci0.
     */
    if (opcode == HCI_OP_RESET) {
        b->pending_evt[0] = HCI_EVT_COMMAND_COMPLETE;
        b->pending_evt[1] = 4; /* NumPackets(1) + Opcode(2) + Status(1) */
        b->pending_evt[2] = 1;
        hci_put_le16(b->pending_evt + 3, HCI_OP_RESET);
        b->pending_evt[5] = 0x00;
        b->pending_evt_len = 6;
        return 0;
    }

    uint8_t tx[1 + HCI_MAX_CMD_SIZE];
    tx[0] = HCI_H4_CMD_PKT;
    memcpy(tx + 1, data, length);

    struct hci_filter flt;
    memset(&flt, 0, sizeof(flt));
    flt.type_mask = (1u << HCI_H4_EVT_PKT);
    flt.event_mask[0] = (1u << HCI_EVT_COMMAND_COMPLETE);
    flt.opcode = opcode;
    if (setsockopt(b->hci_fd, SOL_HCI, HCI_OPT_FILTER, &flt, sizeof(flt)) < 0) {
        return -2;
    }
    if (write(b->hci_fd, tx, 1u + length) < 0) {
        return -3;
    }

    uint8_t rx[1 + HCI_MAX_EVT_SIZE];
    ssize_t n = read(b->hci_fd, rx, sizeof(rx));
    if (n < 2 || rx[0] != HCI_H4_EVT_PKT) {
        return -4;
    }
    b->pending_evt_len = (size_t)(n - 1);
    memcpy(b->pending_evt, rx + 1, b->pending_evt_len);
    return 0;
}

static void run_profile_test(int hci_fd, const char *name, uint16_t vid, uint16_t pid,
                             uint16_t intr_mps) {
    assert(bt_usb_is_hci_interface(0xE0, 0x01, 0x01) == true);
    assert(bt_usb_is_hci_interface(0x03, 0x00, 0x00) == false);

    bt_usb_device_info_t dev;
    memset(&dev, 0, sizeof(dev));
    dev.vid = vid;
    dev.pid = pid;
    dev.interface_num = 0;
    dev.has_intr_in = true;
    dev.intr_in.bEndpointAddress = 0x81;
    dev.intr_in.bmAttributes = BT_USB_EP_TYPE_INTERRUPT;
    dev.intr_in.wMaxPacketSize = intr_mps;
    dev.intr_in.bInterval = 1;
    dev.has_bulk_in = true;
    dev.bulk_in.bEndpointAddress = 0x82;
    dev.bulk_in.bmAttributes = BT_USB_EP_TYPE_BULK;
    dev.bulk_in.wMaxPacketSize = 64;
    dev.has_bulk_out = true;
    dev.bulk_out.bEndpointAddress = 0x02;
    dev.bulk_out.bmAttributes = BT_USB_EP_TYPE_BULK;
    dev.bulk_out.wMaxPacketSize = 64;

    linux_usb_bridge_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.hci_fd = hci_fd;

    bt_usb_hal_ops_t hal = {
        .ctx = &ctx,
        .submit_control_out = bridge_submit_control_out,
        .submit_intr_in = bridge_submit_intr_in
    };

    bt_usb_transport_t tr;
    assert(bt_usb_transport_init(&tr, &dev, &hal) == 0);
    assert(bt_usb_transport_start_init(&tr) == 0);

    /* Pump pending events in wMaxPacketSize chunks to test USB frame reassembly */
    while (tr.state != BT_INIT_READY && tr.state != BT_INIT_ERROR) {
        assert(ctx.rx_armed);
        assert(ctx.pending_evt_len > 0);

        uint8_t evt_copy[HCI_MAX_EVT_SIZE];
        size_t evt_len = ctx.pending_evt_len;
        memcpy(evt_copy, ctx.pending_evt, evt_len);
        ctx.pending_evt_len = 0;

        size_t off = 0;
        while (off < evt_len) {
            size_t chunk = evt_len - off;
            if (chunk > ctx.rx_cap) {
                chunk = ctx.rx_cap;
            }
            ctx.rx_armed = false;
            memcpy(ctx.rx_buf, evt_copy + off, chunk);
            off += chunk;
            bt_usb_transport_on_intr_in(&tr, ctx.rx_buf, chunk);
        }
    }

    assert(tr.state == BT_INIT_READY);
    assert(tr.has_local_ver);
    assert(tr.has_bd_addr);
    if (hci_fd < 0) {
        static const uint8_t expected_addr[] = { 1, 2, 3, 4, 5, 6 };
        assert(tr.local_ver.manufacturer == 0x005d);
        assert(memcmp(tr.bd_addr.bd_addr, expected_addr, sizeof(expected_addr)) == 0);
    }

    printf("[PASS] %-30s (VID:PID %04X:%04X, IntrMPS=%u) -> BT %s | Mfr=0x%04X | BD=%02X:%02X:%02X:%02X:%02X:%02X\n",
           name, vid, pid, intr_mps,
           hci_version_str(tr.local_ver.hci_version),
           tr.local_ver.manufacturer,
           tr.bd_addr.bd_addr[5], tr.bd_addr.bd_addr[4], tr.bd_addr.bd_addr[3],
           tr.bd_addr.bd_addr[2], tr.bd_addr.bd_addr[1], tr.bd_addr.bd_addr[0]);
}

int main(int argc, char **argv) {
    /* Sanity check Xbox 360 HAL struct sizes and endian helper */
    assert(x360_bswap16(0x1234u) == 0x3412u);
    assert(sizeof(x360_usb_device_descriptor_t) == 18u);
    assert(sizeof(x360_usb_interface_descriptor_t) == 9u);
    assert(sizeof(x360_usb_endpoint_descriptor_t) == 7u);
    assert(sizeof(x360_usb_setup_packet_t) == 8u);

    int fd = -1;
    if (argc > 3 || (argc >= 2 && strcmp(argv[1], "--live") != 0)) {
        fprintf(stderr, "Usage: %s [--live [HCI_INDEX]]\n", argv[0]);
        return 2;
    }
    if (argc >= 2) {
        char *end = NULL;
        long index = argc == 3 ? strtol(argv[2], &end, 10) : 0;
        if (argc == 3 && (end == argv[2] || *end != '\0' || index < 0 || index > 65534)) {
            fprintf(stderr, "Invalid HCI index: %s\n", argv[2]);
            return 2;
        }
        fd = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI);
        if (fd < 0) {
            perror("socket(AF_BLUETOOTH, BTPROTO_HCI)");
            return 1;
        }

        struct timeval tv = { 2, 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        struct sockaddr_hci addr;
        memset(&addr, 0, sizeof(addr));
        addr.hci_family  = AF_BLUETOOTH;
        addr.hci_dev     = (unsigned short)index;
        addr.hci_channel = HCI_CHANNEL_RAW;

        if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            fprintf(stderr, "bind(hci%ld) failed: %s\n", index, strerror(errno));
            close(fd);
            return 1;
        }
    }

    /* Device profiles use simulated responses by default; --live uses hci0. */
    run_profile_test(fd, "UGREEN RTL8761BU profile",     0x2B89, 0x8761, 16);
    run_profile_test(fd, "CSR8510 A10 (Generic)",       0x0A12, 0x0001, 16);
    run_profile_test(fd, "UGREEN CM748 (Barrot)",       0x33FA, 0x0010, 16);
    run_profile_test(fd, "UGREEN BT 5.0 (RTL8761B)",    0x0BDA, 0x8771, 8);

    if (fd >= 0) close(fd);
    return 0;
}
