#ifndef BT_PAIR_SESSION_H
#define BT_PAIR_SESSION_H

/*
 * Sessao de pareamento/reconexao de um unico DualShock 4 (tarefa 06).
 *
 * Logica pura: nao conhece USB, arquivos nem a API do Xbox. Tudo o que toca o
 * mundo exterior passa por bt_pair_ops_t, entao os mesmos fluxos rodam no host
 * (src/linux_test/test_pair_session.c) e no plugin.
 *
 * Contexto de execucao: uma unica thread (a do plugin). Os eventos HCI chegam
 * pela fila SPSC do transporte (bt_usb_transport_pop_event).
 *
 * Decisoes de jogador, frases, aceite e timeout vem de bt_phase1_decide; a
 * sessao nao repete essas regras.
 */

#include "bt_manager.h"
#include "bt_phase1.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Duracao do Inquiry em unidades de 1,28 s; 48 (~61,4 s) cobre o limite de 60 s. */
#define BT_PAIR_INQUIRY_UNITS      48u
/* Espera entre tentativas de reconexao ao DS4 gravado. */
#define BT_PAIR_RECONNECT_RETRY_MS 3000u
/* Intervalo minimo entre avisos de "ja existe um DualShock 4" (PAR-03). */
#define BT_PAIR_SECOND_DS4_MS      10000u

typedef struct {
    void *ctx;
    /* Enfileira um comando HCI (transporte). Retorna 0 se aceito. */
    int  (*send_cmd)(void *ctx, uint16_t opcode, const uint8_t *params, uint8_t len);
    /* Reaproveita o fluxo de inquiry do transporte. */
    int  (*start_inquiry)(void *ctx, uint8_t duration_1280ms);
    int  (*stop_inquiry)(void *ctx);
    /* true quando o controle oficial ocupa o jogador 1 (XamInputGetState). */
    bool (*official_on_player1)(void *ctx);
    /* Grava bt_devices.ini a partir do estado atual de ipc. */
    void (*persist)(void *ctx, const bt_ipc_state_t *ipc);
    /* Aviso na tela; o texto vem sempre de bt_phase1_phrase_text. */
    void (*notify)(void *ctx, uint32_t phrase_id);
    /* Diagnostico opcional. */
    void (*log)(void *ctx, const char *what, uint32_t value);
} bt_pair_ops_t;

typedef enum {
    BT_PAIR_IDLE = 0,    /* nada em andamento */
    BT_PAIR_SCANNING,    /* inquiry ativo, aguardando um DS4 */
    BT_PAIR_CONNECTING,  /* Create_Connection / Accept enviado */
    BT_PAIR_AUTH,        /* ACL aberto, autenticando */
    BT_PAIR_CONNECTED    /* autenticado; sessao estabelecida */
} bt_pair_state_t;

/* Mascara dos comandos de preparo do adaptador que ainda faltam responder. */
#define BT_PAIR_SETUP_EVENT_MASK  0x01u
#define BT_PAIR_SETUP_SSP         0x02u
#define BT_PAIR_SETUP_PAGE_TO     0x04u
#define BT_PAIR_SETUP_SCAN        0x08u
#define BT_PAIR_SETUP_ALL         0x0Fu

typedef struct {
    bt_pair_ops_t   ops;
    bt_ipc_state_t *ipc;

    bool            adapter_ready;
    uint8_t         setup_pending;

    bt_pair_state_t state;
    bool            pair_active;     /* publicado em bt_console_reply_t */
    bool            want_inquiry;
    uint64_t        pair_start_ms;

    /* Ultimo evento de DS4 publicado (reply do console). */
    uint32_t        phrase_id;
    uint8_t         player;

    /* Par em andamento. */
    uint8_t         peer[6];
    uint8_t         peer_psr;
    uint16_t        peer_clk;
    bool            has_peer;
    bool            incoming;        /* conexao aceita (Accept) em vez de Create */
    uint8_t         target[6];       /* endereco pedido pelo comando; valido se has_target */
    bool            has_target;
    uint16_t        acl_handle;
    bool            key_valid;
    uint8_t         key[16];
    uint8_t         key_type;
    bool            established;     /* autenticado: queda vira DS4_DISCONNECT */
    bool            encrypted;       /* Encryption Change ok; so entao abre o HID */

    bool            retry_armed;
    bool            user_held;       /* o jogador pediu desconectar; nao repagina sozinho */
    uint64_t        retry_at_ms;
    uint64_t        last_second_ms;
    bool            second_notified;
} bt_pair_session_t;

void bt_pair_session_init(bt_pair_session_t *s, bt_ipc_state_t *ipc, const bt_pair_ops_t *ops);

/* Adaptador pronto (re-init incluido): prepara o HCI e, havendo DS4 gravado,
 * conecta nele sem inquiry e sem esperar o gerenciador (PAR-05). */
void bt_pair_session_adapter_ready(bt_pair_session_t *s, uint64_t now_ms);
/* Adaptador removido ou em erro: zera a sessao sem frase de DS4. */
void bt_pair_session_adapter_lost(bt_pair_session_t *s);

/* Comando do mailbox. addr: 6 bytes na ordem HCI (como em hci_inquiry_item_t) ou NULL. */
void bt_pair_session_on_command(bt_pair_session_t *s, uint32_t command,
                                const uint8_t *addr, uint64_t now_ms);
/* Evento HCI cru (codigo, tamanho, parametros). */
void bt_pair_session_on_event(bt_pair_session_t *s, const uint8_t *evt, size_t len,
                              uint64_t now_ms);
/* Relogio: timeout de 60 s (via bt_phase1_decide) e novas tentativas. */
void bt_pair_session_tick(bt_pair_session_t *s, uint64_t now_ms);

/* Endereco do DS4 gravado em ipc (ordem HCI) ou NULL. */
const uint8_t *bt_pair_session_stored_addr(const bt_pair_session_t *s);
/* CoD de gamepad (0x002508) ou nome "Wireless Controller". */
bool bt_pair_is_ds4_candidate(const hci_inquiry_item_t *item);
/* A na lista: pagina esse endereco agora, sem abrir outra busca. */
void bt_pair_session_connect_known(bt_pair_session_t *s, const hci_inquiry_item_t *item,
                                   uint64_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* BT_PAIR_SESSION_H */
