#ifndef XBOX360_BT_MANAGER_H
#define XBOX360_BT_MANAGER_H

#include "../hci/hci_core.h"
#include "../hid/ds4_bt.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BT_IPC_MAGIC             0x42543336u /* "BT36" */
#define BT_IPC_VERSION           1u
#define BT_MAX_ACTIVE_SLOTS      4u
#define BT_MAX_ACTIVE_BT_LINKS   2u
#define BT_MAX_PAIRED_DEVICES    8u
#define BT_MAX_SCAN_ITEMS        16u
#define BT_SCAN_FILE_MAGIC       0x42544331u /* "BTC1" */
#define BT_SCAN_FILE_BYTES       (16u + BT_MAX_SCAN_ITEMS * 44u)

typedef enum {
    BT_IPC_CMD_NONE = 0,
    BT_IPC_CMD_START_SCAN,
    BT_IPC_CMD_STOP_SCAN,
    BT_IPC_CMD_PAIR_AND_CONNECT,
    BT_IPC_CMD_CONNECT_PAIRED,
    BT_IPC_CMD_DISCONNECT_SLOT,
    BT_IPC_CMD_FORGET_PAIRED,
    BT_IPC_CMD_SET_PLAYER
} bt_ipc_cmd_t;

typedef struct {
    bool     valid;
    uint8_t  bd_addr[6];
    uint8_t  link_key[16];
    uint8_t  key_type;
    uint8_t  player;          /* 1 ou 2 quando atribuido; 0 = sem jogador */
    uint32_t class_of_device;
    char     name[32];
} bt_paired_device_t;

typedef struct {
    bool            connected;
    uint8_t         bd_addr[6];
    uint16_t        acl_handle;
    uint8_t         battery_pct;
    char            name[32];
    bt_button_map_t map;
} bt_player_slot_t;

typedef struct {
    bool     active;
    uint32_t slot_idx;
    uint8_t  step;          /* 0 .. BT_X360_MAP_SLOTS - 1 */
    uint32_t prev_raw_mask;
    uint32_t last_raw_mask; /* Latest raw buttons from target BT controller */
} bt_remap_wizard_t;

typedef struct {
    uint32_t           magic;
    uint32_t           version;

#define BT_LINK_DOWN        0u
#define BT_LINK_CONNECTING  1u
#define BT_LINK_UP          2u

    /* Dongle status */
    bool               dongle_ready;
    uint8_t            pad_link; /* BT_LINK_*: algum link aberto, para o texto geral */
    uint8_t            live_player;     /* primeiro jogador com link aberto; 0 se nenhum */
    uint8_t            player_link[4];  /* BT_LINK_* do jogador 1..4 */
    uint8_t            identify_player; /* 1..4 enquanto SHARE esta preso nesse controle */
    uint16_t           dongle_vid;
    uint16_t           dongle_pid;
    uint8_t            hci_version;
    uint8_t            dongle_bd_addr[6];

    /* Active player slots (0..3 -> Xbox 360 Ring of Light 1..4) */
    bt_player_slot_t   slots[BT_MAX_ACTIVE_SLOTS];

    /* Latest raw physical button mask per slot (used by Remap Wizard) */
    uint32_t           slot_raw_mask[BT_MAX_ACTIVE_SLOTS];

    /* Paired devices (persisted with Link Keys) */
    bt_paired_device_t paired[BT_MAX_PAIRED_DEVICES];

    /* Scan / Inquiry state */
    bool               scan_active;
    bool               scan_finished_empty; /* busca terminou e nenhum aparelho chegou */
    uint32_t           scan_count;
    hci_inquiry_item_t scan_items[BT_MAX_SCAN_ITEMS];

    /* UI -> Daemon command mailbox */
    volatile uint32_t  cmd_seq;
    volatile uint32_t  ack_seq;
    uint32_t           cmd;
    uint8_t            cmd_bd_addr[6];
    uint32_t           cmd_slot;
    char               status_msg[64];
} bt_ipc_state_t;

/* UI focus columns & navigation model (shared between Xbox UI and Linux tests) */
typedef enum {
    BT_UI_PANE_SLOTS = 0,
    BT_UI_PANE_PAIRED,
    BT_UI_PANE_SCAN
} bt_ui_pane_t;

typedef struct {
    bt_ui_pane_t pane;
    int          cursor;
    int          cursor_by_pane[3]; /* memoria de foco por aba */
    bool         content_focused; /* false: menu vertical; true: painel da direita */
    bool         exit_requested;
    bool         identify_mode; /* LB: SHARE no controle acende o card do jogador */
} bt_ui_model_t;

void bt_ipc_init(bt_ipc_state_t *ipc);

/* Paired device / Link Key management */
int bt_manager_upsert_paired(bt_ipc_state_t *ipc, const uint8_t bd_addr[6],
                             const uint8_t link_key[16], uint8_t key_type,
                             uint32_t cod, const char *name);
const bt_paired_device_t *bt_manager_find_paired(const bt_ipc_state_t *ipc,
                                                 const uint8_t bd_addr[6]);
