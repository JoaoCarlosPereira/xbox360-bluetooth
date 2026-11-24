#define _GNU_SOURCE
#include "../hal/xbox360_usbd.h"
#include "../hci/hci_core.h"
#include "../hid/ds4_bt.h"
#include "../l2cap/l2cap.h"
#include "../transport/bt_usb_transport.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/usb/ch9.h>
#include <linux/usbdevice_fs.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static volatile sig_atomic_t g_running = 1;
static void on_sigint(int sig) { (void)sig; g_running = 0; }

#define TAG_INTR_IN ((void *)(uintptr_t)1)
#define TAG_BULK_IN ((void *)(uintptr_t)2)

typedef struct {
    int                 fd;
    uint8_t             iface_num;
    uint8_t             ep_intr_in;
    uint8_t             ep_bulk_in;
    uint8_t             ep_bulk_out;
    uint16_t            intr_mps;
    uint16_t            bulk_mps;

    struct usbdevfs_urb intr_urb;
    bool                intr_pending;
    uint8_t             intr_buf[64];

    struct usbdevfs_urb bulk_urb;
    bool                bulk_pending;
    uint8_t             bulk_buf[512];

    uint8_t             evt_accum[HCI_MAX_EVT_SIZE];
    size_t              evt_accum_len;

    bool                connecting;
    bool                connected;
    uint8_t             pending_bd[6];
    uint8_t             pending_psr;
    uint16_t            pending_clk_off;
    bool                pending_create_conn;
    uint16_t            acl_handle;
    l2cap_hid_session_t l2cap;
    bool                ds4_feature_sent;

    x360_gamepad_state_t last_gp;
    bool                 has_last_gp;
} ds4_usb_ctx_t;

static int send_hci_ctrl(ds4_usb_ctx_t *u, uint16_t opcode,
                         const uint8_t *params, uint8_t param_len) {
    uint8_t cmd_buf[HCI_MAX_CMD_SIZE];
    size_t cmd_len = hci_build_command(cmd_buf, sizeof(cmd_buf), opcode, params, param_len);
    if (cmd_len == 0) return -1;

    struct usbdevfs_ctrltransfer ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.bRequestType = BT_USB_REQ_TYPE_HCI_CMD;
    ctrl.bRequest     = BT_USB_REQ_HCI_CMD;
    ctrl.wValue       = 0;
    ctrl.wIndex       = u->iface_num;
    ctrl.wLength      = (uint16_t)cmd_len;
    ctrl.timeout      = 2000;
    ctrl.data         = cmd_buf;
    if (ioctl(u->fd, USBDEVFS_CONTROL, &ctrl) < 0) {
        fprintf(stderr, "Control cmd 0x%04X failed: %s\n", opcode, strerror(errno));
        return -1;
    }
    return 0;
}

static int submit_intr_in(ds4_usb_ctx_t *u) {
    if (u->intr_pending) return 0;
    memset(&u->intr_urb, 0, sizeof(u->intr_urb));
    u->intr_urb.type          = USBDEVFS_URB_TYPE_INTERRUPT;
    u->intr_urb.endpoint      = u->ep_intr_in;
    u->intr_urb.buffer        = u->intr_buf;
    u->intr_urb.buffer_length = u->intr_mps ? u->intr_mps : 16;
    u->intr_urb.usercontext   = TAG_INTR_IN;
    if (ioctl(u->fd, USBDEVFS_SUBMITURB, &u->intr_urb) < 0) return -1;
    u->intr_pending = true;
    return 0;
}

static int submit_bulk_in(ds4_usb_ctx_t *u) {
    if (u->bulk_pending) return 0;
    memset(&u->bulk_urb, 0, sizeof(u->bulk_urb));
    u->bulk_urb.type          = USBDEVFS_URB_TYPE_BULK;
    u->bulk_urb.endpoint      = u->ep_bulk_in;
    u->bulk_urb.buffer        = u->bulk_buf;
    u->bulk_urb.buffer_length = sizeof(u->bulk_buf);
    u->bulk_urb.usercontext   = TAG_BULK_IN;
    if (ioctl(u->fd, USBDEVFS_SUBMITURB, &u->bulk_urb) < 0) return -1;
    u->bulk_pending = true;
    return 0;
}

