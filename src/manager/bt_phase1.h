#ifndef BT_PHASE1_H
#define BT_PHASE1_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t phrase_id;
    uint8_t  player;
    bool     accept_pair;
} bt_phase1_decision_t;

typedef enum {
    BT_PHRASE_NONE = 0,
    BT_PHRASE_ADAPTER_READY,
    BT_PHRASE_ADAPTER_NOT_FOUND,
    BT_PHRASE_ADAPTER_WRONG,
    BT_PHRASE_ADAPTER_DISCONNECTED,
    BT_PHRASE_DS4_PAIRED_PLAYER1,
    BT_PHRASE_DS4_PAIRED_PLAYER2,
    BT_PHRASE_DS4_ALREADY_PAIRED,
    BT_PHRASE_DS4_PAIR_FAILED,
    BT_PHRASE_DS4_DISCONNECTED,
} bt_phase1_phrase_id_t;

typedef enum {
    BT_PHASE1_ADAPTER_NONE = 0,
    BT_PHASE1_ADAPTER_WRONG,
    BT_PHASE1_ADAPTER_READY,
    BT_PHASE1_ADAPTER_REMOVED,
} bt_phase1_adapter_state_t;

typedef enum {
    BT_PHASE1_EV_ADAPTER = 0,
    BT_PHASE1_EV_PAIR_ATTEMPT,
    BT_PHASE1_EV_PAIR_TICK,
    BT_PHASE1_EV_DS4_DISCONNECT,
    BT_PHASE1_EV_GAME_INPUT,
} bt_phase1_event_t;

typedef struct {
    bt_phase1_event_t          event;
    bt_phase1_adapter_state_t  adapter;
    bool                       official_on_player1;
    bool                       ds4_paired;
    uint32_t                   pair_elapsed_sec;
    const uint8_t             *candidate_bd_addr;
    const uint8_t             *stored_bd_addr;
} bt_phase1_context_t;

#define BT_PHASE1_PAIR_TIMEOUT_SEC 60u

bt_phase1_decision_t bt_phase1_decide(const bt_phase1_context_t *ctx);
const char          *bt_phase1_phrase_text(uint32_t phrase_id);
bool                 bt_phase1_is_adapter_state_phrase(uint32_t phrase_id);

#ifdef __cplusplus
}
#endif

#endif /* BT_PHASE1_H */