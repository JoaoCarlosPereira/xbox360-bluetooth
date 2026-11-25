#include "../manager/bt_phase1.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_adp_02_no_ugreen(void)
{
    const bt_phase1_context_t ctx = {
        .event   = BT_PHASE1_EV_ADAPTER,
        .adapter = BT_PHASE1_ADAPTER_NONE,
    };
    bt_phase1_decision_t d = bt_phase1_decide(&ctx);
    assert(d.phrase_id == BT_PHRASE_ADAPTER_NOT_FOUND);
    assert(strcmp(bt_phase1_phrase_text(d.phrase_id), "Adaptador UGREEN não encontrado.") == 0);
    printf("[PASS] ADP-02 sem UGREEN\n");
}

static void test_adp_03_wrong_adapter(void)
{
    const bt_phase1_context_t ctx = {
        .event   = BT_PHASE1_EV_ADAPTER,
        .adapter = BT_PHASE1_ADAPTER_WRONG,
    };
    bt_phase1_decision_t d = bt_phase1_decide(&ctx);
    assert(d.phrase_id == BT_PHRASE_ADAPTER_WRONG);
    assert(strcmp(bt_phase1_phrase_text(d.phrase_id),
                 "Este adaptador não faz parte desta entrega.") == 0);
    printf("[PASS] ADP-03 adaptador errado\n");
}

static void test_sta_01_adapter_state_phrases(void)
{
    const bt_phase1_context_t ready_ctx = {
        .event   = BT_PHASE1_EV_ADAPTER,
        .adapter = BT_PHASE1_ADAPTER_READY,
    };
    bt_phase1_decision_t ready = bt_phase1_decide(&ready_ctx);
    assert(ready.phrase_id == BT_PHRASE_ADAPTER_READY);
    assert(strcmp(bt_phase1_phrase_text(ready.phrase_id), "Adaptador UGREEN pronto") == 0);

    assert(strcmp(bt_phase1_phrase_text(BT_PHRASE_ADAPTER_NOT_FOUND),
                 "Adaptador UGREEN não encontrado.") == 0);
    assert(strcmp(bt_phase1_phrase_text(BT_PHRASE_ADAPTER_DISCONNECTED),
                 "Adaptador UGREEN desconectado.") == 0);

    assert(bt_phase1_is_adapter_state_phrase(BT_PHRASE_ADAPTER_READY));
    assert(bt_phase1_is_adapter_state_phrase(BT_PHRASE_ADAPTER_NOT_FOUND));
    assert(bt_phase1_is_adapter_state_phrase(BT_PHRASE_ADAPTER_DISCONNECTED));
    assert(!bt_phase1_is_adapter_state_phrase(BT_PHRASE_ADAPTER_WRONG));
    assert(!bt_phase1_is_adapter_state_phrase(BT_PHRASE_DS4_ALREADY_PAIRED));
    assert(!bt_phase1_is_adapter_state_phrase(BT_PHRASE_DS4_PAIR_FAILED));
    assert(!bt_phase1_is_adapter_state_phrase(BT_PHRASE_DS4_DISCONNECTED));
    assert(!bt_phase1_is_adapter_state_phrase(BT_PHRASE_NONE));
    printf("[PASS] STA-01 frases de estado do adaptador\n");
}

static void test_par_01_official_player2(void)
{
    const bt_phase1_context_t ctx = {
        .event                  = BT_PHASE1_EV_PAIR_ATTEMPT,
        .official_on_player1    = true,
        .ds4_paired             = false,
    };
    bt_phase1_decision_t d = bt_phase1_decide(&ctx);
    assert(d.accept_pair);
    assert(d.player == 2u);
    assert(d.phrase_id == BT_PHRASE_DS4_PAIRED_PLAYER2);
    assert(strcmp(bt_phase1_phrase_text(d.phrase_id), "DualShock 4 pareado no Jogador 2") == 0);
    printf("[PASS] PAR-01 oficial no Jogador 1\n");
}

static void test_par_02_no_official_player1(void)
{
    const bt_phase1_context_t ctx = {
        .event                  = BT_PHASE1_EV_PAIR_ATTEMPT,
        .official_on_player1    = false,
        .ds4_paired             = false,
    };
    bt_phase1_decision_t d = bt_phase1_decide(&ctx);
    assert(d.accept_pair);
    assert(d.player == 1u);
    assert(d.phrase_id == BT_PHRASE_DS4_PAIRED_PLAYER1);
    assert(strcmp(bt_phase1_phrase_text(d.phrase_id), "DualShock 4 pareado no Jogador 1") == 0);
    printf("[PASS] PAR-02 sem oficial\n");
}

