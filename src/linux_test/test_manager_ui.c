#include "../hid/ds4_bt.h"
#include "../manager/bt_manager.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_paired_ini_roundtrip(void) {
    bt_ipc_state_t ipc;
    bt_ipc_init(&ipc);

    const uint8_t bd1[6] = { 0x67, 0x2F, 0x82, 0x92, 0x29, 0x20 }; /* 20:29:92:82:2F:67 */
    const uint8_t lk1[16] = {
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
        0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x01
    };
    assert(bt_manager_upsert_paired(&ipc, bd1, lk1, 0x04, 0x002508u, "DS4-1") == 0);

    char ini_buf[1024];
    size_t ini_len = bt_manager_export_ini(&ipc, ini_buf, sizeof(ini_buf));
    assert(ini_len > 0);

    bt_ipc_state_t ipc2;
    bt_ipc_init(&ipc2);
    assert(bt_manager_import_ini(&ipc2, ini_buf, ini_len) == 1);

    const bt_paired_device_t *found = bt_manager_find_paired(&ipc2, bd1);
    assert(found != NULL);
    assert(memcmp(found->link_key, lk1, 16u) == 0);
    assert(found->key_type == 0x04);
    assert(strcmp(found->name, "DS4-1") == 0);

    assert(bt_manager_forget_paired(&ipc2, bd1) == 0);
    assert(bt_manager_find_paired(&ipc2, bd1) == NULL);

    printf("[PASS] Paired controller Link Key INI export/import & forget\n");
}

static void test_slots_and_ui_ipc(void) {
    bt_ipc_state_t ipc;
    bt_ipc_init(&ipc);

    const uint8_t bd_a[6] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 };
    const uint8_t bd_b[6] = { 0x11, 0x12, 0x13, 0x14, 0x15, 0x16 };
    assert(bt_manager_assign_slot(&ipc, bd_a, 0x0001u, "DS4-A") == 0);
    assert(bt_manager_assign_slot(&ipc, bd_b, 0x0002u, "DS4-B") == 1);
    assert(bt_manager_find_slot_by_handle(&ipc, 0x0002u) == 1);

    /* Populate scan result and drive UI model */
    ipc.scan_count = 1;
    memcpy(ipc.scan_items[0].bd_addr, bd_a, 6u);
    snprintf(ipc.scan_items[0].name, sizeof(ipc.scan_items[0].name), "Wireless Controller");

    bt_ui_model_t ui;
    bt_ui_model_init(&ui);
    assert(ui.pane == BT_UI_PANE_SCAN);

    /* Press Y -> START_SCAN */
    bt_ui_handle_input(&ui, &ipc, X360_BTN_Y);
    assert(ipc.cmd == BT_IPC_CMD_START_SCAN && ipc.cmd_seq == 1);

    /* Press A on scan item 0 -> PAIR_AND_CONNECT */
    bt_ui_handle_input(&ui, &ipc, X360_BTN_A);
    assert(ipc.cmd == BT_IPC_CMD_PAIR_AND_CONNECT && ipc.cmd_seq == 2);
    assert(memcmp(ipc.cmd_bd_addr, bd_a, 6u) == 0);

    /* Navigate Left -> PAIRED -> SLOTS, press X on slot 0 -> DISCONNECT_SLOT */
    bt_ui_handle_input(&ui, &ipc, X360_BTN_DPAD_LEFT);
    assert(ui.pane == BT_UI_PANE_PAIRED);
    bt_ui_handle_input(&ui, &ipc, X360_BTN_DPAD_LEFT);
    assert(ui.pane == BT_UI_PANE_SLOTS);
    bt_ui_handle_input(&ui, &ipc, X360_BTN_X);
    assert(ipc.cmd == BT_IPC_CMD_DISCONNECT_SLOT && ipc.cmd_slot == 0);

    /* Press B -> exit */
    bt_ui_handle_input(&ui, &ipc, X360_BTN_B);
    assert(ui.exit_requested);

    printf("[PASS] 4-player slot manager & NXE UI navigation/IPC commands\n");
}

static void test_interactive_button_remap(void) {
    bt_button_map_t map;
    bt_button_map_init_ds4_default(&map);

    bt_remap_wizard_t wiz;
    bt_remap_wizard_start(&wiz, 0, 0);
    assert(wiz.active && wiz.step == 0); /* Step 0 = Xbox A */

    /* User presses Square on their BT controller to bind it to Xbox [A] */
    assert(bt_remap_wizard_feed_raw(&wiz, (1u << BT_RAW_BIT_SQUARE), &map) == true);
    assert(map.raw_bit_for_x360[0] == BT_RAW_BIT_SQUARE);
    assert(wiz.step == 1); /* Step 1 = Xbox B */

    /* Release before next press */
    assert(bt_remap_wizard_feed_raw(&wiz, 0, &map) == false);

    /* User presses Cross on their BT controller to bind it to Xbox [B] */
    assert(bt_remap_wizard_feed_raw(&wiz, (1u << BT_RAW_BIT_CROSS), &map) == true);
    assert(map.raw_bit_for_x360[1] == BT_RAW_BIT_CROSS);

    /* Verify DS4 report with Square pressed now triggers Xbox [A]! */
    uint8_t ds4_pkt[11] = { 0xA1, 0x01, 0x80, 0x80, 0x80, 0x80, 0x18, 0x00, 0x00, 0x00, 0x00 };
    x360_gamepad_state_t gp;
    uint32_t raw_mask = 0;
    assert(ds4_bt_parse_hidp_mapped(ds4_pkt, sizeof(ds4_pkt), &map, &gp, &raw_mask) == 0);
    assert((raw_mask & (1u << BT_RAW_BIT_SQUARE)) != 0);
    assert((gp.buttons & X360_BTN_A) != 0);

    printf("[PASS] Interactive 1-by-1 button remap wizard & custom mapping parser\n");
}

int main(void) {
    test_paired_ini_roundtrip();
    test_slots_and_ui_ipc();
    test_interactive_button_remap();
    return 0;
}