int bt_manager_forget_paired(bt_ipc_state_t *ipc, const uint8_t bd_addr[6]);

/* Ate quatro DualShock 4, jogadores 1..4.
 * Retorna indice >= 0 em sucesso; -1 argumento invalido (player fora de 1..4);
 * -3 se ja existem quatro enderecos (os registros existentes ficam). */
int bt_manager_store_single_ds4(bt_ipc_state_t *ipc, const uint8_t bd_addr[6],
                                const uint8_t link_key[16], uint8_t key_type,
                                uint8_t player, const char *name);

/* Quantos enderecos validos estao gravados. */
int bt_manager_paired_count(const bt_ipc_state_t *ipc);
/* Quantos links Bluetooth estao ativos (conectados ou conectando), maximo 2. */
int bt_manager_active_bt_count(const bt_ipc_state_t *ipc);

/* Move o endereco para o jogador 1..4. Se outro controle ocupa esse jogador, os dois trocam. */
int bt_manager_set_player(bt_ipc_state_t *ipc, const uint8_t bd_addr[6], uint8_t player);

/* O XAM entrega o usuário livre. O jogador é esse usuário mais um.
 * Usuário 0 com o controle oficial ligado, usuário repetido ou fora de 0..3 devolve 0. */
uint8_t bt_player_for_bound_user(bool official_on_player1, uint8_t xam_user, uint8_t other_user);

/* Dois links no reply de 32 bytes: jogador no nibble, estado nos 2 bits. */
void bt_manager_pack_links(uint8_t *player_byte, uint8_t *link_byte,
                           uint8_t player_a, uint8_t link_a,
                           uint8_t player_b, uint8_t link_b);
void bt_manager_apply_links(bt_ipc_state_t *ipc, uint8_t player_byte, uint8_t link_byte);

/* Resposta positiva de link key (HCI 0x040B) para o endereco gravado.
 * Retorna o tamanho do comando escrito em out_buf, ou 0 se bd_addr nao esta gravado. */
size_t bt_manager_build_link_key_reply(const bt_ipc_state_t *ipc, const uint8_t bd_addr[6],
                                       uint8_t *out_buf, size_t out_cap);

/* Active player slot management (up to 4 controllers) */
int bt_manager_assign_slot(bt_ipc_state_t *ipc, const uint8_t bd_addr[6],
                           uint16_t acl_handle, const char *name);
int bt_manager_release_slot(bt_ipc_state_t *ipc, uint32_t slot_idx);
int bt_manager_find_slot_by_handle(const bt_ipc_state_t *ipc, uint16_t acl_handle);

/* INI serialization/deserialization for Usb:\bt_devices.ini (zero-alloc) */
size_t bt_manager_export_ini(const bt_ipc_state_t *ipc, char *out_buf, size_t cap);
int bt_manager_import_ini(bt_ipc_state_t *ipc, const char *ini_text, size_t len);

/* Tarefa 07: existe um DualShock 4 gravado? (nenhum novo pareamento e oferecido se sim) */
bool bt_manager_has_stored_ds4(const bt_ipc_state_t *ipc);

/* Tarefa 07: copia para status_msg o texto de bt_phase1_phrase_text(phrase_id) quando o id
 * e uma frase de DualShock 4 do reply do plugin. Retorna false (status_msg intocado) para
 * BT_PHRASE_NONE, ids desconhecidos e frases de adaptador. */
bool bt_ui_apply_reply_phrase(bt_ipc_state_t *ipc, uint32_t phrase_id);

/* Lista da busca: arquivo fixo entre o plugin e a tela (o reply de 32 bytes nao cabe). */
size_t bt_scan_pack(uint8_t *dst, size_t cap, const hci_inquiry_item_t *items,
                    uint32_t count, bool scanning);
bool bt_scan_apply(bt_ipc_state_t *ipc, const uint8_t *src, size_t len);

/* Uma busca nova zera o arquivo. O controle ja gravado continua na lista. */
void bt_manager_keep_stored_in_scan(bt_ipc_state_t *ipc);

/* UI navigation & command dispatch */
void bt_ui_model_init(bt_ui_model_t *m);
void bt_ui_model_select_initial_pane(bt_ui_model_t *m, const bt_ipc_state_t *ipc);
void bt_ui_handle_input(bt_ui_model_t *m, bt_ipc_state_t *ipc, uint16_t newly_pressed_buttons);
void bt_ipc_issue_cmd(bt_ipc_state_t *ipc, bt_ipc_cmd_t cmd,
                      const uint8_t *bd_addr, uint32_t slot);

/* Interactive 1-by-1 button remap wizard */
void bt_remap_wizard_start(bt_remap_wizard_t *w, uint32_t slot_idx, uint32_t current_raw_mask);
bool bt_remap_wizard_feed_raw(bt_remap_wizard_t *w, uint32_t raw_mask, bt_button_map_t *map);

#ifdef __cplusplus
}
#endif

#endif /* XBOX360_BT_MANAGER_H */
