#include "hci_core.h"
#include <string.h>

size_t hci_build_command(uint8_t *out_buf, size_t out_cap,
                         uint16_t opcode, const uint8_t *params, uint8_t param_len) {
    if (!out_buf || out_cap < (size_t)(HCI_CMD_HDR_SIZE + param_len)) {
        return 0;
    }
    if (param_len > 0 && !params) {
        return 0;
    }

    hci_put_le16(out_buf, opcode);
    out_buf[2] = param_len;
    if (param_len > 0) {
        memcpy(out_buf + HCI_CMD_HDR_SIZE, params, param_len);
    }
    return (size_t)(HCI_CMD_HDR_SIZE + param_len);
}

size_t hci_build_link_key_reply(uint8_t *out_buf, size_t out_cap,
                                const uint8_t bd_addr[6], const uint8_t link_key[16]) {
    if (!bd_addr || !link_key) {
        return 0;
    }
    uint8_t params[22];
    memcpy(params, bd_addr, 6u);
    memcpy(params + 6, link_key, 16u);
    return hci_build_command(out_buf, out_cap, HCI_OP_LINK_KEY_REQ_REPLY,
                             params, (uint8_t)sizeof(params));
}

int hci_parse_command_complete(const uint8_t *evt_buf, size_t evt_len,
                               hci_cmd_complete_t *out_cc) {
    if (!evt_buf || !out_cc || evt_len < 5u) {
        return -1;
    }
    if (evt_buf[0] != HCI_EVT_COMMAND_COMPLETE) {
        return -2;
    }

    uint8_t total_param_len = evt_buf[1];
    if (evt_len < (size_t)(HCI_EVT_HDR_SIZE + total_param_len) || total_param_len < 3u) {
        return -3;
    }

    out_cc->num_hci_cmd_packets = evt_buf[2];
    out_cc->opcode = hci_get_le16(evt_buf + 3);
    out_cc->params = evt_buf + 5;
    out_cc->params_len = (uint8_t)(total_param_len - 3u);
    return 0;
}

int hci_decode_local_version(const hci_cmd_complete_t *cc,
                             hci_local_version_info_t *out_info) {
    if (!cc || !out_info || cc->opcode != HCI_OP_READ_LOCAL_VERSION_INFO) {
        return -1;
    }
    if (cc->params_len < 9u) {
        return -2;
    }

    out_info->status         = cc->params[0];
    out_info->hci_version    = cc->params[1];
    out_info->hci_revision   = hci_get_le16(cc->params + 2);
    out_info->lmp_version    = cc->params[4];
    out_info->manufacturer   = hci_get_le16(cc->params + 5);
    out_info->lmp_subversion = hci_get_le16(cc->params + 7);
    return 0;
}

int hci_decode_bd_addr(const hci_cmd_complete_t *cc,
                       hci_bd_addr_info_t *out_addr) {
    if (!cc || !out_addr || cc->opcode != HCI_OP_READ_BD_ADDR) {
        return -1;
    }
    if (cc->params_len < 7u) {
        return -2;
    }

    out_addr->status = cc->params[0];
    memcpy(out_addr->bd_addr, cc->params + 1, 6u);
    return 0;
}

int hci_parse_command_status(const uint8_t *evt_buf, size_t evt_len,
                             hci_cmd_status_t *out_cs) {
    if (!evt_buf || !out_cs || evt_len < 6u) {
        return -1;
    }
    if (evt_buf[0] != HCI_EVT_COMMAND_STATUS || evt_buf[1] < 4u) {
        return -2;
    }
    out_cs->status              = evt_buf[2];
    out_cs->num_hci_cmd_packets = evt_buf[3];
    out_cs->opcode              = hci_get_le16(evt_buf + 4);
    return 0;
}