static int send_acl_bulk(void *ctx, uint16_t acl_handle,
                         const uint8_t *acl_payload, uint16_t len) {
    ds4_usb_ctx_t *u = (ds4_usb_ctx_t *)ctx;
    uint8_t pkt[516];
    if ((size_t)(4u + len) > sizeof(pkt)) return -1;

    /* PB=2 (First non-automatically-flushable packet), BC=0 */
    uint16_t handle_flags = (uint16_t)((acl_handle & 0x0FFFu) | (0x2u << 12));
    hci_put_le16(pkt + 0, handle_flags);
    hci_put_le16(pkt + 2, len);
    memcpy(pkt + 4, acl_payload, len);

    struct usbdevfs_bulktransfer bt;
    memset(&bt, 0, sizeof(bt));
    bt.ep      = u->ep_bulk_out;
    bt.len     = 4u + len;
    bt.timeout = 2000;
    bt.data    = pkt;
    if (ioctl(u->fd, USBDEVFS_BULK, &bt) < 0) {
        fprintf(stderr, "USBDEVFS_BULK OUT failed: %s\n", strerror(errno));
        return -2;
    }
    return 0;
}

static void on_ds4_hid_data(void *ctx, uint16_t psm,
                            const uint8_t *data, size_t len) {
    ds4_usb_ctx_t *u = (ds4_usb_ctx_t *)ctx;
    static int raw_dbg_count = 0;
    if (raw_dbg_count < 5) {
        raw_dbg_count++;
        printf("  [HID RAW] psm=0x%04X len=%zu bytes:", psm, len);
        for (size_t i = 0; i < len && i < 16; i++) printf(" %02X", data[i]);
        printf("\n");
        fflush(stdout);
    }
    if (psm != L2CAP_PSM_HID_INTERRUPT) return;

    x360_gamepad_state_t gp;
    if (ds4_bt_parse_hidp_report(data, len, &gp) != 0) return;

    if (!u->has_last_gp ||
        gp.buttons != u->last_gp.buttons ||
        gp.left_trigger != u->last_gp.left_trigger ||
        gp.right_trigger != u->last_gp.right_trigger ||
        abs((int)gp.thumb_lx - (int)u->last_gp.thumb_lx) > 2500 ||
        abs((int)gp.thumb_ly - (int)u->last_gp.thumb_ly) > 2500 ||
        abs((int)gp.thumb_rx - (int)u->last_gp.thumb_rx) > 2500 ||
        abs((int)gp.thumb_ry - (int)u->last_gp.thumb_ry) > 2500) {
        printf("[DS4] Btns=0x%04X  LT=%3u RT=%3u  LX=%6d LY=%6d  RX=%6d RY=%6d\n",
               gp.buttons, gp.left_trigger, gp.right_trigger,
               gp.thumb_lx, gp.thumb_ly, gp.thumb_rx, gp.thumb_ry);
        fflush(stdout);
        u->last_gp = gp;
        u->has_last_gp = true;
    }
}

