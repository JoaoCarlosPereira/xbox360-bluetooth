#include "bt_usb_transport.h"
#include <string.h>

bool bt_usb_is_hci_interface(uint8_t bInterfaceClass,
                             uint8_t bInterfaceSubClass,
                             uint8_t bInterfaceProtocol) {
    return (bInterfaceClass == USB_CLASS_WIRELESS_CONTROLLER) &&
           (bInterfaceSubClass == USB_SUBCLASS_RF_CONTROLLER) &&
           (bInterfaceProtocol == USB_PROTOCOL_BLUETOOTH_HCI);
}

int bt_usb_transport_init(bt_usb_transport_t *tr,
                          const bt_usb_device_info_t *dev,
                          const bt_usb_hal_ops_t *hal) {
    if (!tr || !dev || !hal || !hal->submit_control_out || !hal->submit_intr_in) {
        return -1;
    }
    if (!dev->has_intr_in) {
        return -2;
    }

    memset(tr, 0, sizeof(*tr));
    tr->dev = *dev;
    tr->hal = *hal;
    tr->state = BT_INIT_IDLE;
    return 0;
}

static int send_hci_cmd(bt_usb_transport_t *tr, uint16_t opcode,
                        const uint8_t *params, uint8_t param_len) {
    uint8_t cmd_buf[HCI_MAX_CMD_SIZE];
    size_t cmd_len = hci_build_command(cmd_buf, sizeof(cmd_buf), opcode, params, param_len);
    if (cmd_len == 0) {
        return -1;
    }
    return tr->hal.submit_control_out(tr->hal.ctx,
                                      BT_USB_REQ_TYPE_HCI_CMD,
                                      BT_USB_REQ_HCI_CMD,
                                      0x0000u,
                                      (uint16_t)tr->dev.interface_num,
                                      cmd_buf,
                                      (uint16_t)cmd_len);
}

static int arm_intr_in(bt_usb_transport_t *tr) {
    uint16_t max_pkt = tr->dev.intr_in.wMaxPacketSize;
    if (max_pkt == 0 || max_pkt > sizeof(tr->evt_rx_chunk)) {
        max_pkt = sizeof(tr->evt_rx_chunk);
    }
    return tr->hal.submit_intr_in(tr->hal.ctx, tr->evt_rx_chunk, max_pkt);
}

int bt_usb_transport_start_init(bt_usb_transport_t *tr) {
    if (!tr) {
        return -1;
    }
    tr->evt_accum_len = 0;
    tr->has_local_ver = false;
    tr->has_bd_addr = false;
    tr->last_error = 0;

    if (arm_intr_in(tr) != 0) {
        tr->state = BT_INIT_ERROR;
        tr->last_error = -10;
        return -10;
    }

    tr->state = BT_INIT_WAIT_RESET;
    if (send_hci_cmd(tr, HCI_OP_RESET, NULL, 0) != 0) {
        tr->state = BT_INIT_ERROR;
        tr->last_error = -11;
        return -11;
    }
    return 0;
}

static void record_inquiry_items(bt_usb_transport_t *tr,
                                 const hci_inquiry_item_t *items, int count) {
    for (int i = 0; i < count; i++) {
        const hci_inquiry_item_t *src = &items[i];
        int match = -1;
        for (size_t j = 0; j < tr->scan_count; j++) {
            if (memcmp(tr->scan_results[j].bd_addr, src->bd_addr, 6u) == 0) {
                match = (int)j;
                break;
            }
        }
        if (match >= 0) {
            hci_inquiry_item_t *dst = &tr->scan_results[match];
            dst->class_of_device = src->class_of_device;
            if (src->has_rssi) {
                dst->rssi = src->rssi;
                dst->has_rssi = true;
            }
            if (src->name[0] != '\0') {
                memcpy(dst->name, src->name, sizeof(dst->name));
            }
        } else if (tr->scan_count < BT_MAX_DISCOVERED_DEVICES) {
            tr->scan_results[tr->scan_count++] = *src;
        }
    }
}

