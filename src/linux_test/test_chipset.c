#include "../chipset/bt_chipset.h"
#include "../transport/bt_usb_transport.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_usb_match_policy(void) {
    /* Standard HCI dongles across brands/chipsets must be accepted. */
    assert(bt_chipset_match_usb(0x2B89u, 0x8761u, 0xE0u, 0x01u, 0x01u) == BT_USB_MATCH_ACCEPT);
    assert(bt_chipset_match_usb(0x0A12u, 0x0001u, 0xE0u, 0x01u, 0x01u) == BT_USB_MATCH_ACCEPT);
    assert(bt_chipset_match_usb(0x33FAu, 0x0010u, 0xE0u, 0x01u, 0x01u) == BT_USB_MATCH_ACCEPT);
    assert(bt_chipset_match_usb(0x10D7u, 0xB012u, 0xE0u, 0x01u, 0x01u) == BT_USB_MATCH_ACCEPT);
    assert(bt_chipset_match_usb(0x2357u, 0x0604u, 0xE0u, 0x01u, 0x01u) == BT_USB_MATCH_ACCEPT);
    assert(bt_chipset_match_usb(0x0BDAu, 0x8771u, 0xE0u, 0x01u, 0x01u) == BT_USB_MATCH_ACCEPT);

    /* Microsoft devices must always be ignored silently. */
    assert(bt_chipset_match_usb(0x045Eu, 0x0291u, 0xE0u, 0x01u, 0x01u) == BT_USB_MATCH_IGNORE);
    assert(bt_chipset_match_usb(0x045Eu, 0x028Eu, 0xFFu, 0x5Du, 0x01u) == BT_USB_MATCH_IGNORE);

    /* Non-HCI classes (HID, Mass Storage, arbitrary Vendor-Specific) must be ignored. */
    assert(bt_chipset_match_usb(0x0A12u, 0x0001u, 0x03u, 0x00u, 0x00u) == BT_USB_MATCH_IGNORE);
    assert(bt_chipset_match_usb(0x0951u, 0x1666u, 0x08u, 0x06u, 0x50u) == BT_USB_MATCH_IGNORE);
    assert(bt_chipset_match_usb(0x0A5Cu, 0x21E8u, 0xFFu, 0x01u, 0x01u) == BT_USB_MATCH_IGNORE);

    /* Intel bootloader adapters must be flagged as unsupported. */
    assert(bt_chipset_match_usb(0x8087u, 0x0029u, 0xE0u, 0x01u, 0x01u) == BT_USB_MATCH_UNSUPPORTED);

    printf("[PASS] bt_chipset_match_usb: generico 0xE0/0x01/0x01, protege 0x045E, recusa Intel\n");
}

static void test_chipset_identification(void) {
    bt_chipset_profile_t p;

    /* Realtek RTL8761BU (UGREEN, TP-Link UB500, generic 0BDA:8771) */
    hci_local_version_info_t rtl_bu = {
        .hci_version = 0x0Au, .hci_revision = 0x000Bu,
        .manufacturer = 0x005Du, .lmp_subversion = 0x8761u
    };
    bt_chipset_identify(0x2357u, 0x0604u, &rtl_bu, &p);
    assert(p.family == BT_CHIP_REALTEK);
    assert(p.needs_fw);
    assert(strcmp(p.fw_name, "rtl8761bu_fw.bin") == 0);
    assert(strcmp(p.cfg_name, "rtl8761bu_config.bin") == 0);
    assert((p.quirks & BT_QUIRK_RTL_EPATCH_V1) != 0u);

    /* Realtek RTL8761A */
    hci_local_version_info_t rtl_a = {
        .hci_version = 0x06u, .hci_revision = 0x000Au,
        .manufacturer = 0x005Du, .lmp_subversion = 0x8761u
    };
    bt_chipset_identify(0x0BDAu, 0x8761u, &rtl_a, &p);
    assert(p.family == BT_CHIP_REALTEK && p.needs_fw);
    assert(strcmp(p.fw_name, "rtl8761a_fw.bin") == 0);

    /* Realtek RTL8821C */
    hci_local_version_info_t rtl_8821c = {
        .hci_version = 0x09u, .hci_revision = 0x000Cu,
        .manufacturer = 0x005Du, .lmp_subversion = 0x8821u
    };
    bt_chipset_identify(0x0BDAu, 0xC821u, &rtl_8821c, &p);
    assert(p.family == BT_CHIP_REALTEK && p.needs_fw);
    assert(strcmp(p.fw_name, "rtl8821c_fw.bin") == 0);

    /* Realtek already patched (post-commit LMP 0x8A6B) */
    hci_local_version_info_t rtl_patched = {
        .hci_version = 0x0Au, .hci_revision = 0x09A9u,
        .manufacturer = 0x005Du, .lmp_subversion = 0x8A6Bu
    };
    bt_chipset_identify(0x2B89u, 0x8761u, &rtl_patched, &p);
    assert(p.family == BT_CHIP_REALTEK);
    assert(!p.needs_fw);
    assert(p.fw_name == NULL);

    /* CSR8510 A10 / cheap clone (no firmware needed) */
    hci_local_version_info_t csr = {
        .hci_version = 0x06u, .hci_revision = 0x22BBu,
        .manufacturer = 0x000Au, .lmp_subversion = 0x22BBu
    };
    bt_chipset_identify(0x0A12u, 0x0001u, &csr, &p);
    assert(p.family == BT_CHIP_CSR);
    assert(!p.needs_fw);
    assert((p.quirks & BT_QUIRK_CONSERVATIVE_EVENT_MASK) != 0u);
    assert((p.quirks & BT_QUIRK_POST_RESET_DELAY) != 0u);

    /* Barrot BT 5.3/5.4 (no firmware needed) */
    hci_local_version_info_t barrot = {
        .hci_version = 0x0Cu, .hci_revision = 0x0001u,
        .manufacturer = 0x08E7u, .lmp_subversion = 0x0001u
    };
    bt_chipset_identify(0x33FAu, 0x0010u, &barrot, &p);
    assert(p.family == BT_CHIP_BARROT);
    assert(!p.needs_fw);
    assert((p.quirks & BT_QUIRK_POST_RESET_DELAY) != 0u);

    /* Actions ATS2851 (identified by VID even if manufacturer differs) */
    hci_local_version_info_t actions = {
        .hci_version = 0x0Cu, .hci_revision = 0x0001u,
        .manufacturer = 0x03E0u, .lmp_subversion = 0x0001u
    };
    bt_chipset_identify(0x10D7u, 0xB012u, &actions, &p);
    assert(p.family == BT_CHIP_ACTIONS);
    assert(!p.needs_fw);

    /* Unknown modern generic HCI dongle */
    hci_local_version_info_t generic = {
        .hci_version = 0x0Bu, .hci_revision = 0x0001u,
        .manufacturer = 0x1234u, .lmp_subversion = 0x0001u
    };
    bt_chipset_identify(0x1234u, 0x5678u, &generic, &p);
    assert(p.family == BT_CHIP_GENERIC);
    assert(!p.needs_fw);

    printf("[PASS] bt_chipset_identify: Realtek/CSR/Barrot/Actions/Generic classificados\n");
}

int main(void) {
    test_usb_match_policy();
    test_chipset_identification();
    return 0;
}