static void test_par_03_second_ds4_phrase(void)
{
    const uint8_t stored[6] = { 0xAA, 0x22, 0x33, 0x44, 0x55, 0xFF };
    const uint8_t other[6]  = { 0xBB, 0x22, 0x33, 0x44, 0x55, 0xEE };
    const bt_phase1_context_t ctx = {
        .event              = BT_PHASE1_EV_PAIR_ATTEMPT,
        .ds4_paired         = true,
        .stored_bd_addr     = stored,
        .candidate_bd_addr  = other,
    };
    bt_phase1_decision_t d = bt_phase1_decide(&ctx);
    assert(d.phrase_id == BT_PHRASE_DS4_ALREADY_PAIRED);
    assert(d.player == 0u);
    assert(!d.accept_pair);
    assert(strcmp(bt_phase1_phrase_text(d.phrase_id), "Já existe um DualShock 4 pareado.") == 0);
    printf("[PASS] PAR-03 segundo DualShock 4\n");
}

static void test_par_04_second_ds4_no_slot(void)
{
    const uint8_t stored[6] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 };
    const uint8_t other[6]  = { 0x10, 0x20, 0x30, 0x40, 0x50, 0x60 };
    const bt_phase1_context_t ctx = {
        .event              = BT_PHASE1_EV_PAIR_ATTEMPT,
        .ds4_paired         = true,
        .stored_bd_addr     = stored,
        .candidate_bd_addr  = other,
    };
    bt_phase1_decision_t d = bt_phase1_decide(&ctx);
    assert(d.player == 0u);
    assert(!d.accept_pair);
    printf("[PASS] PAR-04 recusa segundo DS4\n");
}

static void test_par_06_pair_timeout(void)
{
    const bt_phase1_context_t before = {
        .event            = BT_PHASE1_EV_PAIR_TICK,
        .pair_elapsed_sec = 59u,
    };
    bt_phase1_decision_t d59 = bt_phase1_decide(&before);
    assert(d59.phrase_id == BT_PHRASE_NONE);
    assert(!d59.accept_pair);
    assert(d59.player == 0u);

    const bt_phase1_context_t at60 = {
        .event            = BT_PHASE1_EV_PAIR_TICK,
        .pair_elapsed_sec = 60u,
    };
    bt_phase1_decision_t d60 = bt_phase1_decide(&at60);
    assert(d60.phrase_id == BT_PHRASE_DS4_PAIR_FAILED);
    assert(!d60.accept_pair);
    assert(d60.player == 0u);
    assert(strcmp(bt_phase1_phrase_text(d60.phrase_id), "Falha ao parear o DualShock 4.") == 0);

    const bt_phase1_context_t after = {
        .event            = BT_PHASE1_EV_PAIR_TICK,
        .pair_elapsed_sec = 61u,
    };
    bt_phase1_decision_t d61 = bt_phase1_decide(&after);
    assert(d61.phrase_id == BT_PHRASE_DS4_PAIR_FAILED);
    assert(!d61.accept_pair);
    assert(d61.player == 0u);
    assert(strcmp(bt_phase1_phrase_text(d61.phrase_id), "Falha ao parear o DualShock 4.") == 0);
    printf("[PASS] PAR-06 timeout 60 segundos\n");
}

static void test_lim_01_rejects_second_address(void)
{
    const uint8_t stored[6] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01 };
    const uint8_t other[6]  = { 0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x02 };
    const bt_phase1_context_t ctx = {
        .event              = BT_PHASE1_EV_PAIR_ATTEMPT,
        .ds4_paired         = true,
        .stored_bd_addr     = stored,
        .candidate_bd_addr  = other,
    };
    bt_phase1_decision_t d = bt_phase1_decide(&ctx);
    assert(!d.accept_pair);
    assert(d.player == 0u);
    assert(d.phrase_id == BT_PHRASE_DS4_ALREADY_PAIRED);

    const bt_phase1_context_t null_candidate = {
        .event              = BT_PHASE1_EV_PAIR_ATTEMPT,
        .ds4_paired         = true,
        .stored_bd_addr     = stored,
        .candidate_bd_addr  = NULL,
    };
    bt_phase1_decision_t d_null = bt_phase1_decide(&null_candidate);
    assert(!d_null.accept_pair);
    assert(d_null.player == 0u);
    assert(d_null.phrase_id == BT_PHRASE_DS4_ALREADY_PAIRED);

    const bt_phase1_context_t same_addr = {
        .event              = BT_PHASE1_EV_PAIR_ATTEMPT,
        .ds4_paired         = true,
        .stored_bd_addr     = stored,
        .candidate_bd_addr  = stored,
    };
    bt_phase1_decision_t d_same = bt_phase1_decide(&same_addr);
    assert(d_same.phrase_id != BT_PHRASE_DS4_ALREADY_PAIRED);
    assert(d_same.phrase_id == BT_PHRASE_NONE);
    assert(d_same.player == 0u);
    assert(!d_same.accept_pair);

    const uint8_t candidate_only[6] = { 0xCA, 0xFE, 0xBA, 0xBE, 0x00, 0x02 };
    const bt_phase1_context_t null_stored = {
        .event              = BT_PHASE1_EV_PAIR_ATTEMPT,
        .ds4_paired         = true,
        .stored_bd_addr     = NULL,
        .candidate_bd_addr  = candidate_only,
    };
    bt_phase1_decision_t d_null_stored = bt_phase1_decide(&null_stored);
    assert(d_null_stored.phrase_id == BT_PHRASE_DS4_ALREADY_PAIRED);
    assert(strcmp(bt_phase1_phrase_text(d_null_stored.phrase_id),
                 "Já existe um DualShock 4 pareado.") == 0);
    assert(d_null_stored.player == 0u);
    assert(!d_null_stored.accept_pair);
    printf("[PASS] LIM-01 um único endereço DS4\n");
}

