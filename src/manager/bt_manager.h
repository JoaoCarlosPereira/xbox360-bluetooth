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
#define BT_MAX_PAIRED_DEVICES    8u
#define BT_MAX_SCAN_ITEMS        16u

typedef enum {
    BT_IPC_CMD_NONE = 0,
    BT_IPC_CMD_START_SCAN,
    BT_IPC_CMD_STOP_SCAN,
    BT_IPC_CMD_PAIR_AND_CONNECT,
    BT_IPC_CMD_CONNECT_PAIRED,
    BT_IPC_CMD_DISCONNECT_SLOT,
    BT_IPC_CMD_FORGET_PAIRED
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

    /* Dongle status */
    bool               dongle_ready;
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
    bool         exit_requested;
} bt_ui_model_t;

void bt_ipc_init(bt_ipc_state_t *ipc);

/* Paired device / Link Key management */
int bt_manager_upsert_paired(bt_ipc_state_t *ipc, const uint8_t bd_addr[6],
                             const uint8_t link_key[16], uint8_t key_type,
                             uint32_t cod, const char *name);
const bt_paired_device_t *bt_manager_find_paired(const bt_ipc_state_t *ipc,
                                                 const uint8_t bd_addr[6]);
int bt_manager_forget_paired(bt_ipc_state_t *ipc, const uint8_t bd_addr[6]);

/* Registro de um unico DualShock 4 (PAR-05 / LIM-01).
 * Retorna indice >= 0 em sucesso; -1 argumento invalido (player fora de 1..2);
 * -3 se ja existe outro endereco gravado (o registro existente e preservado). */
int bt_manager_store_single_ds4(bt_ipc_state_t *ipc, const uint8_t bd_addr[6],
                                const uint8_t link_key[16], uint8_t key_type,
                                uint8_t player, const char *name);

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

/* UI navigation & command dispatch */
void bt_ui_model_init(bt_ui_model_t *m);
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
