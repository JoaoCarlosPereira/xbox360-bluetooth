#ifndef XBOX360_BT_CHIPSET_H
#define XBOX360_BT_CHIPSET_H

#include "../hci/hci_core.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bluetooth SIG Company Identifiers (Manufacturer_Name from 0x1001) */
#define BT_MFR_INTEL        0x0002u
#define BT_MFR_CSR          0x000Au
#define BT_MFR_BROADCOM     0x000Fu
#define BT_MFR_REALTEK      0x005Du
#define BT_MFR_ACTIONS      0x03E0u
#define BT_MFR_BARROT       0x08E7u

/* Well-known USB Vendor IDs */
#define BT_USB_VID_MICROSOFT 0x045Eu
#define BT_USB_VID_INTEL     0x8087u
#define BT_USB_VID_CSR       0x0A12u
#define BT_USB_VID_REALTEK   0x0BDAu
#define BT_USB_VID_ACTIONS   0x10D7u
#define BT_USB_VID_TPLINK    0x2357u
#define BT_USB_VID_UGREEN    0x2B89u
#define BT_USB_VID_BARROT    0x33FAu

typedef enum {
    BT_USB_MATCH_IGNORE = 0,      /* Not a Bluetooth HCI interface, or protected host device */
    BT_USB_MATCH_ACCEPT = 1,      /* Standard Bluetooth USB HCI adapter */
    BT_USB_MATCH_UNSUPPORTED = 2  /* Known incompatible Bluetooth USB adapter (e.g. Intel bootloader) */
} bt_usb_match_result_t;

typedef enum {
    BT_CHIP_GENERIC = 0,
    BT_CHIP_REALTEK,
    BT_CHIP_CSR,
    BT_CHIP_BARROT,
    BT_CHIP_ACTIONS,
    BT_CHIP_BROADCOM
} bt_chip_family_t;

/* Quirk bitmask */
#define BT_QUIRK_NONE                    0u
#define BT_QUIRK_RTL_EPATCH_V1           (1u << 0)
#define BT_QUIRK_CONSERVATIVE_EVENT_MASK (1u << 1)
#define BT_QUIRK_POST_RESET_DELAY        (1u << 2)
#define BT_QUIRK_ORPHAN_HANDLE_CLEANUP   (1u << 3)

typedef struct {
    bt_chip_family_t family;
    const char      *name;
    uint32_t         quirks;
    bool             needs_fw;
    const char      *fw_name;  /* Basename, e.g. "rtl8761bu_fw.bin", or NULL */
    const char      *cfg_name; /* Basename, e.g. "rtl8761bu_config.bin", or NULL */
} bt_chipset_profile_t;

/**
 * Decide whether a USB interface descriptor should be claimed as a Bluetooth HCI adapter.
 * Accepts standard 0xE0/0x01/0x01 adapters while protecting Microsoft devices (0x045E)
 * and rejecting known incompatible bootloaders (Intel 0x8087).
 */
bt_usb_match_result_t bt_chipset_match_usb(uint16_t vid, uint16_t pid,
                                           uint8_t iface_class,
                                           uint8_t iface_subclass,
                                           uint8_t iface_proto);

/**
 * Classify a Bluetooth adapter after HCI_Read_Local_Version_Information completes.
 * Populates chipset family, human-readable name, quirk flags, and optional firmware filenames.
 */
void bt_chipset_identify(uint16_t vid, uint16_t pid,
                         const hci_local_version_info_t *ver,
                         bt_chipset_profile_t *out);

#ifdef __cplusplus
}
#endif

#endif /* XBOX360_BT_CHIPSET_H */