static void test_est_01_unpaired_game_input(void)
{
    const bt_phase1_context_t pair_ctx = {
        .event               = BT_PHASE1_EV_PAIR_ATTEMPT,
        .official_on_player1 = true,
        .ds4_paired          = false,
    };
    bt_phase1_decision_t would_pair = bt_phase1_decide(&pair_ctx);
    assert(would_pair.accept_pair);

    const bt_phase1_context_t game_ctx = {
        .event               = BT_PHASE1_EV_GAME_INPUT,
        .official_on_player1 = true,
        .ds4_paired          = false,
    };
    bt_phase1_decision_t d = bt_phase1_decide(&game_ctx);
    assert(!d.accept_pair);
    assert(d.player == 0u);
    assert(d.phrase_id == BT_PHRASE_NONE);

    const bt_phase1_context_t paired_game = {
        .event      = BT_PHASE1_EV_GAME_INPUT,
        .ds4_paired = true,
    };
    bt_phase1_decision_t d_paired = bt_phase1_decide(&paired_game);
    assert(d_paired.phrase_id == BT_PHRASE_NONE);
    assert(d_paired.phrase_id != BT_PHRASE_DS4_PAIR_FAILED);
    assert(d_paired.phrase_id != BT_PHRASE_DS4_ALREADY_PAIRED);
    assert(d_paired.player == 0u);
    printf("[PASS] EST-01 entrada sem pareamento\n");
}

static void test_err_01_adapter_removed(void)
{
    const bt_phase1_context_t ctx = {
        .event   = BT_PHASE1_EV_ADAPTER,
        .adapter = BT_PHASE1_ADAPTER_REMOVED,
    };
    bt_phase1_decision_t d = bt_phase1_decide(&ctx);
    assert(d.phrase_id == BT_PHRASE_ADAPTER_DISCONNECTED);
    assert(strcmp(bt_phase1_phrase_text(d.phrase_id), "Adaptador UGREEN desconectado.") == 0);
    printf("[PASS] ERR-01 UGREEN desconectado\n");
}

static void test_err_03_ds4_disconnect(void)
{
    const bt_phase1_context_t ctx = {
        .event = BT_PHASE1_EV_DS4_DISCONNECT,
    };
    bt_phase1_decision_t d = bt_phase1_decide(&ctx);
    assert(d.phrase_id == BT_PHRASE_DS4_DISCONNECTED);
    assert(d.player == 0u);
    assert(!d.accept_pair);
    assert(strcmp(bt_phase1_phrase_text(d.phrase_id), "DualShock 4 desconectado.") == 0);
    printf("[PASS] ERR-03 DS4 desconectado\n");
}

static void test_phrase_bounds_and_null_decide(void)
{
    assert(bt_phase1_phrase_text(BT_PHRASE_NONE) == NULL);
    assert(bt_phase1_phrase_text((uint32_t)BT_PHRASE_DS4_DISCONNECTED + 1u) == NULL);
    assert(bt_phase1_phrase_text(0xFFFFFFFFu) == NULL);

    bt_phase1_decision_t d = bt_phase1_decide(NULL);
    assert(d.phrase_id == BT_PHRASE_NONE);
    assert(d.player == 0u);
    assert(!d.accept_pair);
    printf("[PASS] limites de phrase_id e decide(NULL)\n");
}

int main(void)
{
    test_adp_02_no_ugreen();
    test_adp_03_wrong_adapter();
    test_sta_01_adapter_state_phrases();
    test_par_01_official_player2();
    test_par_02_no_official_player1();
    test_par_03_second_ds4_phrase();
    test_par_04_second_ds4_no_slot();
    test_par_06_pair_timeout();
    test_lim_01_rejects_second_address();
    test_est_01_unpaired_game_input();
    test_err_01_adapter_removed();
    test_err_03_ds4_disconnect();
    test_phrase_bounds_and_null_decide();
    return 0;
}