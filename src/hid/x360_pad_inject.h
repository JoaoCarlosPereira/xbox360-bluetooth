#ifndef XBOX360_BT_X360_PAD_INJECT_H
#define XBOX360_BT_X360_PAD_INJECT_H

/*
 * Publicacao do estado de UM DualShock 4 no slot livre do XamInput (tarefa 08).
 *
 * Logica pura: nao conhece XamInput, kernel nem enderecos. Quem escreve de fato no
 * slot (e quem envia o quadro de vibracao ao DS4) e o x360_pad_sink_t. Assim a mesma
 * regra roda no host (src/linux_test/test_pad_inject.c) e, depois, no plugin.
 *
 * Regras:
 *  - nada e publicado antes de o DS4 estar autenticado nem sem jogador decidido;
 *  - o jogador (1 ou 2) vem da sessao de pareamento; este modulo so escreve nele;
 *  - o slot do controle oficial nunca e escrito;
 *  - na queda, zera somente o slot que o DS4 ocupava.
 *
 * Contexto de execucao: uma unica thread (a do plugin).
 */

#include "ds4_bt.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Jogadores que o DS4 pode ocupar: 1..4 (indice XInput = jogador - 1). */
#define X360_PAD_PLAYER_MIN 1u
#define X360_PAD_PLAYER_MAX 4u

typedef struct {
    void *ctx;
    /* Grava o estado no slot XInput user_index (0 = jogador 1, 1 = jogador 2). */
    void (*write_slot)(void *ctx, uint8_t user_index, const x360_gamepad_state_t *state);
    /* Envia o quadro HIDP de saida (ds4_bt_build_rumble_report) ao DS4. */
    void (*send_report)(void *ctx, const uint8_t *report, size_t len);
} x360_pad_sink_t;

typedef struct {
    x360_pad_sink_t      sink;
    /* Campos abaixo: escritos pela thread do plugin sob seq (impar = escrita em curso) e lidos
     * pelos ganchos do XamInput, que rodam em outras threads (x360_pad_inject_snapshot). */
    uint32_t             seq;
    bool                 active;      /* ja publicou e o slot pertence ao DS4 */
    uint8_t              player;      /* 1 ou 2, valido se active */
    uint32_t             packet;      /* sobe a cada publish */
    x360_gamepad_state_t last;        /* ultimo estado publicado (com o bit Guide) */
} x360_pad_inject_t;

typedef struct {
    bool                 active;
    uint8_t              player;
    uint32_t             packet;
    x360_gamepad_state_t state;
} x360_pad_snapshot_t;

void x360_pad_inject_init(x360_pad_inject_t *p, const x360_pad_sink_t *sink);

/*
 * Publica o estado do DS4 no slot do jogador decidido pela sessao.
 *  authenticated: sessao em BT_PAIR_CONNECTED com established;
 *  player:        bt_pair_session_t.player (1 ou 2; qualquer outro valor nao publica);
 *  ps_pressed:    botao PS, injetado como X360_BTN_GUIDE (0x0400).
 * Retorna true se escreveu no slot. Se o jogador mudar enquanto ativo, zera o slot antigo.
 */
bool x360_pad_inject_publish(x360_pad_inject_t *p, bool authenticated, uint8_t player,
                             const x360_gamepad_state_t *state, bool ps_pressed);

/* Queda do DS4: zera so o slot dele (nada se nunca publicou). */
void x360_pad_inject_drop(x360_pad_inject_t *p);

/*
 * Vibracao: monta o quadro com ds4_bt_build_rumble_report e o entrega ao sink.
 * Retorna o tamanho enviado, ou 0 se nao ha DS4 ativo / sink sem send_report.
 */
size_t x360_pad_inject_rumble(x360_pad_inject_t *p, uint8_t small_motor, uint8_t large_motor);

/* Copia coerente do estado (seqlock); segura de qualquer thread. */
x360_pad_snapshot_t x360_pad_inject_snapshot(const x360_pad_inject_t *p);

/*
 * Regras dos ganchos do XamInput (main.cpp:1307, 1330 e 1365 do hiddriver360). Cada uma recebe o
 * resultado do original, ja chamado, e so muda algo no user index do DS4 ativo
 * (jogador 1 = 0, jogador 2 = 1). Qualquer outro user fica com o resultado do original.
 */
#define X360_PAD_ERROR_SUCCESS             0u
#define X360_PAD_ERROR_DEVICE_NOT_CONNECTED 0x48Fu
#define X360_PAD_DEVTYPE_GAMEPAD           1u
#define X360_PAD_DEVSUBTYPE_GAMEPAD        1u

