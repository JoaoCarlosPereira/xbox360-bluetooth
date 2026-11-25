#include "bt_phase1.h"

#include <string.h>

static const char *const k_phrases[] = {
    [BT_PHRASE_NONE]                   = NULL,
    [BT_PHRASE_ADAPTER_READY]          = "Adaptador UGREEN pronto",
    [BT_PHRASE_ADAPTER_NOT_FOUND]      = "Adaptador UGREEN não encontrado.",
    [BT_PHRASE_ADAPTER_WRONG]          = "Este adaptador não faz parte desta entrega.",
    [BT_PHRASE_ADAPTER_DISCONNECTED]   = "Adaptador UGREEN desconectado.",
    [BT_PHRASE_DS4_PAIRED_PLAYER1]     = "DualShock 4 pareado no Jogador 1",
    [BT_PHRASE_DS4_PAIRED_PLAYER2]     = "DualShock 4 pareado no Jogador 2",
    [BT_PHRASE_DS4_ALREADY_PAIRED]     = "Já existe um DualShock 4 pareado.",
    [BT_PHRASE_DS4_PAIR_FAILED]        = "Falha ao parear o DualShock 4.",
    [BT_PHRASE_DS4_DISCONNECTED]       = "DualShock 4 desconectado.",
};

static bt_phase1_decision_t decision_no_phrase(void)
{
    bt_phase1_decision_t out = { 0u, 0u, false };
    return out;
}

static bool reject_second_ds4_pair(const bt_phase1_context_t *ctx)
{
    if (!ctx->ds4_paired)
        return false;
    if (ctx->candidate_bd_addr == NULL || ctx->stored_bd_addr == NULL)
        return true;
    return memcmp(ctx->candidate_bd_addr, ctx->stored_bd_addr, 6u) != 0;
}

bt_phase1_decision_t bt_phase1_decide(const bt_phase1_context_t *ctx)
{
    bt_phase1_decision_t out = decision_no_phrase();

    if (ctx == NULL)
        return out;

    switch (ctx->event) {
    case BT_PHASE1_EV_ADAPTER:
        switch (ctx->adapter) {
        case BT_PHASE1_ADAPTER_NONE:
            out.phrase_id = BT_PHRASE_ADAPTER_NOT_FOUND;
            break;
        case BT_PHASE1_ADAPTER_WRONG:
            out.phrase_id = BT_PHRASE_ADAPTER_WRONG;
            break;
        case BT_PHASE1_ADAPTER_READY:
            out.phrase_id = BT_PHRASE_ADAPTER_READY;
            break;
        case BT_PHASE1_ADAPTER_REMOVED:
            out.phrase_id = BT_PHRASE_ADAPTER_DISCONNECTED;
            break;
        }
        break;

    case BT_PHASE1_EV_PAIR_ATTEMPT:
        if (reject_second_ds4_pair(ctx)) {
            out.phrase_id = BT_PHRASE_DS4_ALREADY_PAIRED;
            out.player = 0u;
            out.accept_pair = false;
            break;
        }
        if (ctx->ds4_paired)
            break;
        out.accept_pair = true;
        if (ctx->official_on_player1) {
            out.player = 2u;
            out.phrase_id = BT_PHRASE_DS4_PAIRED_PLAYER2;
        } else {
            out.player = 1u;
            out.phrase_id = BT_PHRASE_DS4_PAIRED_PLAYER1;
        }
        break;

    case BT_PHASE1_EV_PAIR_TICK:
        if (ctx->pair_elapsed_sec >= BT_PHASE1_PAIR_TIMEOUT_SEC) {
            out.phrase_id = BT_PHRASE_DS4_PAIR_FAILED;
            out.accept_pair = false;
            out.player = 0u;
        }
        break;

    case BT_PHASE1_EV_DS4_DISCONNECT:
        out.phrase_id = BT_PHRASE_DS4_DISCONNECTED;
        out.player = 0u;
        out.accept_pair = false;
        break;

    case BT_PHASE1_EV_GAME_INPUT:
        out.player = 0u;
        out.accept_pair = false;
        break;
    }

    return out;
}

const char *bt_phase1_phrase_text(uint32_t phrase_id)
{
    if (phrase_id >= (sizeof(k_phrases) / sizeof(k_phrases[0])))
        return NULL;
    return k_phrases[phrase_id];
}

bool bt_phase1_is_adapter_state_phrase(uint32_t phrase_id)
{
    return phrase_id == BT_PHRASE_ADAPTER_READY ||
           phrase_id == BT_PHRASE_ADAPTER_NOT_FOUND ||
           phrase_id == BT_PHRASE_ADAPTER_DISCONNECTED;
}