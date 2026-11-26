#include "../hid/ds4_bt.h"
#include "../manager/bt_manager.h"
#include "../manager/bt_phase1.h"

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

    /* Jogador fora de 1..4 e recusado e nada e gravado. 3 e 4 passam. */
    bt_ipc_state_t bad;
    bt_ipc_init(&bad);
    assert(bt_manager_store_single_ds4(&bad, k_ds4_a, k_ds4_key, 0x05, 0u, "DS4-A") == -1);
    assert(bt_manager_store_single_ds4(&bad, k_ds4_a, k_ds4_key, 0x05, 5u, "DS4-A") == -1);
    assert(bt_manager_find_paired(&bad, k_ds4_a) == NULL);
    assert(bt_manager_store_single_ds4(&bad, k_ds4_a, k_ds4_key, 0x05, 3u, "DS4-A") == 0);
    assert(bt_manager_find_paired(&bad, k_ds4_a)->player == 3u);
    assert(bt_manager_store_single_ds4(&bad, k_ds4_a, k_ds4_key, 0x05, 4u, "DS4-A") == 0);
    assert(bt_manager_find_paired(&bad, k_ds4_a)->player == 4u);

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

/* [PAR-05] player= fora de 1..4 no INI nao e jogador atribuido. 3 e 4 valem. */
static void test_par05_import_rejects_invalid_player(void) {
    const char *dev = "dev=202992822F67,A0A1A2A3A4A5A6A7A8A9AAABACADAEAF,05,DS4-A\n";
    char ini[256];
    bt_ipc_state_t ipc;
    const bt_paired_device_t *found;

    /* player=03 sozinho: Jogador 3. */
    snprintf(ini, sizeof(ini), "%splayer=202992822F67,03\n", dev);
    bt_ipc_init(&ipc);
    assert(bt_manager_import_ini(&ipc, ini, strlen(ini)) == 1);
    found = bt_manager_find_paired(&ipc, k_ds4_a);
    assert(found != NULL);
    assert(found->player == 3u);

    /* player=00 sozinho: nao e Jogador 1 nem 2. */
    snprintf(ini, sizeof(ini), "%splayer=202992822F67,00\n", dev);
    bt_ipc_init(&ipc);
    assert(bt_manager_import_ini(&ipc, ini, strlen(ini)) == 1);
    found = bt_manager_find_paired(&ipc, k_ds4_a);
    assert(found != NULL);
    assert(found->player == 0u);
    assert(found->player != 1u && found->player != 2u);

    /* 03 depois de 01 passa o controle para o Jogador 3. 00 nao sobrescreve. */
    snprintf(ini, sizeof(ini), "%splayer=202992822F67,01\nplayer=202992822F67,03\n", dev);
    bt_ipc_init(&ipc);
    assert(bt_manager_import_ini(&ipc, ini, strlen(ini)) == 1);
    assert(bt_manager_find_paired(&ipc, k_ds4_a)->player == 3u);

    snprintf(ini, sizeof(ini), "%splayer=202992822F67,02\nplayer=202992822F67,00\n", dev);
    bt_ipc_init(&ipc);
    assert(bt_manager_import_ini(&ipc, ini, strlen(ini)) == 1);
    assert(bt_manager_find_paired(&ipc, k_ds4_a)->player == 2u);

    printf("[PASS] PAR-05 import aceita player 1..4 e ignora 00\n");
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

/* Ate quatro enderecos. O quinto e recusado e os quatro permanecem. */
static void test_lim01_second_address_refused(void) {
    bt_ipc_state_t ipc;
    bt_ipc_init(&ipc);
    assert(bt_manager_store_single_ds4(&ipc, k_ds4_a, k_ds4_key, 0x05, 2u, "DS4-A") == 0);

    const uint8_t other_key[16] = { 0xFF, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA, 0x99, 0x88,
                                    0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x00 };
    assert(bt_manager_store_single_ds4(&ipc, k_ds4_b, other_key, 0x05, 1u, "DS4-B") >= 0);
    assert(bt_manager_find_paired(&ipc, k_ds4_b) != NULL);
    assert(bt_manager_find_paired(&ipc, k_ds4_b)->player == 1u);
    assert(bt_manager_find_paired(&ipc, k_ds4_a)->player == 2u);

    const uint8_t c[6] = { 3, 3, 3, 3, 3, 3 };
    const uint8_t d[6] = { 4, 4, 4, 4, 4, 4 };
    const uint8_t e[6] = { 5, 5, 5, 5, 5, 5 };
    assert(bt_manager_store_single_ds4(&ipc, c, other_key, 0x05, 3u, "DS4-C") >= 0);
    assert(bt_manager_store_single_ds4(&ipc, d, other_key, 0x05, 4u, "DS4-D") >= 0);
    assert(bt_manager_store_single_ds4(&ipc, e, other_key, 0x05, 1u, "DS4-E") == -3);
    assert(bt_manager_find_paired(&ipc, e) == NULL);
    assert(bt_manager_paired_count(&ipc) == 4);
    assert(bt_manager_find_paired(&ipc, k_ds4_a)->player == 2u);

    /* Regravar o mesmo endereco A continua permitido e troca com quem ocupa o jogador. */
    assert(bt_manager_store_single_ds4(&ipc, k_ds4_a, other_key, 0x05, 1u, "DS4-A") == 0);
    assert(bt_manager_find_paired(&ipc, k_ds4_a)->player == 1u);
    assert(memcmp(bt_manager_find_paired(&ipc, k_ds4_a)->link_key, other_key, 16u) == 0);
    assert(bt_manager_find_paired(&ipc, k_ds4_b)->player == 2u);
    assert(bt_manager_set_player(&ipc, k_ds4_a, 3u) >= 0);
    assert(bt_manager_find_paired(&ipc, k_ds4_a)->player == 3u);
    assert(bt_manager_find_paired(&ipc, c)->player == 1u);

    const char *two =
        "dev=202992822F67,A0A1A2A3A4A5A6A7A8A9AAABACADAEAF,05,DS4-A\n"
        "player=202992822F67,02\n"
        "dev=060504030201,FFEEDDCCBBAA99887766554433221100,05,DS4-B\n";
    bt_ipc_state_t loaded;
    bt_ipc_init(&loaded);
    assert(bt_manager_import_ini(&loaded, two, strlen(two)) == 2);
    assert(bt_manager_find_paired(&loaded, k_ds4_a) != NULL);
    assert(bt_manager_find_paired(&loaded, k_ds4_b) != NULL);

    printf("[PASS] quatro controles gravados, o quinto recusado, A permanece\n");
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

    /* Direita entra na lista; A no item 0 pareia */
    bt_ui_handle_input(&ui, &ipc, X360_BTN_DPAD_RIGHT);
    assert(ui.content_focused && ui.cursor == 0);
    bt_ui_handle_input(&ui, &ipc, X360_BTN_A);
    assert(ipc.cmd == BT_IPC_CMD_PAIR_AND_CONNECT && ipc.cmd_seq == 2);
    assert(memcmp(ipc.cmd_bd_addr, bd_a, 6u) == 0);

    /* Esquerda volta ao menu; cima sobe Buscar -> Pareados -> Controles */
    bt_ui_handle_input(&ui, &ipc, X360_BTN_DPAD_LEFT);
    assert(!ui.content_focused && ui.pane == BT_UI_PANE_SCAN);
    bt_ui_handle_input(&ui, &ipc, X360_BTN_DPAD_UP);
    assert(ui.pane == BT_UI_PANE_PAIRED);
    bt_ui_handle_input(&ui, &ipc, X360_BTN_DPAD_UP);
    assert(ui.pane == BT_UI_PANE_SLOTS);
    bt_ui_handle_input(&ui, &ipc, X360_BTN_DPAD_RIGHT);
    bt_ui_handle_input(&ui, &ipc, X360_BTN_X);
    assert(ipc.cmd == BT_IPC_CMD_DISCONNECT_SLOT && ipc.cmd_slot == 0);

    /* Press B -> exit */
    bt_ui_handle_input(&ui, &ipc, X360_BTN_B);
    assert(ui.exit_requested);

    printf("[PASS] 4-player slot manager & NXE UI navigation/IPC commands\n");
}

static void test_dpad_follows_the_screen(void) {
    bt_ipc_state_t ipc;
    bt_ipc_init(&ipc);
    bt_ui_model_t ui;
    bt_ui_model_init(&ui);

    bt_ui_handle_input(&ui, &ipc, X360_BTN_DPAD_UP);
    assert(ui.pane == BT_UI_PANE_PAIRED && !ui.content_focused);
    bt_ui_handle_input(&ui, &ipc, X360_BTN_DPAD_UP);
    assert(ui.pane == BT_UI_PANE_SLOTS);
    bt_ui_handle_input(&ui, &ipc, X360_BTN_DPAD_UP);
    assert(ui.pane == BT_UI_PANE_SLOTS);

    bt_ui_handle_input(&ui, &ipc, X360_BTN_DPAD_RIGHT);
    assert(ui.content_focused && ui.cursor == 0);
    bt_ui_handle_input(&ui, &ipc, X360_BTN_DPAD_RIGHT);
    assert(ui.cursor == 1);
    bt_ui_handle_input(&ui, &ipc, X360_BTN_DPAD_DOWN);
    assert(ui.cursor == 3);
    bt_ui_handle_input(&ui, &ipc, X360_BTN_DPAD_UP);
    assert(ui.cursor == 1);
    bt_ui_handle_input(&ui, &ipc, X360_BTN_DPAD_LEFT);
    assert(ui.cursor == 0 && ui.content_focused);
    bt_ui_handle_input(&ui, &ipc, X360_BTN_DPAD_LEFT);
    assert(!ui.content_focused && ui.pane == BT_UI_PANE_SLOTS);

    bt_ui_handle_input(&ui, &ipc, X360_BTN_DPAD_DOWN);
    assert(ui.pane == BT_UI_PANE_PAIRED);
    bt_ui_handle_input(&ui, &ipc, X360_BTN_DPAD_DOWN);
    assert(ui.pane == BT_UI_PANE_SCAN);
    bt_ui_handle_input(&ui, &ipc, X360_BTN_DPAD_DOWN);
    assert(ui.pane == BT_UI_PANE_SCAN);

    printf("[PASS] direcional segue o menu vertical e a grade 2x2\n");
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

/* [Tarefa 07] Sem DS4 gravado, A em "Buscar" pede o primeiro pareamento (sem endereco). */
static void test_first_pairing_requests_pair_and_connect(void) {
    static const uint8_t zero[6] = { 0 };
    bt_ipc_state_t ipc;
    bt_ipc_init(&ipc);
    assert(!bt_manager_has_stored_ds4(&ipc));

    bt_ui_model_t ui;
    bt_ui_model_init(&ui);
    assert(ui.pane == BT_UI_PANE_SCAN && !ui.content_focused);

    /* Menu focado: A na aba Buscar nao envia comando e nao mexe na navegacao. */
    bt_ui_handle_input(&ui, &ipc, X360_BTN_A);
    assert(ipc.cmd_seq == 0u && ipc.cmd == BT_IPC_CMD_NONE);
    assert(!ui.content_focused && ui.pane == BT_UI_PANE_SCAN);

    /* Painel focado (lista vazia, sem DS4 gravado): A pede o primeiro pareamento. */
    bt_ui_handle_input(&ui, &ipc, X360_BTN_DPAD_RIGHT);
    assert(ui.content_focused);
    bt_ui_handle_input(&ui, &ipc, X360_BTN_A);
    assert(ipc.cmd_seq == 1u);
    assert(ipc.cmd == BT_IPC_CMD_PAIR_AND_CONNECT);
    assert(memcmp(ipc.cmd_bd_addr, zero, 6u) == 0);
    assert(ui.content_focused);

    /* Voltando ao menu (esquerda), A volta a nao enviar nada. */
    bt_ui_handle_input(&ui, &ipc, X360_BTN_DPAD_LEFT);
    assert(!ui.content_focused);
    bt_ui_handle_input(&ui, &ipc, X360_BTN_A);
    assert(ipc.cmd_seq == 1u);

    /* Nas outras abas com o menu focado, A e X nao enviam nada. */
    bt_ui_handle_input(&ui, &ipc, X360_BTN_DPAD_UP); /* Pareados */
    bt_ui_handle_input(&ui, &ipc, X360_BTN_A);
    bt_ui_handle_input(&ui, &ipc, X360_BTN_X);
    assert(ipc.cmd_seq == 1u);
    bt_ui_handle_input(&ui, &ipc, X360_BTN_DPAD_UP); /* Controles */
    assert(bt_manager_assign_slot(&ipc, k_ds4_a, 0x0001u, "DS4-A") == 0);
    bt_ui_handle_input(&ui, &ipc, X360_BTN_A);
    bt_ui_handle_input(&ui, &ipc, X360_BTN_X);
    assert(ipc.cmd_seq == 1u && !ui.content_focused);

    printf("[PASS] tarefa 07: sem DS4 gravado, A pede PAIR_AND_CONNECT\n");
}

/* [Tarefa 07] Com DS4 gravado, nenhuma acao oferece novo pareamento. */
static void test_stored_ds4_does_not_offer_pairing(void) {
    bt_ipc_state_t ipc;
    bt_ipc_init(&ipc);
    assert(bt_manager_store_single_ds4(&ipc, k_ds4_a, k_ds4_key, 0x05, 1u, "DS4-A") == 0);
    assert(bt_manager_has_stored_ds4(&ipc));

    bt_ui_model_t ui;
    bt_ui_model_init(&ui);

    /* Aba Buscar: A com menu focado e com painel focado, com e sem item na lista. */
    bt_ui_handle_input(&ui, &ipc, X360_BTN_A);
    assert(ipc.cmd_seq == 0u && ipc.cmd == BT_IPC_CMD_NONE);
    bt_ui_handle_input(&ui, &ipc, X360_BTN_DPAD_RIGHT);
    assert(ui.content_focused);
    bt_ui_handle_input(&ui, &ipc, X360_BTN_A);
    assert(ipc.cmd_seq == 1u && ipc.cmd == BT_IPC_CMD_PAIR_AND_CONNECT);

    ipc.scan_count = 1;
    memcpy(ipc.scan_items[0].bd_addr, k_ds4_b, 6u);
    snprintf(ipc.scan_items[0].name, sizeof(ipc.scan_items[0].name), "Wireless Controller");
    bt_ui_handle_input(&ui, &ipc, X360_BTN_A);
    assert(ui.content_focused && ui.cursor == 0);
    assert(ipc.cmd_seq == 2u && ipc.cmd == BT_IPC_CMD_PAIR_AND_CONNECT);

    /* Gravar o DS4 depois de um pedido anterior tambem fecha a porta. */
    bt_ipc_state_t later;
    bt_ipc_init(&later);
    bt_ui_model_init(&ui);
    bt_ui_handle_input(&ui, &later, X360_BTN_DPAD_RIGHT);
    bt_ui_handle_input(&ui, &later, X360_BTN_A);
    assert(later.cmd_seq == 1u && later.cmd == BT_IPC_CMD_PAIR_AND_CONNECT);
    assert(bt_manager_store_single_ds4(&later, k_ds4_a, k_ds4_key, 0x05, 2u, "DS4-A") == 0);
    bt_ui_handle_input(&ui, &later, X360_BTN_A);
    assert(later.cmd_seq == 2u && later.cmd == BT_IPC_CMD_PAIR_AND_CONNECT);
    const uint8_t c[6] = { 3, 3, 3, 3, 3, 3 };
    const uint8_t d[6] = { 4, 4, 4, 4, 4, 4 };
    const uint8_t e[6] = { 5, 5, 5, 5, 5, 5 };
    assert(bt_manager_store_single_ds4(&later, c, k_ds4_key, 0x05, 1u, "C") >= 0);
    assert(bt_manager_store_single_ds4(&later, d, k_ds4_key, 0x05, 3u, "D") >= 0);
    assert(bt_manager_store_single_ds4(&later, e, k_ds4_key, 0x05, 4u, "E") >= 0);
    bt_ui_handle_input(&ui, &later, X360_BTN_A);
    assert(later.cmd_seq == 2u);

    /* Reconectar o DS4 gravado (aba Pareados) continua disponivel. */
    bt_ui_model_init(&ui);
    bt_ui_handle_input(&ui, &ipc, X360_BTN_DPAD_UP);
    bt_ui_handle_input(&ui, &ipc, X360_BTN_DPAD_RIGHT);
    bt_ui_handle_input(&ui, &ipc, X360_BTN_A);
    assert(ipc.cmd_seq == 3u && ipc.cmd == BT_IPC_CMD_CONNECT_PAIRED);

    printf("[PASS] tarefa 07: pareia ate quatro; o quinto nao oferece pareamento\n");
}

/* [Tarefa 07] phrase_id do reply vira o texto exato da tabela da tarefa 01. */
static void test_reply_phrase_becomes_exact_text(void) {
    static const struct {
        uint32_t    id;
        const char *text;
    } k_cases[] = {
        { BT_PHRASE_DS4_PAIRED_PLAYER2, "DualShock 4 pareado no Jogador 2" },
        { BT_PHRASE_DS4_PAIRED_PLAYER1, "DualShock 4 pareado no Jogador 1" },
        { BT_PHRASE_DS4_PAIR_FAILED,    "Falha ao parear o DualShock 4." },
        { BT_PHRASE_DS4_ALREADY_PAIRED, "Já existe um DualShock 4 pareado." },
    };
    for (size_t i = 0; i < sizeof(k_cases) / sizeof(k_cases[0]); i++) {
        bt_ipc_state_t ipc;
        bt_ipc_init(&ipc);
        assert(bt_ui_apply_reply_phrase(&ipc, k_cases[i].id));
        assert(strcmp(ipc.status_msg, k_cases[i].text) == 0);
        /* Mesmo texto da tabela da tarefa 01: nada reescrito na UI. */
        assert(strcmp(ipc.status_msg, bt_phase1_phrase_text(k_cases[i].id)) == 0);
    }

    /* Sem frase de pareamento, a mensagem anterior nao e tocada. */
    bt_ipc_state_t ipc;
    bt_ipc_init(&ipc);
    snprintf(ipc.status_msg, sizeof(ipc.status_msg), "mensagem anterior");
    assert(!bt_ui_apply_reply_phrase(&ipc, BT_PHRASE_NONE));
    assert(strcmp(ipc.status_msg, "mensagem anterior") == 0);
    assert(!bt_ui_apply_reply_phrase(&ipc, 0xFFFFu));
    assert(strcmp(ipc.status_msg, "mensagem anterior") == 0);
    assert(!bt_ui_apply_reply_phrase(NULL, BT_PHRASE_DS4_PAIR_FAILED));

    assert(bt_ui_apply_reply_phrase(&ipc, BT_PHRASE_DS4_DISCONNECTED));
    assert(strcmp(ipc.status_msg, "DualShock 4 desconectado.") == 0);

    printf("[PASS] tarefa 07: phrase_id do reply vira o texto exato da tabela\n");
}

/* [Tarefa 07] Frase de adaptador no reply nunca substitui o status ja mostrado. */
static void test_adapter_phrase_keeps_status(void) {
    static const uint32_t k_adapter[] = {
        BT_PHRASE_ADAPTER_READY, BT_PHRASE_ADAPTER_NOT_FOUND,
        BT_PHRASE_ADAPTER_WRONG, BT_PHRASE_ADAPTER_DISCONNECTED,
    };
    for (size_t i = 0; i < sizeof(k_adapter) / sizeof(k_adapter[0]); i++) {
        bt_ipc_state_t ipc;
        bt_ipc_init(&ipc);
        snprintf(ipc.status_msg, sizeof(ipc.status_msg), "texto anterior");
        assert(bt_phase1_phrase_text(k_adapter[i]) != NULL); /* a frase existe na tabela */
        assert(!bt_ui_apply_reply_phrase(&ipc, k_adapter[i]));
        assert(strcmp(ipc.status_msg, "texto anterior") == 0);
    }

    printf("[PASS] tarefa 07: frase de adaptador nao substitui o status\n");
}

static void test_scan_list_reaches_the_screen(void) {
    hci_inquiry_item_t items[2];
    memset(items, 0, sizeof items);
    memcpy(items[0].bd_addr, "\x11\x22\x33\x44\x55\x66", 6);
    items[0].class_of_device = 0x002508u;
    memcpy(items[0].name, "Wireless Controller", 20);
    memcpy(items[1].bd_addr, "\xAA\xBB\xCC\xDD\xEE\xFF", 6);
    memcpy(items[1].name, "Fone", 4);
    uint8_t buf[BT_SCAN_FILE_BYTES];
    assert(bt_scan_pack(buf, sizeof buf, items, 2, true) == BT_SCAN_FILE_BYTES);
    bt_ipc_state_t ipc;
    memset(&ipc, 0, sizeof ipc);
    assert(bt_scan_apply(&ipc, buf, sizeof buf));
    assert(ipc.scan_active);
    assert(ipc.scan_count == 2);
    assert(memcmp(ipc.scan_items[0].bd_addr, items[0].bd_addr, 6) == 0);
    assert(ipc.scan_items[0].class_of_device == 0x002508u);
    assert(strcmp(ipc.scan_items[0].name, "Wireless Controller") == 0);
    assert(strcmp(ipc.scan_items[1].name, "Fone") == 0);
    printf("[PASS] busca: a lista encontrada chega na tela\n");
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
    test_dpad_follows_the_screen();
    test_interactive_button_remap();
    test_first_pairing_requests_pair_and_connect();
    test_stored_ds4_does_not_offer_pairing();
    test_reply_phrase_becomes_exact_text();
    test_adapter_phrase_keeps_status();
    test_scan_list_reaches_the_screen();
    return 0;
}