static void handle_hci_event(bt_usb_transport_t *tr,
                             const uint8_t *evt, size_t evt_len) {
    if (evt_len < HCI_EVT_HDR_SIZE) {
        return;
    }

    uint8_t evt_code = evt[0];

    if (evt_code == HCI_EVT_INQUIRY_RESULT ||
        evt_code == HCI_EVT_INQUIRY_RESULT_RSSI ||
        evt_code == HCI_EVT_EXTENDED_INQUIRY_RESULT) {
        hci_inquiry_item_t items[8];
        int n = hci_decode_inquiry_event(evt, evt_len, items, 8);
        if (n > 0) {
            record_inquiry_items(tr, items, n);
        }
        return;
    }

    if (evt_code == HCI_EVT_INQUIRY_COMPLETE) {
        tr->scan_active = false;
        tr->scan_done = true;
        return;
    }

    if (evt_code == HCI_EVT_COMMAND_STATUS) {
        hci_cmd_status_t cs;
        if (hci_parse_command_status(evt, evt_len, &cs) == 0) {
            if (cs.opcode == HCI_OP_INQUIRY && cs.status != 0x00) {
                tr->scan_active = false;
                tr->scan_done = true;
                tr->last_error = -(int)cs.status;
            }
        }
        return;
    }

    if (evt_code != HCI_EVT_COMMAND_COMPLETE) {
        return;
    }

    hci_cmd_complete_t cc;
    if (hci_parse_command_complete(evt, evt_len, &cc) != 0) {
        return;
    }

    if (cc.opcode == HCI_OP_SET_EVENT_MASK && tr->scan_active) {
        uint8_t mode = 0x02u; /* Extended Inquiry Result */
        send_hci_cmd(tr, HCI_OP_WRITE_INQUIRY_MODE, &mode, 1);
        return;
    }

    if (cc.opcode == HCI_OP_WRITE_INQUIRY_MODE && tr->scan_active) {
        /* GIAC LAP = 0x9E8B33 (little-endian: 33 8B 9E), length, unlimited responses */
        uint8_t inq_params[5] = { 0x33u, 0x8Bu, 0x9Eu, tr->scan_duration, 0x00u };
        if (send_hci_cmd(tr, HCI_OP_INQUIRY, inq_params, sizeof(inq_params)) != 0) {
            tr->scan_active = false;
            tr->scan_done = true;
            tr->last_error = -30;
        }
        return;
    }

    switch (tr->state) {
        case BT_INIT_WAIT_RESET:
            if (cc.opcode == HCI_OP_RESET) {
                if (cc.params_len >= 1 && cc.params[0] == 0x00) {
                    tr->state = BT_INIT_WAIT_LOCAL_VER;
                    if (send_hci_cmd(tr, HCI_OP_READ_LOCAL_VERSION_INFO, NULL, 0) != 0) {
                        tr->state = BT_INIT_ERROR;
                        tr->last_error = -20;
                    }
                } else {
                    tr->state = BT_INIT_ERROR;
                    tr->last_error = -21;
                }
            }
            break;

        case BT_INIT_WAIT_LOCAL_VER:
            if (cc.opcode == HCI_OP_READ_LOCAL_VERSION_INFO) {
                if (hci_decode_local_version(&cc, &tr->local_ver) == 0 &&
                    tr->local_ver.status == 0x00) {
                    tr->has_local_ver = true;
                    tr->state = BT_INIT_WAIT_BD_ADDR;
                    if (send_hci_cmd(tr, HCI_OP_READ_BD_ADDR, NULL, 0) != 0) {
                        tr->state = BT_INIT_ERROR;
                        tr->last_error = -22;
                    }
                } else {
                    tr->state = BT_INIT_ERROR;
                    tr->last_error = -23;
                }
            }
            break;

        case BT_INIT_WAIT_BD_ADDR:
            if (cc.opcode == HCI_OP_READ_BD_ADDR) {
                if (hci_decode_bd_addr(&cc, &tr->bd_addr) == 0 &&
                    tr->bd_addr.status == 0x00) {
                    tr->has_bd_addr = true;
                    tr->state = BT_INIT_READY;
                } else {
                    tr->state = BT_INIT_ERROR;
                    tr->last_error = -24;
                }
            }
            break;

        default:
            break;
    }
}

int bt_usb_transport_start_inquiry(bt_usb_transport_t *tr, uint8_t duration_1280ms) {
    if (!tr || tr->state != BT_INIT_READY) {
        return -1;
    }
    if (duration_1280ms < 1u) duration_1280ms = 1u;
    if (duration_1280ms > 48u) duration_1280ms = 48u;

    tr->scan_active = true;
    tr->scan_done = false;
    tr->scan_count = 0;
    tr->scan_duration = duration_1280ms;

    if (arm_intr_in(tr) != 0) {
        tr->scan_active = false;
        return -2;
    }

    /* Enable all standard + Extended Inquiry Result events (bits 0..63) */
    static const uint8_t event_mask[8] = {
        0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0x3Fu
    };
    return send_hci_cmd(tr, HCI_OP_SET_EVENT_MASK, event_mask, sizeof(event_mask));
}

int bt_usb_transport_stop_inquiry(bt_usb_transport_t *tr) {
    if (!tr || !tr->scan_active) {
        return 0;
    }
    tr->scan_active = false;
    return send_hci_cmd(tr, HCI_OP_INQUIRY_CANCEL, NULL, 0);
}

void bt_usb_transport_on_intr_in(bt_usb_transport_t *tr,
                                 const uint8_t *data, size_t len) {
    if (!tr || !data || len == 0) {
        return;
    }

    if (tr->evt_accum_len + len > sizeof(tr->evt_accum)) {
        /* Overflow guard: reset accumulator */
        tr->evt_accum_len = 0;
        return;
    }

    memcpy(tr->evt_accum + tr->evt_accum_len, data, len);
    tr->evt_accum_len += len;

    while (tr->evt_accum_len >= HCI_EVT_HDR_SIZE) {
        size_t expected = HCI_EVT_HDR_SIZE + (size_t)tr->evt_accum[1];
        if (tr->evt_accum_len < expected) {
            break; /* Wait for remaining USB frames */
        }

        handle_hci_event(tr, tr->evt_accum, expected);

        size_t rem = tr->evt_accum_len - expected;
        if (rem > 0) {
            memmove(tr->evt_accum, tr->evt_accum + expected, rem);
        }
        tr->evt_accum_len = rem;
    }

    if ((tr->state != BT_INIT_READY && tr->state != BT_INIT_ERROR) || tr->scan_active) {
        arm_intr_in(tr);
    }
}
