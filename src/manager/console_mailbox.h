#ifndef BT_CONSOLE_MAILBOX_H
#define BT_CONSOLE_MAILBOX_H

#include "bt_manager.h"

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint64_t token;
    uint32_t command;
    uint32_t slot;
    uint8_t address[6];
    uint8_t reserved[2];
} bt_console_request_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint64_t token;
    int32_t usb_status;
    uint32_t adapter_stage;
    uint32_t phrase_id;   /* bt_phase1_phrase_id_t (tabela unica em bt_phase1.h) */
    uint8_t player;       /* 0 = nenhum, 1 ou 2 */
    uint8_t pair_active;  /* 0 = falso, 1 = pareamento ativo */
    uint8_t reserved[2];
} bt_console_reply_t;

/* Diagnostic mailbox on the same USB volume as bt360_diag.log. */
bool bt_console_read_request(bt_console_request_t *request);
bool bt_console_write_request(const bt_console_request_t *request);
bool bt_console_read_reply(bt_console_reply_t *reply);
bool bt_console_write_reply(const bt_console_reply_t *reply);

#endif