static void handle_event(ds4_usb_ctx_t *u, const uint8_t *evt, size_t len) {
    if (len < 2) return;
    uint8_t code = evt[0];

    if (code == HCI_EVT_EXTENDED_INQUIRY_RESULT ||
        code == HCI_EVT_INQUIRY_RESULT_RSSI ||
        code == HCI_EVT_INQUIRY_RESULT) {
        hci_inquiry_item_t items[4];
        int n = hci_decode_inquiry_event(evt, len, items, 4);
        for (int i = 0; i < n; i++) {
            const hci_inquiry_item_t *it = &items[i];
            printf("  Discovered %02X:%02X:%02X:%02X:%02X:%02X  CoD=0x%06X  RSSI=%d  Name=\"%s\"\n",
                   it->bd_addr[5], it->bd_addr[4], it->bd_addr[3],
                   it->bd_addr[2], it->bd_addr[1], it->bd_addr[0],
                   it->class_of_device, it->has_rssi ? it->rssi : 0,
                   it->name[0] ? it->name : "(unnamed)");
            bool is_peripheral = ((it->class_of_device & 0x1F00u) == 0x0500u);
            if ((is_peripheral || strstr(it->name, "Wireless Controller")) &&
                !u->connecting && !u->connected) {
                printf("-> Connecting to DS4 %02X:%02X:%02X:%02X:%02X:%02X...\n",
                       it->bd_addr[5], it->bd_addr[4], it->bd_addr[3],
                       it->bd_addr[2], it->bd_addr[1], it->bd_addr[0]);
                u->connecting = true;
                memcpy(u->pending_bd, it->bd_addr, 6);
                u->pending_psr = it->page_scan_rep_mode;
                u->pending_clk_off = it->clock_offset;
                u->pending_create_conn = true;
                send_hci_ctrl(u, HCI_OP_INQUIRY_CANCEL, NULL, 0);
            }
        }
    } else if (code == HCI_EVT_COMMAND_COMPLETE && len >= 6) {
        hci_cmd_complete_t cc;
        if (hci_parse_command_complete(evt, len, &cc) == 0 &&
            cc.opcode == HCI_OP_INQUIRY_CANCEL && u->pending_create_conn) {
            u->pending_create_conn = false;
            uint8_t cp[13];
            memcpy(cp, u->pending_bd, 6);
            hci_put_le16(cp + 6, 0xCC18u); /* Packet types */
            cp[8] = u->pending_psr;
            cp[9] = 0x00;
            hci_put_le16(cp + 10, (uint16_t)(u->pending_clk_off | 0x8000u));
            cp[12] = 0x01; /* Allow role switch */
            send_hci_ctrl(u, HCI_OP_CREATE_CONNECTION, cp, sizeof(cp));
        }
    } else if (code == HCI_EVT_LINK_KEY_REQUEST && len >= 8) {
        printf("  [HCI] Link Key Request -> Negative Reply (fresh pair)\n");
        send_hci_ctrl(u, HCI_OP_LINK_KEY_REQ_NEG_REPLY, evt + 2, 6);
    } else if (code == HCI_EVT_IO_CAPABILITY_REQUEST && len >= 8) {
        printf("  [HCI] IO Capability Request -> NoInputNoOutput\n");
        uint8_t rep[9];
        memcpy(rep, evt + 2, 6);
        rep[6] = 0x03; /* NoInputNoOutput */
        rep[7] = 0x00; /* No OOB */
        rep[8] = 0x04; /* Dedicated Bonding, No MITM */
        send_hci_ctrl(u, HCI_OP_IO_CAP_REQ_REPLY, rep, sizeof(rep));
    } else if (code == HCI_EVT_USER_CONFIRM_REQUEST && len >= 8) {
        printf("  [HCI] User Confirmation Request -> Auto-Accept\n");
        send_hci_ctrl(u, HCI_OP_USER_CONFIRM_REQ_REPLY, evt + 2, 6);
    } else if (code == HCI_EVT_LINK_KEY_NOTIFICATION && len >= 24) {
        printf("  [HCI] Link Key Generated! (paired)\n");
    } else if (code == 0x06u && len >= 5) { /* Authentication Complete */
        uint8_t status = evt[2];
        uint16_t handle = hci_get_le16(evt + 3) & 0x0FFFu;
        printf("  [HCI] Auth Complete status=0x%02X, enabling encryption...\n", status);
        if (status == 0x00) {
            uint8_t enc[3];
            hci_put_le16(enc, handle);
            enc[2] = 0x01; /* Encryption ON */
            send_hci_ctrl(u, 0x0413u, enc, sizeof(enc)); /* HCI_Set_Connection_Encryption */
        }
    } else if (code == 0x08u && len >= 6) { /* Encryption Change */
        uint8_t status = evt[2];
        uint8_t enc_on = evt[5];
        printf("  [HCI] Encryption Change status=0x%02X enc=%u. Opening L2CAP HID channels...\n", status, enc_on);
        if (status == 0x00 && enc_on) {
            l2cap_hid_init(&u->l2cap, u->acl_handle, send_acl_bulk, on_ds4_hid_data, u);
            l2cap_hid_connect(&u->l2cap);
        }
    } else if (code == HCI_EVT_CONNECTION_COMPLETE && len >= 13) {
        uint8_t status = evt[2];
        uint16_t handle = hci_get_le16(evt + 3) & 0x0FFFu;
        u->connecting = false;
        if (status == 0x00) {
            u->connected = true;
            u->acl_handle = handle;
            printf("  [HCI] Connected! ACL Handle=0x%04X. Requesting SSP Authentication...\n", handle);
            uint8_t auth[2];
            hci_put_le16(auth, handle);
            send_hci_ctrl(u, 0x0411u, auth, sizeof(auth)); /* HCI_Authentication_Requested */
        } else {
            printf("  [HCI] Connection failed status=0x%02X, resuming scan...\n", status);
            uint8_t inq[5] = { 0x33, 0x8B, 0x9E, 10, 0x00 };
            send_hci_ctrl(u, HCI_OP_INQUIRY, inq, sizeof(inq));
        }
    } else if (code == HCI_EVT_INQUIRY_COMPLETE) {
        if (!u->connecting && !u->connected) {
            uint8_t inq[5] = { 0x33, 0x8B, 0x9E, 10, 0x00 };
            send_hci_ctrl(u, HCI_OP_INQUIRY, inq, sizeof(inq));
        }
    } else if (code == HCI_EVT_DISCONNECTION_COMPLETE && len >= 6) {
        printf("  [HCI] Disconnected (reason=0x%02X), resuming scan...\n", evt[5]);
        u->connected = false;
        u->connecting = false;
        u->ds4_feature_sent = false;
        uint8_t inq[5] = { 0x33, 0x8B, 0x9E, 10, 0x00 };
        send_hci_ctrl(u, HCI_OP_INQUIRY, inq, sizeof(inq));
    } else if (code != HCI_EVT_COMMAND_COMPLETE && code != HCI_EVT_COMMAND_STATUS) {
        printf("  [HCI EVT] code=0x%02X len=%zu\n", code, len);
    }
}

