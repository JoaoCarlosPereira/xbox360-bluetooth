#define _GNU_SOURCE
#include "../hal/xbox360_usbd.h"
#include "../transport/bt_usb_transport.h"
#include "../chipset/rtl8761b.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/usb/ch9.h>
#include <linux/usbdevice_fs.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

typedef struct {
    int                   fd;
    uint8_t               iface_num;
    uint8_t               ep_intr_in;
    struct usbdevfs_urb   intr_urb;
    bool                  urb_pending;
} usbdevfs_ctx_t;

static int usbdevfs_submit_control_out(void *ctx, uint8_t bmRequestType, uint8_t bRequest,
                                       uint16_t wValue, uint16_t wIndex,
                                       const uint8_t *data, uint16_t length) {
    usbdevfs_ctx_t *u = (usbdevfs_ctx_t *)ctx;
    struct usbdevfs_ctrltransfer ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.bRequestType = bmRequestType;
    ctrl.bRequest     = bRequest;
    ctrl.wValue       = wValue;
    ctrl.wIndex       = wIndex;
    ctrl.wLength      = length;
    ctrl.timeout      = 2000;
    ctrl.data         = (void *)(uintptr_t)data;

    if (ioctl(u->fd, USBDEVFS_CONTROL, &ctrl) < 0) {
        fprintf(stderr, "USBDEVFS_CONTROL failed: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

static int usbdevfs_submit_intr_in(void *ctx, uint8_t *buf, uint16_t max_len) {
    usbdevfs_ctx_t *u = (usbdevfs_ctx_t *)ctx;
    if (u->urb_pending) {
        return 0;
    }

    memset(&u->intr_urb, 0, sizeof(u->intr_urb));
    u->intr_urb.type          = USBDEVFS_URB_TYPE_INTERRUPT;
    u->intr_urb.endpoint      = u->ep_intr_in;
    u->intr_urb.buffer        = buf;
    u->intr_urb.buffer_length = max_len;

    if (ioctl(u->fd, USBDEVFS_SUBMITURB, &u->intr_urb) < 0) {
        fprintf(stderr, "USBDEVFS_SUBMITURB(intr_in) failed: %s\n", strerror(errno));
        return -1;
    }
    u->urb_pending = true;
    return 0;
}

static int find_and_open_bt_usb(uint16_t target_vid, uint16_t target_pid,
                                char *out_path, size_t out_cap,
                                bt_usb_device_info_t *out_info) {
    DIR *bus_dir = opendir("/dev/bus/usb");
    if (!bus_dir) {
        perror("opendir(/dev/bus/usb)");
        return -1;
    }

    struct dirent *bus_ent;
    while ((bus_ent = readdir(bus_dir)) != NULL) {
        if (bus_ent->d_name[0] == '.') continue;

        char bus_path[300];
        snprintf(bus_path, sizeof(bus_path), "/dev/bus/usb/%s", bus_ent->d_name);
        DIR *dev_dir = opendir(bus_path);
        if (!dev_dir) continue;

        struct dirent *dev_ent;
        while ((dev_ent = readdir(dev_dir)) != NULL) {
            if (dev_ent->d_name[0] == '.') continue;

            char dev_path[600];
            snprintf(dev_path, sizeof(dev_path), "%s/%s", bus_path, dev_ent->d_name);
            int fd = open(dev_path, O_RDWR);
            if (fd < 0) continue;

            uint8_t desc[1024];
            ssize_t n = read(fd, desc, sizeof(desc));
            if (n >= 18) {
                struct usb_device_descriptor *dd = (struct usb_device_descriptor *)desc;
                uint16_t vid = (uint16_t)desc[8] | ((uint16_t)desc[9] << 8);
                uint16_t pid = (uint16_t)desc[10] | ((uint16_t)desc[11] << 8);

                bool match = (target_vid != 0)
                    ? (vid == target_vid && pid == target_pid)
                    : bt_usb_is_hci_interface(dd->bDeviceClass, dd->bDeviceSubClass, dd->bDeviceProtocol);

                if (match) {
                    memset(out_info, 0, sizeof(*out_info));
                    out_info->vid = vid;
                    out_info->pid = pid;

                    /* Parse config + interface + endpoint descriptors appended after device descriptor */
                    size_t off = dd->bLength;
                    bool in_hci_iface = false;
                    while (off + 2 <= (size_t)n) {
                        uint8_t len  = desc[off];
                        uint8_t type = desc[off + 1];
                        if (len < 2 || off + len > (size_t)n) break;

                        if (type == USB_DT_INTERFACE && len >= 9) {
                            struct usb_interface_descriptor *id = (struct usb_interface_descriptor *)(desc + off);
                            if (bt_usb_is_hci_interface(id->bInterfaceClass,
                                                        id->bInterfaceSubClass,
                                                        id->bInterfaceProtocol) &&
                                id->bAlternateSetting == 0 && !out_info->has_intr_in) {
                                out_info->interface_num = id->bInterfaceNumber;
                                in_hci_iface = true;
                            } else {
                                in_hci_iface = false;
                            }
                        } else if (type == USB_DT_ENDPOINT && len >= 7 && in_hci_iface) {
                            struct usb_endpoint_descriptor *ep = (struct usb_endpoint_descriptor *)(desc + off);
                            uint8_t xfer = ep->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK;
                            bool is_in = (ep->bEndpointAddress & USB_ENDPOINT_DIR_MASK) != 0;
                            uint16_t mps = (uint16_t)(desc[off + 4] | (desc[off + 5] << 8)) & 0x7FFu;

                            if (xfer == USB_ENDPOINT_XFER_INT && is_in && !out_info->has_intr_in) {
                                out_info->has_intr_in = true;
                                out_info->intr_in.bEndpointAddress = ep->bEndpointAddress;
                                out_info->intr_in.bmAttributes     = xfer;
                                out_info->intr_in.wMaxPacketSize   = mps;
                                out_info->intr_in.bInterval        = ep->bInterval;
                            } else if (xfer == USB_ENDPOINT_XFER_BULK && is_in && !out_info->has_bulk_in) {
                                out_info->has_bulk_in = true;
                                out_info->bulk_in.bEndpointAddress = ep->bEndpointAddress;
                                out_info->bulk_in.bmAttributes     = xfer;
                                out_info->bulk_in.wMaxPacketSize   = mps;
                            } else if (xfer == USB_ENDPOINT_XFER_BULK && !is_in && !out_info->has_bulk_out) {
                                out_info->has_bulk_out = true;
                                out_info->bulk_out.bEndpointAddress = ep->bEndpointAddress;
                                out_info->bulk_out.bmAttributes     = xfer;
                                out_info->bulk_out.wMaxPacketSize   = mps;
                            }
                        }
                        off += len;
                    }

                    if (out_info->has_intr_in) {
                        snprintf(out_path, out_cap, "%s", dev_path);
                        closedir(dev_dir);
                        closedir(bus_dir);
                        return fd;
                    }
                }
            }
            close(fd);
        }
        closedir(dev_dir);
    }

    closedir(bus_dir);
    return -1;
}

typedef struct {
    usbdevfs_ctx_t *u;
    uint16_t        iface_num;
    uint16_t        intr_mps;
    uint8_t         rx_buf[64];
} rtl_sync_ctx_t;

static int read_entire_file(const char *path, uint8_t **out_buf, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -2; }
    long sz = ftell(f);
    if (sz <= 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -3; }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(f); return -4; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return -5; }
    fclose(f);
    *out_buf = buf;
    *out_len = (size_t)sz;
    return 0;
}

static int sync_send_and_wait_cc(void *vctx, uint16_t opcode,
                                 const uint8_t *params, uint8_t param_len,
                                 hci_cmd_complete_t *out_cc,
                                 uint8_t *evt_accum, size_t evt_cap) {
    rtl_sync_ctx_t *sc = (rtl_sync_ctx_t *)vctx;
    uint8_t cmd_buf[HCI_MAX_CMD_SIZE];
    size_t cmd_len = hci_build_command(cmd_buf, sizeof(cmd_buf), opcode, params, param_len);
    if (cmd_len == 0) return -1;

    if (usbdevfs_submit_intr_in(sc->u, sc->rx_buf, sc->intr_mps) != 0) return -2;
    if (usbdevfs_submit_control_out(sc->u, BT_USB_REQ_TYPE_HCI_CMD, BT_USB_REQ_HCI_CMD,
                                    0, sc->iface_num, cmd_buf, (uint16_t)cmd_len) != 0) {
        return -3;
    }

    size_t accum_len = 0;
    while (1) {
        struct pollfd pfd = { .fd = sc->u->fd, .events = POLLOUT };
        int pr = poll(&pfd, 1, 3000);
        if (pr <= 0) return -4;

        struct usbdevfs_urb *reaped = NULL;
        if (ioctl(sc->u->fd, USBDEVFS_REAPURB, &reaped) < 0) {
            if (errno == EAGAIN) continue;
            return -5;
        }
        sc->u->urb_pending = false;
        if (!reaped || reaped->status != 0) return -6;

        if (accum_len + (size_t)reaped->actual_length > evt_cap) return -7;
        memcpy(evt_accum + accum_len, reaped->buffer, (size_t)reaped->actual_length);
        accum_len += (size_t)reaped->actual_length;

        if (accum_len >= HCI_EVT_HDR_SIZE) {
            size_t expected = HCI_EVT_HDR_SIZE + (size_t)evt_accum[1];
            if (accum_len >= expected) {
                if (hci_parse_command_complete(evt_accum, expected, out_cc) == 0 &&
                    out_cc->opcode == opcode) {
                    return 0;
                }
                accum_len = 0;
            }
        }
        if (usbdevfs_submit_intr_in(sc->u, sc->rx_buf, sc->intr_mps) != 0) return -8;
    }
}

static int rtl_chunk_sender(void *vctx, uint16_t opcode,
                            const uint8_t *params, uint8_t param_len) {
    uint8_t evt_accum[HCI_MAX_EVT_SIZE];
    hci_cmd_complete_t cc;
    int rc = sync_send_and_wait_cc(vctx, opcode, params, param_len, &cc, evt_accum, sizeof(evt_accum));
    if (rc != 0) return rc;
    if (cc.params_len < 1 || cc.params[0] != 0x00) return -10;
    return 0;
}

static int try_upload_realtek_fw(usbdevfs_ctx_t *u, const bt_usb_device_info_t *info) {
    rtl_sync_ctx_t sc = {
        .u = u,
        .iface_num = info->interface_num,
        .intr_mps = info->intr_in.wMaxPacketSize ? info->intr_in.wMaxPacketSize : 16u
    };
    uint8_t evt_accum[HCI_MAX_EVT_SIZE];
    hci_cmd_complete_t cc;

    if (sync_send_and_wait_cc(&sc, RTL_OP_READ_ROM_VERSION, NULL, 0, &cc, evt_accum, sizeof(evt_accum)) != 0 ||
        cc.params_len < 2 || cc.params[0] != 0x00) {
        fprintf(stderr, "RTL_READ_ROM_VERSION failed\n");
        return -1;
    }
    uint8_t rom_ver = cc.params[1];
    printf("  Realtek ROM version: %u\n", rom_ver);

    uint8_t *fw_buf = NULL, *cfg_buf = NULL;
    size_t fw_sz = 0, cfg_sz = 0;
    if (read_entire_file("/lib/firmware/rtl_bt/rtl8761bu_fw.bin", &fw_buf, &fw_sz) != 0) {
        fprintf(stderr, "Could not open /lib/firmware/rtl_bt/rtl8761bu_fw.bin\n");
        return -2;
    }
    if (read_entire_file("/lib/firmware/rtl_bt/rtl8761bu_config.bin", &cfg_buf, &cfg_sz) != 0) {
        cfg_buf = NULL;
        cfg_sz = 0;
    }

    rtl_fw_image_t img;
    int prep_rc = rtl_fw_prepare(&img, fw_buf, fw_sz, cfg_buf, cfg_sz, rom_ver);
    if (prep_rc != 0) {
        fprintf(stderr, "rtl_fw_prepare failed: %d\n", prep_rc);
        free(fw_buf);
        free(cfg_buf);
        return -3;
    }

    size_t nchunks = rtl_fw_chunk_count(&img);
    printf("  Uploading Realtek FW v0x%08X (%zu bytes, %zu chunks)...\n",
           img.fw_version, img.total_len, nchunks);

    for (size_t i = 0; i < nchunks; i++) {
        if (rtl_fw_send_chunk(&img, i, rtl_chunk_sender, &sc) != 0) {
            fprintf(stderr, "  Chunk %zu/%zu failed!\n", i + 1, nchunks);
            free(fw_buf);
            free(cfg_buf);
            return -4;
        }
    }

    free(fw_buf);
    free(cfg_buf);
    printf("  Firmware upload complete! Resetting controller...\n");
    return 0;
}

int main(int argc, char **argv) {
    bool cold_reset = (argc > 1 && strcmp(argv[1], "--cold-reset") == 0);
    char dev_path[600];
    bt_usb_device_info_t info;
    int fd = find_and_open_bt_usb(0, 0, dev_path, sizeof(dev_path), &info);
    if (fd < 0) {
        fprintf(stderr, "No standard USB Bluetooth HCI (0xE0/0x01/0x01) device found.\n");
        return 1;
    }

    printf("Found USB BT HCI device at %s\n", dev_path);
    printf("  VID:PID    = %04X:%04X\n", info.vid, info.pid);
    printf("  Interface  = %u\n", info.interface_num);
    printf("  Intr IN    = 0x%02X (MPS=%u, interval=%u)\n",
           info.intr_in.bEndpointAddress, info.intr_in.wMaxPacketSize, info.intr_in.bInterval);
    if (info.has_bulk_in) {
        printf("  Bulk IN    = 0x%02X (MPS=%u)\n", info.bulk_in.bEndpointAddress, info.bulk_in.wMaxPacketSize);
    }
    if (info.has_bulk_out) {
        printf("  Bulk OUT   = 0x%02X (MPS=%u)\n", info.bulk_out.bEndpointAddress, info.bulk_out.wMaxPacketSize);
    }

    /* Detach kernel driver (btusb) from interface 0 temporarily */
    struct usbdevfs_ioctl cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.ifno       = info.interface_num;
    cmd.ioctl_code = USBDEVFS_DISCONNECT;
    bool detached = (ioctl(fd, USBDEVFS_IOCTL, &cmd) == 0);

    int iface = info.interface_num;
    if (ioctl(fd, USBDEVFS_CLAIMINTERFACE, &iface) < 0) {
        perror("USBDEVFS_CLAIMINTERFACE");
        close(fd);
        return 1;
    }

    if (cold_reset) {
        printf("Performing USBDEVFS_RESET (simulating cold boot on Xbox 360)...\n");
        ioctl(fd, USBDEVFS_RELEASEINTERFACE, &iface);
        if (ioctl(fd, USBDEVFS_RESET, 0) < 0) {
            perror("USBDEVFS_RESET");
        }
        usleep(200000);
        detached = (ioctl(fd, USBDEVFS_IOCTL, &cmd) == 0);
        if (ioctl(fd, USBDEVFS_CLAIMINTERFACE, &iface) < 0) {
            perror("USBDEVFS_CLAIMINTERFACE after reset");
            close(fd);
            return 1;
        }
    }

    usbdevfs_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.fd         = fd;
    ctx.iface_num  = info.interface_num;
    ctx.ep_intr_in = info.intr_in.bEndpointAddress;

    bt_usb_hal_ops_t hal = {
        .ctx                = &ctx,
        .submit_control_out = usbdevfs_submit_control_out,
        .submit_intr_in     = usbdevfs_submit_intr_in
    };

    bt_usb_transport_t tr;
    if (bt_usb_transport_init(&tr, &info, &hal) != 0 ||
        bt_usb_transport_start_init(&tr) != 0) {
        fprintf(stderr, "Failed to start USB HCI init\n");
        goto cleanup;
    }

    while (tr.state != BT_INIT_READY && tr.state != BT_INIT_ERROR) {
        struct pollfd pfd = { .fd = fd, .events = POLLOUT };
        int pr = poll(&pfd, 1, 2000);
        if (pr <= 0) {
            fprintf(stderr, "Timeout waiting for USB Interrupt IN event (state=%d)\n", tr.state);
            break;
        }

        struct usbdevfs_urb *reaped = NULL;
        if (ioctl(fd, USBDEVFS_REAPURB, &reaped) < 0) {
            if (errno == EAGAIN) continue;
            perror("USBDEVFS_REAPURB");
            break;
        }
        ctx.urb_pending = false;
        if (reaped && reaped->status == 0 && reaped->actual_length > 0) {
            bt_usb_transport_on_intr_in(&tr, (const uint8_t *)reaped->buffer, (size_t)reaped->actual_length);
        } else if (reaped && reaped->status != 0) {
            fprintf(stderr, "URB error status=%d\n", reaped->status);
            break;
        }
    }

    if (tr.state == BT_INIT_READY) {
        printf("\n[SUCCESS] Direct USBDEVFS HCI Init Complete!\n");
        printf("  HCI Version  : 0x%02X (Bluetooth %s)\n",
               tr.local_ver.hci_version, hci_version_str(tr.local_ver.hci_version));
        printf("  HCI Revision : 0x%04X\n", tr.local_ver.hci_revision);
        printf("  LMP Version  : 0x%02X\n", tr.local_ver.lmp_version);
        printf("  Manufacturer : 0x%04X (%u)\n", tr.local_ver.manufacturer, tr.local_ver.manufacturer);
        printf("  LMP Subver   : 0x%04X\n", tr.local_ver.lmp_subversion);
        printf("  BD_ADDR      : %02X:%02X:%02X:%02X:%02X:%02X\n",
               tr.bd_addr.bd_addr[5], tr.bd_addr.bd_addr[4], tr.bd_addr.bd_addr[3],
               tr.bd_addr.bd_addr[2], tr.bd_addr.bd_addr[1], tr.bd_addr.bd_addr[0]);

        if (rtl_needs_firmware(&tr.local_ver)) {
            printf("\nUnpatched Realtek ROM detected (lmp_subver=0x%04X). Loading firmware...\n",
                   tr.local_ver.lmp_subversion);
            if (try_upload_realtek_fw(&ctx, &info) == 0) {
                if (bt_usb_transport_init(&tr, &info, &hal) == 0 &&
                    bt_usb_transport_start_init(&tr) == 0) {
                    while (tr.state != BT_INIT_READY && tr.state != BT_INIT_ERROR) {
                        struct pollfd pfd = { .fd = fd, .events = POLLOUT };
                        if (poll(&pfd, 1, 2000) <= 0) break;
                        struct usbdevfs_urb *reaped = NULL;
                        if (ioctl(fd, USBDEVFS_REAPURB, &reaped) < 0) {
                            if (errno == EAGAIN) continue;
                            break;
                        }
                        ctx.urb_pending = false;
                        if (reaped && reaped->status == 0 && reaped->actual_length > 0) {
                            bt_usb_transport_on_intr_in(&tr, (const uint8_t *)reaped->buffer,
                                                        (size_t)reaped->actual_length);
                        }
                    }
                    printf("[POST-FW] HCI Revision: 0x%04X | LMP Subver: 0x%04X\n",
                           tr.local_ver.hci_revision, tr.local_ver.lmp_subversion);
                }
            }
        }

        printf("\nStarting HCI Inquiry scan (~3.8s)...\n");
        if (bt_usb_transport_start_inquiry(&tr, 3) == 0) {
            while (tr.scan_active && !tr.scan_done) {
                struct pollfd pfd = { .fd = fd, .events = POLLOUT };
                int pr = poll(&pfd, 1, 6000);
                if (pr <= 0) {
                    fprintf(stderr, "Timeout waiting for Inquiry Complete\n");
                    bt_usb_transport_stop_inquiry(&tr);
                    break;
                }
                struct usbdevfs_urb *reaped = NULL;
                if (ioctl(fd, USBDEVFS_REAPURB, &reaped) < 0) {
                    if (errno == EAGAIN) continue;
                    perror("USBDEVFS_REAPURB");
                    break;
                }
                ctx.urb_pending = false;
                if (reaped && reaped->status == 0 && reaped->actual_length > 0) {
                    bt_usb_transport_on_intr_in(&tr, (const uint8_t *)reaped->buffer,
                                                (size_t)reaped->actual_length);
                }
            }
            printf("Inquiry complete: %zu device(s) discovered.\n", tr.scan_count);
            for (size_t i = 0; i < tr.scan_count; i++) {
                const hci_inquiry_item_t *it = &tr.scan_results[i];
                printf("  [%zu] %02X:%02X:%02X:%02X:%02X:%02X  CoD=0x%06X  RSSI=%d dBm  Name=\"%s\"\n",
                       i,
                       it->bd_addr[5], it->bd_addr[4], it->bd_addr[3],
                       it->bd_addr[2], it->bd_addr[1], it->bd_addr[0],
                       it->class_of_device,
                       it->has_rssi ? (int)it->rssi : 0,
                       it->name[0] ? it->name : "(unnamed)");
            }
        }
    } else {
        fprintf(stderr, "\n[FAIL] USBDEVFS HCI Init failed (state=%d, err=%d)\n", tr.state, tr.last_error);
    }

cleanup:
    if (ctx.urb_pending) {
        ioctl(fd, USBDEVFS_DISCARDURB, &ctx.intr_urb);
        struct usbdevfs_urb *tmp = NULL;
        ioctl(fd, USBDEVFS_REAPURBNDELAY, &tmp);
    }
    ioctl(fd, USBDEVFS_RELEASEINTERFACE, &iface);
    if (detached) {
        cmd.ioctl_code = USBDEVFS_CONNECT;
        ioctl(fd, USBDEVFS_IOCTL, &cmd);
    }
    close(fd);
    return (tr.state == BT_INIT_READY) ? 0 : 1;
}