static void eir_extract_name(const uint8_t *eir, size_t eir_len, char *out_name, size_t out_cap) {
    if (!out_name || out_cap == 0) return;
    out_name[0] = '\0';

    size_t off = 0;
    while (off < eir_len) {
        uint8_t field_len = eir[off];
        if (field_len == 0 || off + 1u + field_len > eir_len) break;
        uint8_t ad_type = eir[off + 1u];
        /* 0x08 = Shortened Local Name, 0x09 = Complete Local Name */
        if (ad_type == 0x08u || ad_type == 0x09u) {
            size_t str_len = (size_t)(field_len - 1u);
            if (str_len >= out_cap) str_len = out_cap - 1u;
            memcpy(out_name, eir + off + 2u, str_len);
            out_name[str_len] = '\0';
            if (ad_type == 0x09u) return;
        }
        off += 1u + (size_t)field_len;
    }
}

int hci_decode_inquiry_event(const uint8_t *evt_buf, size_t evt_len,
                             hci_inquiry_item_t *out_items, size_t max_items) {
    if (!evt_buf || !out_items || max_items == 0 || evt_len < 3u) {
        return -1;
    }

    uint8_t evt_code  = evt_buf[0];
    uint8_t param_len = evt_buf[1];
    if (evt_len < (size_t)(HCI_EVT_HDR_SIZE + param_len) || param_len < 1u) {
        return -2;
    }

    uint8_t num_resp = evt_buf[2];
    const uint8_t *p = evt_buf + 3;
    size_t avail = (size_t)(param_len - 1u);
    size_t count = 0;

    if (evt_code == HCI_EVT_INQUIRY_RESULT) {
        for (uint8_t i = 0; i < num_resp && count < max_items && avail >= 14u; i++) {
            hci_inquiry_item_t *it = &out_items[count++];
            memset(it, 0, sizeof(*it));
            memcpy(it->bd_addr, p, 6u);
            it->page_scan_rep_mode = p[6];
            it->class_of_device = (uint32_t)p[9] | ((uint32_t)p[10] << 8) | ((uint32_t)p[11] << 16);
            it->clock_offset = hci_get_le16(p + 12);
            it->has_rssi = false;
            p += 14u;
            avail -= 14u;
        }
        return (int)count;
    }

    if (evt_code == HCI_EVT_INQUIRY_RESULT_RSSI) {
        for (uint8_t i = 0; i < num_resp && count < max_items && avail >= 14u; i++) {
            hci_inquiry_item_t *it = &out_items[count++];
            memset(it, 0, sizeof(*it));
            memcpy(it->bd_addr, p, 6u);
            it->page_scan_rep_mode = p[6];
            it->class_of_device = (uint32_t)p[8] | ((uint32_t)p[9] << 8) | ((uint32_t)p[10] << 16);
            it->clock_offset = hci_get_le16(p + 11);
            it->rssi = (int8_t)p[13];
            it->has_rssi = true;
            p += 14u;
            avail -= 14u;
        }
        return (int)count;
    }

    if (evt_code == HCI_EVT_EXTENDED_INQUIRY_RESULT) {
        if (num_resp >= 1u && avail >= 14u) {
            hci_inquiry_item_t *it = &out_items[count++];
            memset(it, 0, sizeof(*it));
            memcpy(it->bd_addr, p, 6u);
            it->page_scan_rep_mode = p[6];
            it->class_of_device = (uint32_t)p[8] | ((uint32_t)p[9] << 8) | ((uint32_t)p[10] << 16);
            it->clock_offset = hci_get_le16(p + 11);
            it->rssi = (int8_t)p[13];
            it->has_rssi = true;
            if (avail > 14u) {
                eir_extract_name(p + 14u, avail - 14u, it->name, sizeof(it->name));
            }
        }
        return (int)count;
    }

    return -3;
}

const char *hci_version_str(uint8_t hci_version) {
    switch (hci_version) {
        case 0x06: return "4.0";
        case 0x07: return "4.1";
        case 0x08: return "4.2";
        case 0x09: return "5.0";
        case 0x0A: return "5.1";
        case 0x0B: return "5.2";
        case 0x0C: return "5.3";
        case 0x0D: return "5.4";
        case 0x0E: return "6.0";
        default:   return "Unknown";
    }
}