static int open_bt_usb(ds4_usb_ctx_t *u) {
    DIR *bus_dir = opendir("/dev/bus/usb");
    if (!bus_dir) return -1;
    struct dirent *be;
    while ((be = readdir(bus_dir)) != NULL) {
        if (be->d_name[0] == '.') continue;
        char bp[300];
        snprintf(bp, sizeof(bp), "/dev/bus/usb/%s", be->d_name);
        DIR *ddir = opendir(bp);
        if (!ddir) continue;
        struct dirent *de;
        while ((de = readdir(ddir)) != NULL) {
            if (de->d_name[0] == '.') continue;
            char dp[600];
            snprintf(dp, sizeof(dp), "%s/%s", bp, de->d_name);
            int fd = open(dp, O_RDWR);
            if (fd < 0) {
                if (errno == EACCES) {
                    static bool warned = false;
                    if (!warned) {
                        fprintf(stderr, "Permission denied opening %s (run with sudo).\n", dp);
                        warned = true;
                    }
                }
                continue;
            }
            uint8_t d[1024];
            ssize_t n = read(fd, d, sizeof(d));
            if (n >= 18 && bt_usb_is_hci_interface(d[4], d[5], d[6])) {
                size_t off = d[0];
                bool in_if = false;
                while (off + 2 <= (size_t)n) {
                    uint8_t l = d[off], t = d[off + 1];
                    if (l < 2 || off + l > (size_t)n) break;
                    if (t == USB_DT_INTERFACE && l >= 9) {
                        in_if = (bt_usb_is_hci_interface(d[off + 5], d[off + 6], d[off + 7]) && d[off + 3] == 0);
                        if (in_if) u->iface_num = d[off + 2];
                    } else if (t == USB_DT_ENDPOINT && l >= 7 && in_if) {
                        uint8_t addr = d[off + 2], xfer = d[off + 3] & 0x03u;
                        uint16_t mps = (uint16_t)(d[off + 4] | (d[off + 5] << 8)) & 0x7FFu;
                        if (xfer == 3 && (addr & 0x80u)) { u->ep_intr_in = addr; u->intr_mps = mps; }
                        if (xfer == 2 && (addr & 0x80u)) { u->ep_bulk_in = addr; u->bulk_mps = mps; }
                        if (xfer == 2 && !(addr & 0x80u)) { u->ep_bulk_out = addr; }
                    }
                    off += l;
                }
                if (u->ep_intr_in && u->ep_bulk_in && u->ep_bulk_out) {
                    u->fd = fd;
                    closedir(ddir);
                    closedir(bus_dir);
                    return 0;
                }
            }
            close(fd);
        }
        closedir(ddir);
    }
    closedir(bus_dir);
    return -1;
}

