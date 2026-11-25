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

static const uint8_t k_ds4_a[6] = { 0x67, 0x2F, 0x82, 0x92, 0x29, 0x20 };
static const uint8_t k_ds4_b[6] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 };
static const uint8_t k_ds4_key[16] = {
    0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7,
    0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF
};

/* [PAR-05] Gravar A no Jogador 2, rereler e obter o mesmo endereco e o Jogador 2. */
static void test_par05_player_roundtrip(void) {
    bt_ipc_state_t ipc;
    bt_ipc_init(&ipc);
    assert(bt_manager_store_single_ds4(&ipc, k_ds4_a, k_ds4_key, 0x05, 2u, "DS4-A") == 0);

    char ini_buf[1024];
    size_t ini_len = bt_manager_export_ini(&ipc, ini_buf, sizeof(ini_buf));
    assert(ini_len > 0);

    bt_ipc_state_t reloaded;
    bt_ipc_init(&reloaded);
    assert(bt_manager_import_ini(&reloaded, ini_buf, ini_len) == 1);

    const bt_paired_device_t *found = bt_manager_find_paired(&reloaded, k_ds4_a);
    assert(found != NULL);
    assert(memcmp(found->bd_addr, k_ds4_a, 6u) == 0);
    assert(found->player == 2u);
    assert(memcmp(found->link_key, k_ds4_key, 16u) == 0);
    assert(found->class_of_device == 0x002508u);

    /* Jogador fora de 1..2 e recusado e nada e gravado. */
    bt_ipc_state_t bad;
    bt_ipc_init(&bad);
    assert(bt_manager_store_single_ds4(&bad, k_ds4_a, k_ds4_key, 0x05, 0u, "DS4-A") == -1);
    assert(bt_manager_store_single_ds4(&bad, k_ds4_a, k_ds4_key, 0x05, 3u, "DS4-A") == -1);
    assert(bt_manager_find_paired(&bad, k_ds4_a) == NULL);

    printf("[PASS] PAR-05 endereco e Jogador 2 gravados e relidos do INI\n");
}

/* [PAR-05] Gravar A no Jogador 1, exportar, importar e obter A, a link key e player == 1. */
static void test_par05_player1_roundtrip(void) {
    bt_ipc_state_t ipc;
    bt_ipc_init(&ipc);
    assert(bt_manager_store_single_ds4(&ipc, k_ds4_a, k_ds4_key, 0x05, 1u, "DS4-A") == 0);
    assert(bt_manager_find_paired(&ipc, k_ds4_a)->player == 1u);

    char ini_buf[1024];
    size_t ini_len = bt_manager_export_ini(&ipc, ini_buf, sizeof(ini_buf));
    assert(ini_len > 0);
    assert(strstr(ini_buf, "player=202992822F67,01\n") != NULL);
    assert(strstr(ini_buf, "player=202992822F67,02\n") == NULL);

    bt_ipc_state_t reloaded;
    bt_ipc_init(&reloaded);
    assert(bt_manager_import_ini(&reloaded, ini_buf, ini_len) == 1);

    const bt_paired_device_t *found = bt_manager_find_paired(&reloaded, k_ds4_a);
    assert(found != NULL);
    assert(memcmp(found->bd_addr, k_ds4_a, 6u) == 0);
    assert(memcmp(found->link_key, k_ds4_key, 16u) == 0);
    assert(found->player == 1u);
    assert(found->class_of_device == 0x002508u);

    printf("[PASS] PAR-05 endereco, link key e Jogador 1 gravados e relidos do INI\n");
}

/* [PAR-05] player= fora de 1..2 no INI nao e jogador atribuido. */
static void test_par05_import_rejects_invalid_player(void) {
    const char *dev = "dev=202992822F67,A0A1A2A3A4A5A6A7A8A9AAABACADAEAF,05,DS4-A\n";
    char ini[256];
    bt_ipc_state_t ipc;
    const bt_paired_device_t *found;

    /* player=03 sozinho: registro importado, sem jogador. */
    snprintf(ini, sizeof(ini), "%splayer=202992822F67,03\n", dev);
    bt_ipc_init(&ipc);
    assert(bt_manager_import_ini(&ipc, ini, strlen(ini)) == 1);
    found = bt_manager_find_paired(&ipc, k_ds4_a);
    assert(found != NULL);
    assert(found->player != 3u);
    assert(found->player == 0u);

    /* player=00 sozinho: nao e Jogador 1 nem 2. */
    snprintf(ini, sizeof(ini), "%splayer=202992822F67,00\n", dev);
    bt_ipc_init(&ipc);
    assert(bt_manager_import_ini(&ipc, ini, strlen(ini)) == 1);
    found = bt_manager_find_paired(&ipc, k_ds4_a);
    assert(found != NULL);
    assert(found->player == 0u);
    assert(found->player != 1u && found->player != 2u);

    /* Linha invalida depois de um jogador valido nao sobrescreve o jogador (03 e 00). */
    snprintf(ini, sizeof(ini), "%splayer=202992822F67,01\nplayer=202992822F67,03\n", dev);
    bt_ipc_init(&ipc);
    assert(bt_manager_import_ini(&ipc, ini, strlen(ini)) == 1);
    assert(bt_manager_find_paired(&ipc, k_ds4_a)->player == 1u);

    snprintf(ini, sizeof(ini), "%splayer=202992822F67,02\nplayer=202992822F67,00\n", dev);
    bt_ipc_init(&ipc);
    assert(bt_manager_import_ini(&ipc, ini, strlen(ini)) == 1);
    assert(bt_manager_find_paired(&ipc, k_ds4_a)->player == 2u);

    printf("[PASS] PAR-05 import ignora player= 00 e 03 (fora de 1..2)\n");
}

