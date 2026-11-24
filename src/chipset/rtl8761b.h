#ifndef XBOX360_BT_CHIPSET_RTL8761B_H
#define XBOX360_BT_CHIPSET_RTL8761B_H

#include "../hci/hci_core.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RTL_OP_READ_ROM_VERSION   0xFC6Du
#define RTL_OP_DOWNLOAD_CMD       0xFC20u
#define RTL_FRAG_MAX_DATA         252u

typedef struct {
    const uint8_t *fw_data;
    size_t         fw_size;
    const uint8_t *cfg_data;
    size_t         cfg_size;
    uint32_t       fw_version;
    const uint8_t *patch_ptr;
    uint16_t       patch_len;
    size_t         total_len;     /* patch_len + cfg_size */
    uint8_t        rom_version;
} rtl_fw_image_t;

/**
 * Callback invoked by rtl_fw_upload_next() to send an HCI command (0xFC20).
 */
typedef int (*rtl_send_cmd_fn)(void *ctx, uint16_t opcode,
                               const uint8_t *params, uint8_t param_len);

/**
 * Check if Local Version Information indicates an unpatched Realtek ROM
 * that needs firmware upload (Manufacturer 0x005D, LMP Subversion 0x8761).
 */
bool rtl_needs_firmware(const hci_local_version_info_t *ver);

/**
 * Parse Realtek EPATCH v1 firmware buffer for the given rom_version.
 * Zero-allocation: points directly into fw_buf.
 * Returns 0 on success, negative on error.
 */
int rtl_fw_prepare(rtl_fw_image_t *img,
                   const uint8_t *fw_buf, size_t fw_size,
                   const uint8_t *cfg_buf, size_t cfg_size,
                   uint8_t rom_version);

/**
 * Compute total number of 0xFC20 download chunks required.
 */
size_t rtl_fw_chunk_count(const rtl_fw_image_t *img);

/**
 * Send chunk `chunk_idx` (0 .. rtl_fw_chunk_count(img)-1) via send_fn.
 * Automatically patches the last 4 bytes of the firmware patch with fw_version
 * and appends cfg_data at the end.
 */
int rtl_fw_send_chunk(const rtl_fw_image_t *img, size_t chunk_idx,
                      rtl_send_cmd_fn send_fn, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* XBOX360_BT_CHIPSET_RTL8761B_H */