static int send_hci_sync(ds4_usb_ctx_t *u, uint16_t opcode,
                         const uint8_t *params, uint8_t param_len) {
    if (send_hci_ctrl(u, opcode, params, param_len) != 0) return -1;
    while (g_running) {
        struct pollfd pfd = { .fd = u->fd, .events = POLLOUT };
        if (poll(&pfd, 1, 2000) <= 0) return -2;
        struct usbdevfs_urb *reaped = NULL;
        if (ioctl(u->fd, USBDEVFS_REAPURB, &reaped) < 0) {
            if (errno == EAGAIN) continue;
            return -3;
        }
        if (!reaped) continue;
        if (reaped->usercontext == TAG_INTR_IN) {
            u->intr_pending = false;
            if (reaped->status == 0 && reaped->actual_length > 0 &&
                u->evt_accum_len + (size_t)reaped->actual_length <= sizeof(u->evt_accum)) {
                memcpy(u->evt_accum + u->evt_accum_len, reaped->buffer, (size_t)reaped->actual_length);
                u->evt_accum_len += (size_t)reaped->actual_length;
                while (u->evt_accum_len >= 2u) {
                    size_t exp = 2u + u->evt_accum[1];
                    if (u->evt_accum_len < exp) break;
                    hci_cmd_complete_t cc;
                    bool done = (hci_parse_command_complete(u->evt_accum, exp, &cc) == 0 && cc.opcode == opcode);
                    size_t rem = u->evt_accum_len - exp;
                    if (rem > 0) memmove(u->evt_accum, u->evt_accum + exp, rem);
                    u->evt_accum_len = rem;
                    if (done) {
                        submit_intr_in(u);
                        return 0;
                    }
                }
            }
            submit_intr_in(u);
        } else if (reaped->usercontext == TAG_BULK_IN) {
            u->bulk_pending = false;
            submit_bulk_in(u);
        }
    }
    return -4;
}

