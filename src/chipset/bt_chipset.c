#include "bt_chipset.h"
#include "../transport/bt_usb_transport.h"
#include <stddef.h>

bt_usb_match_result_t bt_chipset_match_usb(uint16_t vid, uint16_t pid,
                                           uint8_t iface_class,
                                           uint8_t iface_subclass,
                                           uint8_t iface_proto) {
    (void)pid;
    if (!bt_usb_is_hci_interface(iface_class, iface_subclass, iface_proto)) {
        return BT_USB_MATCH_IGNORE;
    }
    /* Never risk claiming Microsoft hardware on the Xbox 360 USB bus. */
    if (vid == BT_USB_VID_MICROSOFT) {
        return BT_USB_MATCH_IGNORE;
    }
    /* Intel M.2/USB controllers start in proprietary bootloader mode (.sfi/.ddc). */
    if (vid == BT_USB_VID_INTEL) {
        return BT_USB_MATCH_UNSUPPORTED;
    }
    return BT_USB_MATCH_ACCEPT;
}

void bt_chipset_identify(uint16_t vid, uint16_t pid,
                         const hci_local_version_info_t *ver,
                         bt_chipset_profile_t *out) {
    (void)pid;
    if (!out) return;

    out->family   = BT_CHIP_GENERIC;
    out->name     = "Generic HCI";
    out->quirks   = BT_QUIRK_NONE;
    out->needs_fw = false;
    out->fw_name  = NULL;
    out->cfg_name = NULL;

    const uint16_t mfr     = ver ? ver->manufacturer   : 0u;
    const uint16_t lmp     = ver ? ver->lmp_subversion : 0u;
    const uint16_t hci_rev = ver ? ver->hci_revision   : 0u;
    const uint8_t  hci_ver = ver ? ver->hci_version    : 0u;

    if (mfr == BT_MFR_REALTEK) {
        out->family = BT_CHIP_REALTEK;
        out->name   = "Realtek";
        out->quirks = BT_QUIRK_ORPHAN_HANDLE_CLEANUP;
        if (lmp == 0x8761u) {
            out->quirks  |= BT_QUIRK_RTL_EPATCH_V1;
            out->needs_fw = true;
            if (hci_rev == 0x000Au) {
                out->name     = "Realtek RTL8761A";
                out->fw_name  = "rtl8761a_fw.bin";
                out->cfg_name = "rtl8761a_config.bin";
            } else if (hci_rev == 0x000Cu) {
                out->name     = "Realtek RTL8761BTV";
                out->fw_name  = "rtl8761b_fw.bin";
                out->cfg_name = "rtl8761b_config.bin";
            } else {
                out->name     = "Realtek RTL8761BU";
                out->fw_name  = "rtl8761bu_fw.bin";
                out->cfg_name = "rtl8761bu_config.bin";
            }
        } else if (lmp == 0x8821u && hci_rev == 0x000Cu) {
            out->name     = "Realtek RTL8821C";
            out->quirks  |= BT_QUIRK_RTL_EPATCH_V1;
            out->needs_fw = true;
            out->fw_name  = "rtl8821c_fw.bin";
            out->cfg_name = "rtl8821c_config.bin";
        }
        return;
    }

    if (mfr == BT_MFR_BARROT || vid == BT_USB_VID_BARROT) {
        out->family = BT_CHIP_BARROT;
        out->name   = "Barrot";
        out->quirks = BT_QUIRK_POST_RESET_DELAY;
        return;
    }

    if (mfr == BT_MFR_ACTIONS || vid == BT_USB_VID_ACTIONS) {
        out->family = BT_CHIP_ACTIONS;
        out->name   = "Actions";
        out->quirks = BT_QUIRK_POST_RESET_DELAY;
        return;
    }

    if (mfr == BT_MFR_CSR || vid == BT_USB_VID_CSR) {
        out->family = BT_CHIP_CSR;
        out->name   = "CSR/Qualcomm";
        out->quirks = BT_QUIRK_CONSERVATIVE_EVENT_MASK | BT_QUIRK_POST_RESET_DELAY;
        return;
    }

    if (mfr == BT_MFR_BROADCOM) {
        out->family = BT_CHIP_BROADCOM;
        out->name   = "Broadcom";
        return;
    }

    /* Older BT 2.x/4.0 generic controllers benefit from the conservative event mask. */
    if (ver && hci_ver < 0x07u) {
        out->quirks = BT_QUIRK_CONSERVATIVE_EVENT_MASK;
    }
}