/* [PAR-05] A resposta positiva (0x040B) para A traz os 16 bytes gravados. */
static void test_par05_link_key_reply(void) {
    bt_ipc_state_t ipc;
    bt_ipc_init(&ipc);
    assert(bt_manager_store_single_ds4(&ipc, k_ds4_a, k_ds4_key, 0x05, 2u, "DS4-A") == 0);

    uint8_t cmd[HCI_MAX_CMD_SIZE];
    size_t n = bt_manager_build_link_key_reply(&ipc, k_ds4_a, cmd, sizeof(cmd));
    assert(n == (size_t)(HCI_CMD_HDR_SIZE + 22u));
    assert(hci_get_le16(cmd) == HCI_OP_LINK_KEY_REQ_REPLY);
    assert(hci_get_le16(cmd) != HCI_OP_LINK_KEY_REQ_NEG_REPLY);
    assert(cmd[2] == 22u);
    assert(memcmp(cmd + HCI_CMD_HDR_SIZE, k_ds4_a, 6u) == 0);
    assert(memcmp(cmd + HCI_CMD_HDR_SIZE + 6, k_ds4_key, 16u) == 0);

    /* Endereco nao gravado nao recebe resposta positiva. */
    assert(bt_manager_build_link_key_reply(&ipc, k_ds4_b, cmd, sizeof(cmd)) == 0);

    printf("[PASS] PAR-05 resposta de link key 0x040B com os 16 bytes gravados\n");
}

/* [LIM-01] Gravar B enquanto A existe falha e A permanece o unico registro. */
static void test_lim01_second_address_refused(void) {
    bt_ipc_state_t ipc;
    bt_ipc_init(&ipc);
    assert(bt_manager_store_single_ds4(&ipc, k_ds4_a, k_ds4_key, 0x05, 2u, "DS4-A") == 0);

    const uint8_t other_key[16] = { 0xFF, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA, 0x99, 0x88,
                                    0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x00 };
    assert(bt_manager_store_single_ds4(&ipc, k_ds4_b, other_key, 0x05, 1u, "DS4-B") < 0);
    assert(bt_manager_find_paired(&ipc, k_ds4_b) == NULL);

    size_t count = 0;
    for (size_t i = 0; i < BT_MAX_PAIRED_DEVICES; i++) {
        if (ipc.paired[i].valid) count++;
    }
    assert(count == 1u);
    const bt_paired_device_t *a = bt_manager_find_paired(&ipc, k_ds4_a);
    assert(a != NULL && a->player == 2u && memcmp(a->link_key, k_ds4_key, 16u) == 0);

    /* Regravar o mesmo endereco A continua permitido (atualiza o registro). */
    assert(bt_manager_store_single_ds4(&ipc, k_ds4_a, other_key, 0x05, 1u, "DS4-A") == 0);

    /* Um INI com dois dispositivos carrega somente o primeiro. */
    const char *two =
        "dev=202992822F67,A0A1A2A3A4A5A6A7A8A9AAABACADAEAF,05,DS4-A\n"
        "player=202992822F67,02\n"
        "dev=060504030201,FFEEDDCCBBAA99887766554433221100,05,DS4-B\n";
    bt_ipc_state_t loaded;
    bt_ipc_init(&loaded);
    assert(bt_manager_import_ini(&loaded, two, strlen(two)) == 1);
    assert(bt_manager_find_paired(&loaded, k_ds4_a) != NULL);
    assert(bt_manager_find_paired(&loaded, k_ds4_b) == NULL);

    printf("[PASS] LIM-01 segundo endereco recusado, A segue unico registro\n");
}

/* [PAR-05] Arquivo vazio nao produz resposta positiva. */
static void test_par05_empty_ini_no_reply(void) {
    bt_ipc_state_t ipc;
    bt_ipc_init(&ipc);
    assert(bt_manager_import_ini(&ipc, "", 0u) == 0);
    assert(bt_manager_import_ini(&ipc, "# bt360 paired controllers\n", 27u) == 0);

    uint8_t cmd[HCI_MAX_CMD_SIZE];
    memset(cmd, 0, sizeof(cmd));
    assert(bt_manager_build_link_key_reply(&ipc, k_ds4_a, cmd, sizeof(cmd)) == 0);
    assert(hci_get_le16(cmd) != HCI_OP_LINK_KEY_REQ_REPLY);

    printf("[PASS] PAR-05 INI vazio nao gera resposta positiva de link key\n");
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
    test_par05_player_roundtrip();
    test_par05_player1_roundtrip();
    test_par05_import_rejects_invalid_player();
    test_par05_link_key_reply();
    test_lim01_second_address_refused();
    test_par05_empty_ini_no_reply();
    test_slots_and_ui_ipc();
    test_interactive_button_remap();
    return 0;
}