int main(void) {
    signal(SIGINT, on_sigint);
    ds4_usb_ctx_t u;
    memset(&u, 0, sizeof(u));
    if (open_bt_usb(&u) != 0) {
        fprintf(stderr, "No USB Bluetooth HCI dongle found\n");
        return 1;
    }

    struct usbdevfs_ioctl cmd = { .ifno = u.iface_num, .ioctl_code = USBDEVFS_DISCONNECT };
    bool detached = (ioctl(u.fd, USBDEVFS_IOCTL, &cmd) == 0);
    int iface = u.iface_num;
    if (ioctl(u.fd, USBDEVFS_CLAIMINTERFACE, &iface) < 0) {
        perror("CLAIMINTERFACE");
        close(u.fd);
        return 1;
    }

    submit_intr_in(&u);
    submit_bulk_in(&u);

    uint8_t mask[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x3F };
    uint8_t ssp = 1, inq_mode = 2;
    uint8_t page_to[2] = { 0x00, 0x40 }; /* 0x4000 * 0.625ms = 10.24s */
    if (send_hci_sync(&u, HCI_OP_RESET, NULL, 0) != 0 ||
        send_hci_sync(&u, HCI_OP_SET_EVENT_MASK, mask, sizeof(mask)) != 0 ||
        send_hci_sync(&u, 0x0C18u, page_to, 2) != 0 ||
        send_hci_sync(&u, HCI_OP_WRITE_SIMPLE_PAIRING, &ssp, 1) != 0 ||
        send_hci_sync(&u, HCI_OP_WRITE_INQUIRY_MODE, &inq_mode, 1) != 0) {
        fprintf(stderr, "HCI init sequence failed\n");
    }

    printf("Scanning for DualShock 4 (put controller in pairing mode: Share + PS)...\n");
    uint8_t inq[5] = { 0x33, 0x8B, 0x9E, 10, 0x00 }; /* ~12.8s scan */
    send_hci_ctrl(&u, HCI_OP_INQUIRY, inq, sizeof(inq));

    while (g_running) {
        struct pollfd pfd = { .fd = u.fd, .events = POLLOUT };
        if (poll(&pfd, 1, 500) <= 0) continue;

        struct usbdevfs_urb *reaped = NULL;
        if (ioctl(u.fd, USBDEVFS_REAPURB, &reaped) < 0) {
            if (errno == EAGAIN) continue;
            break;
        }
        if (!reaped) continue;

        if (reaped->usercontext == TAG_INTR_IN) {
            u.intr_pending = false;
            if (reaped->status == 0 && reaped->actual_length > 0) {
                if (u.evt_accum_len + (size_t)reaped->actual_length <= sizeof(u.evt_accum)) {
                    memcpy(u.evt_accum + u.evt_accum_len, reaped->buffer, (size_t)reaped->actual_length);
                    u.evt_accum_len += (size_t)reaped->actual_length;
                    while (u.evt_accum_len >= 2u) {
                        size_t exp = 2u + u.evt_accum[1];
                        if (u.evt_accum_len < exp) break;
                        handle_event(&u, u.evt_accum, exp);
                        size_t rem = u.evt_accum_len - exp;
                        if (rem > 0) memmove(u.evt_accum, u.evt_accum + exp, rem);
                        u.evt_accum_len = rem;
                    }
                } else {
                    u.evt_accum_len = 0;
                }
            }
            submit_intr_in(&u);
        } else if (reaped->usercontext == TAG_BULK_IN) {
            u.bulk_pending = false;
            if (reaped->status == 0 && reaped->actual_length >= 4 && u.connected) {
                const uint8_t *acl = (const uint8_t *)reaped->buffer;
                uint16_t acl_len = hci_get_le16(acl + 2);
                if (4u + (size_t)acl_len <= (size_t)reaped->actual_length) {
                    l2cap_hid_on_acl_payload(&u.l2cap, acl + 4, acl_len);
                    if (!u.ds4_feature_sent &&
                        u.l2cap.hid_ctrl.state == L2CAP_CH_OPEN &&
                        u.l2cap.hid_intr.state == L2CAP_CH_OPEN) {
                        u.ds4_feature_sent = true;
                        printf("  [L2CAP] HID Control & Interrupt OPEN! Requesting DS4 Feature 0x02 + LED...\n");
                        uint8_t get_feat[2] = { 0x43, 0x02 }; /* HIDP GET_REPORT Feature 0x02 */
                        l2cap_hid_send(&u.l2cap, L2CAP_PSM_HID_CONTROL, get_feat, sizeof(get_feat));

                        /* HIDP SET_REPORT Output 0x11 (full 79-byte frame): turn DS4 lightbar solid green */
                        uint8_t set_led[79];
                        memset(set_led, 0, sizeof(set_led));
                        set_led[0]  = 0x52; /* HIDP SET_REPORT | OUTPUT */
                        set_led[1]  = 0x11; /* Report ID 0x11 */
                        set_led[2]  = 0xC0;
                        set_led[3]  = 0x00;
                        set_led[4]  = 0x07; /* Enable rumble/LED flags */
                        set_led[7]  = 0x00; /* Small rumble */
                        set_led[8]  = 0x00; /* Large rumble */
                        set_led[9]  = 0x00; /* R */
                        set_led[10] = 0x80; /* G */
                        set_led[11] = 0x00; /* B */
                        l2cap_hid_send(&u.l2cap, L2CAP_PSM_HID_CONTROL, set_led, sizeof(set_led));
                    }
                }
            }
            submit_bulk_in(&u);
        }
    }

    if (u.intr_pending) { ioctl(u.fd, USBDEVFS_DISCARDURB, &u.intr_urb); struct usbdevfs_urb *t; ioctl(u.fd, USBDEVFS_REAPURBNDELAY, &t); }
    if (u.bulk_pending) { ioctl(u.fd, USBDEVFS_DISCARDURB, &u.bulk_urb); struct usbdevfs_urb *t; ioctl(u.fd, USBDEVFS_REAPURBNDELAY, &t); }
    ioctl(u.fd, USBDEVFS_RELEASEINTERFACE, &iface);
    if (detached) { cmd.ioctl_code = USBDEVFS_CONNECT; ioctl(u.fd, USBDEVFS_IOCTL, &cmd); }
    close(u.fd);
    return 0;
}
