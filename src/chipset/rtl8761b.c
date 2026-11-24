#include "rtl8761b.h"
#include <string.h>

static uint32_t get_le32(const uint8_t *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

bool rtl_needs_firmware(const hci_local_version_info_t *ver) {
    if (!ver) return false;
    /* Realtek Bluetooth SIG ID = 0x005D, RTL8761B/BU ROM lmp_subver = 0x8761 */
    return (ver->manufacturer == 0x005Du) && (ver->lmp_subversion == 0x8761u);
}

int rtl_fw_prepare(rtl_fw_image_t *img,
                   const uint8_t *fw_buf, size_t fw_size,
                   const uint8_t *cfg_buf, size_t cfg_size,
                   uint8_t rom_version) {
    static const uint8_t epatch_magic[8] = { 'R', 'e', 'a', 'l', 't', 'e', 'c', 'h' };
    if (!img || !fw_buf || fw_size < 14u) {
        return -1;
    }
    if (memcmp(fw_buf, epatch_magic, 8u) != 0) {
        return -2;
    }

    uint32_t fw_version  = get_le32(fw_buf + 8u);
    uint16_t num_patches = hci_get_le16(fw_buf + 12u);
    size_t table_bytes   = (size_t)num_patches * 8u; /* chip_id(2) + len(2) + offset(4) */
    if (num_patches == 0 || fw_size < 14u + table_bytes) {
        return -3;
    }

    uint16_t want_chip_id = (uint16_t)(rom_version + 1u);
    const uint8_t *chips   = fw_buf + 14u;
    const uint8_t *lens    = chips + (size_t)num_patches * 2u;
    const uint8_t *offsets = lens + (size_t)num_patches * 2u;

    for (uint16_t i = 0; i < num_patches; i++) {
        uint16_t chip_id = hci_get_le16(chips + (size_t)i * 2u);
        if (chip_id == want_chip_id) {
            uint16_t plen   = hci_get_le16(lens + (size_t)i * 2u);
            uint32_t offset = get_le32(offsets + (size_t)i * 4u);
            if (plen < 4u || (size_t)offset + (size_t)plen > fw_size) {
                return -4;
            }
            memset(img, 0, sizeof(*img));
            img->fw_data     = fw_buf;
            img->fw_size     = fw_size;
            img->cfg_data    = (cfg_buf && cfg_size > 0) ? cfg_buf : NULL;
            img->cfg_size    = (cfg_buf && cfg_size > 0) ? cfg_size : 0u;
            img->fw_version  = fw_version;
            img->patch_ptr   = fw_buf + offset;
            img->patch_len   = plen;
            img->total_len   = (size_t)plen + img->cfg_size;
            img->rom_version = rom_version;
            return 0;
        }
    }
    return -5;
}

size_t rtl_fw_chunk_count(const rtl_fw_image_t *img) {
    if (!img || img->total_len == 0) {
        return 0;
    }
    return (img->total_len + RTL_FRAG_MAX_DATA - 1u) / RTL_FRAG_MAX_DATA;
}

int rtl_fw_send_chunk(const rtl_fw_image_t *img, size_t chunk_idx,
                      rtl_send_cmd_fn send_fn, void *ctx) {
    if (!img || !send_fn) {
        return -1;
    }
    size_t total_chunks = rtl_fw_chunk_count(img);
    if (chunk_idx >= total_chunks) {
        return -2;
    }

    size_t offset = chunk_idx * RTL_FRAG_MAX_DATA;
    size_t frag_len = img->total_len - offset;
    if (frag_len > RTL_FRAG_MAX_DATA) {
        frag_len = RTL_FRAG_MAX_DATA;
    }

    uint8_t params[1u + RTL_FRAG_MAX_DATA];
    uint8_t idx_byte = (uint8_t)(chunk_idx & 0x7Fu);
    if (chunk_idx + 1u == total_chunks) {
        idx_byte |= 0x80u; /* Last fragment marker */
    }
    params[0] = idx_byte;

    uint8_t ver_bytes[4];
    put_le32(ver_bytes, img->fw_version);
    size_t ver_start = (size_t)img->patch_len - 4u;

    for (size_t i = 0; i < frag_len; i++) {
        size_t pos = offset + i;
        if (pos < ver_start) {
            params[1u + i] = img->patch_ptr[pos];
        } else if (pos < (size_t)img->patch_len) {
            params[1u + i] = ver_bytes[pos - ver_start];
        } else {
            params[1u + i] = img->cfg_data[pos - (size_t)img->patch_len];
        }
    }

    return send_fn(ctx, RTL_OP_DOWNLOAD_CMD, params, (uint8_t)(1u + frag_len));
}