/* Primeiros 20 bytes de XINPUT_CAPABILITIES(_EX): type, subtype, flags, gamepad, vibration. */
typedef struct {
    uint8_t              type;
    uint8_t              subtype;
    uint16_t             flags;
    x360_gamepad_state_t gamepad;
    uint16_t             vibration_left;
    uint16_t             vibration_right;
} x360_pad_caps_t;

/* user & 0xFF == 0xFF vira 0, como nos ganchos do hiddriver. */
uint32_t x360_pad_inject_normalize_user(uint32_t user);

/* XamInputGetCapabilitiesEx: original sem controle + DS4 ativo no user => gamepad, sucesso. */
uint32_t x360_pad_inject_filter_capabilities(const x360_pad_inject_t *p, uint32_t user,
                                             uint32_t original_status, x360_pad_caps_t *caps);

/*
 * XamInputSetState: original sem controle + DS4 ativo no user => vibracao (16 bits por motor,
 * esquerdo = grande) entregue a x360_pad_inject_rumble; sucesso.
 */
uint32_t x360_pad_inject_filter_set_state(x360_pad_inject_t *p, uint32_t user,
                                          uint32_t original_status, uint16_t left_motor,
                                          uint16_t right_motor);

/*
 * XInputdReadState: o gancho recebe um contexto de dispositivo, nao um user index; quem chama
 * diz se o contexto e o do DS4 ativo. Nesse caso devolve o ultimo estado publicado em *gamepad
 * (e o numero do pacote) com sucesso; senao devolve original_status e nao toca nos ponteiros.
 */
uint32_t x360_pad_inject_filter_read_state(const x360_pad_inject_t *p, bool context_is_ds4,
                                           uint32_t original_status, uint32_t *packet_number,
                                           x360_gamepad_state_t *gamepad);

/*
 * Contexto do DS4 no XAM (hiddriver360 main.cpp:498-501 e 1204): XamUserBindDeviceCallback
 * recebe controllerId = 0xA7553952 + indice e context = 0x10000005 + indice. Um unico DS4
 * usa o indice 0. O gancho de XInputdReadState so responde a esse contexto.
 */
#define X360_PAD_CONTROLLER_ID_BASE 0xA7553952u
#define X360_PAD_CONTEXT_BASE       0x10000005u
#define X360_PAD_USER_INDEX_NONE    0xFFu  /* valor inicial do userIndex de saida (uint8_t -1) */

typedef enum {
    X360_PAD_BIND_OK = 0,        /* userIndex e o do jogador ja decidido */
    X360_PAD_BIND_OFFICIAL_SLOT, /* userIndex e o do controle oficial: nao publicar */
    X360_PAD_BIND_MISMATCH       /* qualquer outro valor (inclusive 0xFF): nao publicar */
} x360_pad_bind_verdict_t;

static inline uint32_t x360_pad_controller_id(uint32_t index) { return X360_PAD_CONTROLLER_ID_BASE + index; }
static inline uint32_t x360_pad_device_context(uint32_t index) { return X360_PAD_CONTEXT_BASE + index; }

/*
 * Confere o userIndex devolvido pela alta contra o jogador decidido pela sessao (1 = user 0,
 * 2 = user 1). Com o DS4 no jogador 2, o user 0 e o do oficial.
 */
x360_pad_bind_verdict_t x360_pad_inject_check_bind(uint8_t decided_player, uint8_t returned_user);

/*
 * O painel do Xbox não lê o gamepad. Ele pede uma tecla por vez (XINPUT_KEYSTROKE).
 * Esta fila transforma o estado do DS4 nessas teclas: descida, repetição e subida.
 * O analógico esquerdo vira o direcional quando o direcional físico está solto.
 */
#define X360_PAD_ERROR_EMPTY 0x10D2u
#define X360_KEY_DOWN        0x0001u
#define X360_KEY_UP          0x0002u
#define X360_KEY_REPEAT      0x0004u

typedef struct {
    uint16_t virtual_key;
    uint16_t unicode;
    uint16_t flags;
    uint8_t  user_index;
    uint8_t  hid_code;
} x360_keystroke_t;

typedef struct {
    uint16_t level;
    uint16_t pending_down;
    uint16_t pending_up;
    uint16_t repeat_bit;
    uint32_t next_repeat;
} x360_key_tracker_t;

/* SUCCESS e preenche uma tecla, ou EMPTY quando não há evento neste instante. */
uint32_t x360_pad_keystroke_next(x360_key_tracker_t *tracker, const x360_gamepad_state_t *state,
                                 uint8_t user_index, uint32_t now_ms, x360_keystroke_t *out);

#ifdef __cplusplus
}
#endif

#endif /* XBOX360_BT_X360_PAD_INJECT_H */
