/*
 * Tarefa 06: sessao de pareamento/reconexao de um unico DualShock 4.
 * Casos escritos a partir do card (PAR-01..05, LIM-01, ERR-03, timeout 59/60/61),
 * antes da implementacao de bt_pair_session.c.
 */
#include "../manager/bt_pair_session.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define S 1000u /* ms por segundo */

static const uint8_t ADDR_A[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };
static const uint8_t ADDR_B[6] = { 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6 };
static const uint8_t KEY_A[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };

typedef struct {
    uint16_t opcode;
    uint8_t  len;
    uint8_t  params[32];
} cmd_rec_t;

typedef struct {
    cmd_rec_t cmds[64];
    int       ncmds;
    int       inq_start;
    int       inq_stop;
    uint8_t   inq_units;
    int       persist;
    int       notified[16];
    int       nnotified;
    bool      official;
} mock_t;

static int m_send(void *ctx, uint16_t opcode, const uint8_t *params, uint8_t len) {
    mock_t *m = ctx;
    assert(m->ncmds < 64);
    cmd_rec_t *c = &m->cmds[m->ncmds++];
    c->opcode = opcode;
    c->len = len;
    assert(len <= sizeof(c->params));
    if (len) memcpy(c->params, params, len);
    return 0;
}
static int m_start_inq(void *ctx, uint8_t units) {
    mock_t *m = ctx;
    m->inq_start++;
    m->inq_units = units;
    return 0;
}
static int m_stop_inq(void *ctx) { ((mock_t *)ctx)->inq_stop++; return 0; }
static bool m_official(void *ctx) { return ((mock_t *)ctx)->official; }
static void m_persist(void *ctx, const bt_ipc_state_t *ipc) {
    (void)ipc;
    ((mock_t *)ctx)->persist++;
}
static void m_notify(void *ctx, uint32_t phrase) {
    mock_t *m = ctx;
    assert(m->nnotified < 16);
    m->notified[m->nnotified++] = (int)phrase;
}

typedef struct {
    mock_t            m;
    bt_ipc_state_t    ipc;
    bt_pair_session_t s;
} rig_t;

static void rig_init(rig_t *r, bool with_stored) {
    memset(r, 0, sizeof(*r));
    bt_ipc_init(&r->ipc);
    if (with_stored) {
        assert(bt_manager_store_single_ds4(&r->ipc, ADDR_A, KEY_A, 5u, 1u, "DS4") >= 0);
    }
    bt_pair_ops_t ops = {
        .ctx = &r->m, .send_cmd = m_send, .start_inquiry = m_start_inq,
        .stop_inquiry = m_stop_inq, .official_on_player1 = m_official,
        .persist = m_persist, .notify = m_notify, .log = NULL
    };
    bt_pair_session_init(&r->s, &r->ipc, &ops);
}

static int count_op(const mock_t *m, uint16_t opcode) {
    int n = 0;
    for (int i = 0; i < m->ncmds; i++) if (m->cmds[i].opcode == opcode) n++;
    return n;
}

static const cmd_rec_t *last_op(const mock_t *m, uint16_t opcode) {
    for (int i = m->ncmds - 1; i >= 0; i--) if (m->cmds[i].opcode == opcode) return &m->cmds[i];
    return NULL;
}

/* ---- construtores de eventos HCI ------------------------------------------------ */

static size_t evt_cmd_complete(uint8_t *b, uint16_t opcode) {
    b[0] = HCI_EVT_COMMAND_COMPLETE; b[1] = 4; b[2] = 1;
    hci_put_le16(b + 3, opcode); b[5] = 0;
    return 6;
}
static size_t evt_inquiry(uint8_t *b, const uint8_t *addr, uint32_t cod) {
    b[0] = HCI_EVT_INQUIRY_RESULT; b[1] = 15; b[2] = 1;
    memcpy(b + 3, addr, 6);
    b[9] = 0x01; b[10] = 0; b[11] = 0;
    b[12] = (uint8_t)cod; b[13] = (uint8_t)(cod >> 8); b[14] = (uint8_t)(cod >> 16);
    hci_put_le16(b + 15, 0x1234u);
    return 17;
}
static size_t evt_conn_complete(uint8_t *b, uint8_t status, uint16_t handle, const uint8_t *addr) {
    b[0] = HCI_EVT_CONNECTION_COMPLETE; b[1] = 11; b[2] = status;
    hci_put_le16(b + 3, handle); memcpy(b + 5, addr, 6); b[11] = 1; b[12] = 0;
    return 13;
}
static size_t evt_conn_request(uint8_t *b, const uint8_t *addr, uint32_t cod) {
    b[0] = HCI_EVT_CONNECTION_REQUEST; b[1] = 10; memcpy(b + 2, addr, 6);
    b[8] = (uint8_t)cod; b[9] = (uint8_t)(cod >> 8); b[10] = (uint8_t)(cod >> 16); b[11] = 1;
    return 12;
}
static size_t evt_addr_only(uint8_t *b, uint8_t code, const uint8_t *addr, uint8_t extra) {
    b[0] = code; b[1] = (uint8_t)(6 + extra); memcpy(b + 2, addr, 6);
    memset(b + 8, 0, extra);
    return (size_t)(2 + 6 + extra);
}
static size_t evt_link_key_notif(uint8_t *b, const uint8_t *addr, const uint8_t *key, uint8_t type) {
    b[0] = HCI_EVT_LINK_KEY_NOTIFICATION; b[1] = 23; memcpy(b + 2, addr, 6);
    memcpy(b + 8, key, 16); b[24] = type;
    return 25;
}
static size_t evt_auth_complete(uint8_t *b, uint8_t status, uint16_t handle) {
    b[0] = HCI_EVT_AUTH_COMPLETE; b[1] = 3; b[2] = status; hci_put_le16(b + 3, handle);
    return 5;
}
static size_t evt_disconnect(uint8_t *b, uint16_t handle, uint8_t reason) {
    b[0] = HCI_EVT_DISCONNECTION_COMPLETE; b[1] = 4; b[2] = 0;
    hci_put_le16(b + 3, handle); b[5] = reason;
    return 6;
}

static void feed(rig_t *r, const uint8_t *evt, size_t len, uint64_t now) {
    bt_pair_session_on_event(&r->s, evt, len, now);
}

static void complete_setup(rig_t *r, uint64_t now) {
    static const uint16_t ops[] = { HCI_OP_SET_EVENT_MASK, HCI_OP_WRITE_SIMPLE_PAIRING,
                                    HCI_OP_WRITE_PAGE_TIMEOUT, HCI_OP_WRITE_SCAN_ENABLE };
    uint8_t e[8];
    for (size_t i = 0; i < 4; i++) feed(r, e, evt_cmd_complete(e, ops[i]), now);
    assert(r->s.setup_pending == 0);
}

/* Inicia pareamento sem DS4 gravado e deixa o inquiry rodando. */
static void start_pairing(rig_t *r, uint64_t t0) {
    bt_pair_session_adapter_ready(&r->s, 0);
    bt_pair_session_on_command(&r->s, BT_IPC_CMD_PAIR_AND_CONNECT, NULL, t0);
    complete_setup(r, t0);
    assert(r->m.inq_start == 1);
}

static void select_ds4(rig_t *r, uint64_t now) {
    uint8_t e[32];
    feed(r, e, evt_inquiry(e, ADDR_A, 0x002508u), now);
}

/* Conexao ate o fim da autenticacao de um pareamento novo. */
static void run_pair_handshake(rig_t *r, uint64_t now) {
    uint8_t e[32];
    feed(r, e, evt_conn_complete(e, 0, 0x0021u, ADDR_A), now);
    feed(r, e, evt_addr_only(e, HCI_EVT_LINK_KEY_REQUEST, ADDR_A, 0), now);
    feed(r, e, evt_addr_only(e, HCI_EVT_IO_CAPABILITY_REQUEST, ADDR_A, 0), now);
    feed(r, e, evt_addr_only(e, HCI_EVT_USER_CONFIRM_REQUEST, ADDR_A, 4), now);
    feed(r, e, evt_link_key_notif(e, ADDR_A, KEY_A, 5u), now);
    feed(r, e, evt_auth_complete(e, 0, 0x0021u), now);
}

static size_t evt_encrypt(uint8_t *b, uint8_t status, uint16_t handle, uint8_t on);

/* ---- PAR-05 / reconexao ------------------------------------------------------- */

static void test_par_05_reconnect_without_inquiry(void) {
    rig_t r;
    rig_init(&r, true);
    bt_pair_session_adapter_ready(&r.s, 0);

    assert(r.m.inq_start == 0);
    assert(!r.s.pair_active);
    assert(count_op(&r.m, HCI_OP_WRITE_SIMPLE_PAIRING) == 1);
    assert(count_op(&r.m, HCI_OP_WRITE_SCAN_ENABLE) == 1);
    assert(count_op(&r.m, HCI_OP_SET_EVENT_MASK) == 1);
    const cmd_rec_t *c = last_op(&r.m, HCI_OP_CREATE_CONNECTION);
    assert(c && c->len == 13);
    assert(memcmp(c->params, ADDR_A, 6) == 0);
    assert(r.s.state == BT_PAIR_CONNECTING);
    assert(bt_pair_session_stored_addr(&r.s) != NULL);
    printf("[PASS] PAR-05 reconecta ao DS4 gravado sem inquiry nem mailbox\n");
}

static void test_link_key_reply_with_stored_key(void) {
    rig_t r;
    rig_init(&r, true);
    bt_pair_session_adapter_ready(&r.s, 0);
    uint8_t e[32];
    feed(&r, e, evt_conn_complete(e, 0, 0x0030u, ADDR_A), 100);
    const cmd_rec_t *auth = last_op(&r.m, HCI_OP_AUTH_REQUESTED);
    assert(auth && auth->len == 2 && hci_get_le16(auth->params) == 0x0030u);

    feed(&r, e, evt_addr_only(e, HCI_EVT_LINK_KEY_REQUEST, ADDR_A, 0), 110);
    const cmd_rec_t *k = last_op(&r.m, HCI_OP_LINK_KEY_REQ_REPLY);
    assert(k && k->len == 22);
    assert(memcmp(k->params, ADDR_A, 6) == 0);
    assert(memcmp(k->params + 6, KEY_A, 16) == 0);
    assert(count_op(&r.m, HCI_OP_LINK_KEY_REQ_NEG_REPLY) == 0);

    feed(&r, e, evt_auth_complete(e, 0, 0x0030u), 120);
    assert(r.s.state == BT_PAIR_AUTH && !r.s.established);
    assert(count_op(&r.m, HCI_OP_SET_CONN_ENCRYPT) == 0);
    feed(&r, e, evt_encrypt(e, 0, 0x0030u, 1), 130);
    assert(r.s.state == BT_PAIR_CONNECTED && r.s.established && r.s.encrypted);
    assert(r.s.player == 1u); /* jogador gravado volta na reconexao, sem frase nova */
    assert(count_op(&r.m, HCI_OP_SET_CONN_ENCRYPT) == 0);
    assert(r.m.nnotified == 0); /* reconexao nao tem frase */
    printf("[PASS] PAR-05 pedido de link key respondido com a chave gravada (0x040B)\n");
}

static size_t evt_encrypt(uint8_t *b, uint8_t status, uint16_t handle, uint8_t on) {
    b[0] = HCI_EVT_ENCRYPTION_CHANGE;
    b[1] = 4;
    b[2] = status;
    hci_put_le16(b + 3, handle);
    b[5] = on;
    return 6;
}

/* O gerenciador atrasa o HCI e a autenticação volta 0x0C com a cifra já ligada. */
static void test_auth_disallowed_keeps_link(void) {
    rig_t r;
    rig_init(&r, true);
    bt_pair_session_adapter_ready(&r.s, 0);
    uint8_t e[32];
    feed(&r, e, evt_conn_complete(e, 0, 0x0030u, ADDR_A), 100);
    const int disc = count_op(&r.m, HCI_OP_DISCONNECT);
    feed(&r, e, evt_auth_complete(e, 0x0Cu, 0x0030u), 110);
    assert(count_op(&r.m, HCI_OP_DISCONNECT) == disc);
    assert(r.s.state == BT_PAIR_AUTH && !r.s.established);
    feed(&r, e, evt_encrypt(e, 0, 0x0030u, 1), 120);
    assert(r.s.state == BT_PAIR_CONNECTED && r.s.established && r.s.encrypted);
    assert(count_op(&r.m, HCI_OP_SET_CONN_ENCRYPT) == 0);
    assert(count_op(&r.m, HCI_OP_DISCONNECT) == disc);
    printf("[PASS] auth 0x0C não derruba o link; a cifragem conclui a sessão\n");
}

static void test_reconnect_wait_not_blocking_on_mailbox(void) {
    rig_t r;
    rig_init(&r, true);
    bt_pair_session_adapter_ready(&r.s, 0);
    /* Sem nenhum comando do mailbox o Create_Connection ja saiu. */
    assert(count_op(&r.m, HCI_OP_CREATE_CONNECTION) == 1);
    uint8_t e[32];
    feed(&r, e, evt_conn_complete(e, 0x04, 0, ADDR_A), 10 * S); /* page timeout */
    assert(r.s.state == BT_PAIR_IDLE);
    assert(count_op(&r.m, HCI_OP_CREATE_CONNECTION) == 1);
    bt_pair_session_tick(&r.s, 10 * S + BT_PAIR_RECONNECT_RETRY_MS);
    assert(count_op(&r.m, HCI_OP_CREATE_CONNECTION) == 1);
    bt_pair_session_tick(&r.s, 10 * S + BT_PAIR_LISTEN_MS);
    assert(count_op(&r.m, HCI_OP_CREATE_CONNECTION) == 2);
    assert(r.m.inq_start == 0);
    printf("[PASS] PAR-05 nova tentativa de reconexao sem inquiry\n");
}

/* ---- pareamento --------------------------------------------------------------- */

static void test_inquiry_only_after_pair_command(void) {
    rig_t r;
    rig_init(&r, false);
    bt_pair_session_adapter_ready(&r.s, 0);
    assert(r.m.inq_start == 0);
    assert(count_op(&r.m, HCI_OP_CREATE_CONNECTION) == 0);
    complete_setup(&r, 10);
    bt_pair_session_tick(&r.s, 5 * S);
    assert(r.m.inq_start == 0);
    assert(!r.s.pair_active);

    /* O comando chegou antes de o adaptador terminar o preparo: espera. */
    rig_init(&r, false);
    bt_pair_session_adapter_ready(&r.s, 0);
    bt_pair_session_on_command(&r.s, BT_IPC_CMD_PAIR_AND_CONNECT, NULL, 1 * S);
    assert(r.s.pair_active);
    assert(r.m.inq_start == 0);
    complete_setup(&r, 1 * S + 50);
    assert(r.m.inq_start == 1);
    assert(r.m.inq_units == BT_PAIR_INQUIRY_UNITS);
    assert(r.s.state == BT_PAIR_SCANNING);

    /* Adaptador ja pronto: o inquiry sai no ato. */
    rig_init(&r, false);
    start_pairing(&r, 2 * S);
    assert(r.s.pair_active && r.s.state == BT_PAIR_SCANNING);
    printf("[PASS] inquiry so com comando de parear e sem DS4 gravado\n");
}

static void test_ds4_candidate_filter(void) {
    hci_inquiry_item_t it;
    memset(&it, 0, sizeof(it));
    it.class_of_device = 0x002508u;
    assert(bt_pair_is_ds4_candidate(&it));
    it.class_of_device = 0x002540u; /* teclado */
    assert(!bt_pair_is_ds4_candidate(&it));
    it.class_of_device = 0x5A020Cu; /* celular */
    assert(!bt_pair_is_ds4_candidate(&it));
    memcpy(it.name, "Wireless Controller", 20);
    assert(bt_pair_is_ds4_candidate(&it));
    printf("[PASS] filtro de candidato DualShock 4\n");
}

static void test_par_02_pair_player1(void) {
    rig_t r;
    rig_init(&r, false);
    r.m.official = false;
    start_pairing(&r, 0);

    uint8_t e[32];
    static const uint8_t keyboard[6] = { 9, 9, 9, 9, 9, 9 };
    feed(&r, e, evt_inquiry(e, keyboard, 0x002540u), 1 * S);
    assert(count_op(&r.m, HCI_OP_CREATE_CONNECTION) == 0);

    select_ds4(&r, 2 * S);
    assert(r.m.inq_stop == 1);
    const cmd_rec_t *cc = last_op(&r.m, HCI_OP_CREATE_CONNECTION);
    assert(cc && memcmp(cc->params, ADDR_A, 6) == 0);
    assert(hci_get_le16(cc->params + 10) == (uint16_t)(0x1234u | 0x8000u));
    assert(r.s.state == BT_PAIR_CONNECTING);

    /* Outro DS4 durante a conexao nao vira um segundo candidato. */
    feed(&r, e, evt_inquiry(e, ADDR_B, 0x002508u), 3 * S);
    assert(count_op(&r.m, HCI_OP_CREATE_CONNECTION) == 1);

    run_pair_handshake(&r, 4 * S);

    const cmd_rec_t *auth = last_op(&r.m, HCI_OP_AUTH_REQUESTED);
    assert(auth && hci_get_le16(auth->params) == 0x0021u);
    assert(count_op(&r.m, HCI_OP_LINK_KEY_REQ_NEG_REPLY) == 1);
    const cmd_rec_t *io = last_op(&r.m, HCI_OP_IO_CAP_REQ_REPLY);
    assert(io && io->len == 9 && io->params[6] == 0x03 && io->params[7] == 0x00 &&
           io->params[8] == 0x04);
    assert(count_op(&r.m, HCI_OP_USER_CONFIRM_REQ_REPLY) == 1);
    assert(count_op(&r.m, HCI_OP_SET_CONN_ENCRYPT) == 1);

    const bt_paired_device_t *d = bt_manager_find_paired(&r.ipc, ADDR_A);
    assert(d && d->valid && d->player == 1u);
    assert(d->class_of_device == 0x002508u);
    assert(d->key_type == 5u && memcmp(d->link_key, KEY_A, 16) == 0);
    assert(r.m.persist == 1);
    assert(r.m.nnotified == 1 && r.m.notified[0] == (int)BT_PHRASE_DS4_PAIRED_PLAYER1);
    assert(r.s.phrase_id == BT_PHRASE_DS4_PAIRED_PLAYER1 && r.s.player == 1u);
    assert(!r.s.pair_active);
    assert(r.s.state == BT_PAIR_CONNECTED);
    assert(strcmp(bt_phase1_phrase_text(r.s.phrase_id), "DualShock 4 pareado no Jogador 1") == 0);
    printf("[PASS] PAR-02 sem oficial: DS4 grava jogador 1, frase e INI\n");
}

static void test_par_01_pair_player2(void) {
    rig_t r;
    rig_init(&r, false);
    r.m.official = true;
    start_pairing(&r, 0);
    select_ds4(&r, 1 * S);
    run_pair_handshake(&r, 2 * S);

    const bt_paired_device_t *d = bt_manager_find_paired(&r.ipc, ADDR_A);
    assert(d && d->player == 2u);
    assert(r.s.phrase_id == BT_PHRASE_DS4_PAIRED_PLAYER2 && r.s.player == 2u);
    assert(r.m.notified[0] == (int)BT_PHRASE_DS4_PAIRED_PLAYER2);
    assert(strcmp(bt_phase1_phrase_text(r.s.phrase_id), "DualShock 4 pareado no Jogador 2") == 0);
    printf("[PASS] PAR-01 oficial no jogador 1: DS4 grava jogador 2\n");
}

static void test_pair_target_address(void) {
    rig_t r;
    rig_init(&r, false);
    bt_pair_session_adapter_ready(&r.s, 0);
    bt_pair_session_on_command(&r.s, BT_IPC_CMD_PAIR_AND_CONNECT, ADDR_B, 0);
    complete_setup(&r, 10);
    uint8_t e[32];
    feed(&r, e, evt_inquiry(e, ADDR_A, 0x002508u), 1 * S); /* DS4, mas nao o pedido */
    assert(count_op(&r.m, HCI_OP_CREATE_CONNECTION) == 0);
    feed(&r, e, evt_inquiry(e, ADDR_B, 0x002508u), 2 * S);
    const cmd_rec_t *cc = last_op(&r.m, HCI_OP_CREATE_CONNECTION);
    assert(cc && memcmp(cc->params, ADDR_B, 6) == 0);
    printf("[PASS] comando com endereco pareia so o endereco pedido\n");
}

static void test_pair_connection_failure_rescans(void) {
    rig_t r;
    rig_init(&r, false);
    start_pairing(&r, 0);
    select_ds4(&r, 1 * S);
    uint8_t e[32];
    feed(&r, e, evt_conn_complete(e, 0x04, 0, ADDR_A), 2 * S);
    assert(r.s.state == BT_PAIR_SCANNING && r.s.pair_active);
    assert(r.m.inq_start == 2);

    select_ds4(&r, 3 * S);
    feed(&r, e, evt_conn_complete(e, 0, 0x0040u, ADDR_A), 4 * S);
    feed(&r, e, evt_disconnect(e, 0x0040u, 0x13), 5 * S); /* cai antes de autenticar */
    assert(r.s.state == BT_PAIR_SCANNING && r.s.pair_active);
    assert(r.m.nnotified == 0);
    assert(r.s.phrase_id == BT_PHRASE_NONE);
    assert(r.m.inq_start == 3);
    assert(bt_manager_find_paired(&r.ipc, ADDR_A) == NULL);
    printf("[PASS] falha de conexao no pareamento volta ao inquiry sem frase\n");
}

static void test_pair_timeout_59_60_61(void) {
    rig_t r;
    rig_init(&r, false);
    start_pairing(&r, 5 * S);

    bt_pair_session_tick(&r.s, 5 * S + 59 * S);
    assert(r.s.pair_active && r.m.nnotified == 0 && r.s.phrase_id == BT_PHRASE_NONE);

    bt_pair_session_tick(&r.s, 5 * S + 60 * S);
    assert(!r.s.pair_active);
    assert(r.s.phrase_id == BT_PHRASE_DS4_PAIR_FAILED);
    assert(r.m.nnotified == 1 && r.m.notified[0] == (int)BT_PHRASE_DS4_PAIR_FAILED);
    assert(r.m.inq_stop == 1);
    assert(r.s.state == BT_PAIR_IDLE);
    assert(strcmp(bt_phase1_phrase_text(r.s.phrase_id), "Falha ao parear o DualShock 4.") == 0);
    assert(bt_manager_find_paired(&r.ipc, ADDR_A) == NULL);

    bt_pair_session_tick(&r.s, 5 * S + 61 * S); /* encerrada: sem frase repetida */
    assert(r.m.nnotified == 1);

    rig_init(&r, false);
    start_pairing(&r, 0);
    bt_pair_session_tick(&r.s, 61 * S);
    assert(r.s.phrase_id == BT_PHRASE_DS4_PAIR_FAILED && !r.s.pair_active);
    assert(r.m.nnotified == 1);
    printf("[PASS] timeout de pareamento em 59/60/61 s via bt_phase1_decide\n");
}

static void test_pair_timeout_during_connecting(void) {
    rig_t r;
    rig_init(&r, false);
    start_pairing(&r, 0);
    select_ds4(&r, 1 * S);
    bt_pair_session_tick(&r.s, 60 * S);
    assert(r.s.phrase_id == BT_PHRASE_DS4_PAIR_FAILED && !r.s.pair_active);
    assert(count_op(&r.m, HCI_OP_CREATE_CONN_CANCEL) == 1);

    rig_init(&r, false);
    start_pairing(&r, 0);
    select_ds4(&r, 1 * S);
    uint8_t e[32];
    feed(&r, e, evt_conn_complete(e, 0, 0x0042u, ADDR_A), 2 * S);
    bt_pair_session_tick(&r.s, 60 * S);
    const cmd_rec_t *d = last_op(&r.m, HCI_OP_DISCONNECT);
    assert(d && hci_get_le16(d->params) == 0x0042u);
    assert(r.s.phrase_id == BT_PHRASE_DS4_PAIR_FAILED);
    printf("[PASS] timeout cancela conexao pendente ou ACL sem autenticar\n");
}

/* ---- PAR-03 / LIM-01 ------------------------------------------------------------ */

static void test_par_03_second_ds4(void) {
    rig_t r;
    rig_init(&r, true);
    bt_pair_session_adapter_ready(&r.s, 0);
    int creates = count_op(&r.m, HCI_OP_CREATE_CONNECTION);

    /* O primeiro ainda esta conectando: um segundo endereco espera, sem apagar A. */
    bt_pair_session_on_command(&r.s, BT_IPC_CMD_PAIR_AND_CONNECT, ADDR_B, 1 * S);
    assert(r.m.inq_start == 0 && !r.s.pair_active);
    assert(r.m.nnotified == 0);
    const bt_paired_device_t *d = bt_manager_find_paired(&r.ipc, ADDR_A);
    assert(d && d->valid && memcmp(d->link_key, KEY_A, 16) == 0 && d->player == 1u);
    assert(bt_manager_find_paired(&r.ipc, ADDR_B) == NULL);
    assert(count_op(&r.m, HCI_OP_CREATE_CONNECTION) == creates);
    assert(r.m.persist == 0);

    bt_pair_session_on_command(&r.s, BT_IPC_CMD_PAIR_AND_CONNECT, NULL, 2 * S);
    assert(r.m.inq_start == 0 && !r.s.pair_active);
    printf("[PASS] PAR-03 segundo DS4 espera o radio livre e o primeiro permanece\n");
}

static void test_same_address_is_not_second_ds4(void) {
    rig_t r;
    rig_init(&r, true);
    bt_pair_session_adapter_ready(&r.s, 0);
    int creates = count_op(&r.m, HCI_OP_CREATE_CONNECTION);
    bt_pair_session_on_command(&r.s, BT_IPC_CMD_PAIR_AND_CONNECT, ADDR_A, 1 * S);
    assert(r.s.phrase_id == BT_PHRASE_NONE && r.m.nnotified == 0);
    assert(r.m.inq_start == 0 && !r.s.pair_active);
    assert(count_op(&r.m, HCI_OP_CREATE_CONNECTION) == creates); /* ja conectando */

    /* Idle apos falha: o mesmo endereco reconecta, sem inquiry. */
    uint8_t e[32];
    feed(&r, e, evt_conn_complete(e, 0x04, 0, ADDR_A), 2 * S);
    bt_pair_session_on_command(&r.s, BT_IPC_CMD_PAIR_AND_CONNECT, ADDR_A, 3 * S);
    assert(count_op(&r.m, HCI_OP_CREATE_CONNECTION) == creates + 1);
    assert(r.m.inq_start == 0 && r.m.nnotified == 0);
    printf("[PASS] endereco igual ao gravado nao e segundo controle\n");
}

static void test_incoming_connection_requests(void) {
    rig_t r;
    rig_init(&r, true);
    bt_pair_session_adapter_ready(&r.s, 0);
    uint8_t e[32];
    feed(&r, e, evt_conn_complete(e, 0x04, 0, ADDR_A), 1 * S);

    feed(&r, e, evt_conn_request(e, ADDR_A, 0x002508u), 2 * S);
    const cmd_rec_t *acc = last_op(&r.m, HCI_OP_ACCEPT_CONN_REQ);
    assert(acc && memcmp(acc->params, ADDR_A, 6) == 0 && acc->len == 7);
    assert(r.s.state == BT_PAIR_CONNECTING && r.m.nnotified == 0);
    feed(&r, e, evt_conn_complete(e, 0x04, 0, ADDR_A), 3 * S);

    feed(&r, e, evt_conn_request(e, ADDR_B, 0x002508u), 4 * S); /* segundo DS4 */
    const cmd_rec_t *rej = last_op(&r.m, HCI_OP_REJECT_CONN_REQ);
    assert(rej && memcmp(rej->params, ADDR_B, 6) == 0 && rej->len == 7);
    assert(r.m.nnotified == 1 && r.m.notified[0] == (int)BT_PHRASE_DS4_ALREADY_PAIRED);
    feed(&r, e, evt_conn_request(e, ADDR_B, 0x002508u), 5 * S); /* insistencia: sem spam */
    assert(count_op(&r.m, HCI_OP_REJECT_CONN_REQ) == 2);
    assert(r.m.nnotified == 1);
    feed(&r, e, evt_conn_request(e, ADDR_B, 0x002508u), 4 * S + BT_PAIR_SECOND_DS4_MS);
    assert(r.m.nnotified == 2);

    static const uint8_t phone[6] = { 7, 7, 7, 7, 7, 7 };
    feed(&r, e, evt_conn_request(e, phone, 0x5A020Cu), 20 * S); /* nao e DS4 */
    assert(count_op(&r.m, HCI_OP_REJECT_CONN_REQ) == 4);
    assert(r.m.nnotified == 2);
    assert(bt_manager_find_paired(&r.ipc, ADDR_B) == NULL);
    printf("[PASS] conexoes recebidas: gravado aceita, segundo DS4 recusado com frase\n");
}

/* ---- ERR-03 ---------------------------------------------------------------------- */

static void test_err_03_disconnect_mid_session(void) {
    rig_t r;
    rig_init(&r, true);
    bt_pair_session_adapter_ready(&r.s, 0);
    uint8_t e[32];
    feed(&r, e, evt_conn_complete(e, 0, 0x0050u, ADDR_A), 1 * S);
    feed(&r, e, evt_addr_only(e, HCI_EVT_LINK_KEY_REQUEST, ADDR_A, 0), 1 * S);
    feed(&r, e, evt_auth_complete(e, 0, 0x0050u), 1 * S);
    feed(&r, e, evt_encrypt(e, 0, 0x0050u, 1), 1 * S);
    assert(r.s.state == BT_PAIR_CONNECTED);
    r.s.phrase_id = BT_PHRASE_DS4_PAIRED_PLAYER1; /* frase antiga que a queda substitui */
    r.s.player = 1u;

    feed(&r, e, evt_disconnect(e, 0x0099u, 0x08), 5 * S); /* outro handle: ignorado */
    assert(r.m.nnotified == 0);

    feed(&r, e, evt_disconnect(e, 0x0050u, 0x08), 10 * S);
    assert(r.s.phrase_id == BT_PHRASE_DS4_DISCONNECTED && r.s.player == 0u);
    assert(r.m.nnotified == 1 && r.m.notified[0] == (int)BT_PHRASE_DS4_DISCONNECTED);
    assert(strcmp(bt_phase1_phrase_text(r.s.phrase_id), "DualShock 4 desconectado.") == 0);
    assert(!r.s.pair_active && !r.s.established);
    assert(r.s.state == BT_PAIR_IDLE);
    assert(bt_manager_find_paired(&r.ipc, ADDR_A) != NULL); /* gravado permanece */

    /* Tenta reconectar sem repetir o aviso. */
    int creates = count_op(&r.m, HCI_OP_CREATE_CONNECTION);
    bt_pair_session_tick(&r.s, 10 * S + BT_PAIR_RECONNECT_RETRY_MS);
    assert(count_op(&r.m, HCI_OP_CREATE_CONNECTION) == creates + 1);
    feed(&r, e, evt_conn_complete(e, 0x04, 0, ADDR_A), 20 * S);
    assert(r.m.nnotified == 1);
    printf("[PASS] ERR-03 queda do DS4 no meio da sessao publica frase e reconecta\n");
}

static void test_adapter_lost_resets_session(void) {
    rig_t r;
    rig_init(&r, true);
    bt_pair_session_adapter_ready(&r.s, 0);
    uint8_t e[32];
    feed(&r, e, evt_conn_complete(e, 0, 0x0050u, ADDR_A), 1 * S);
    feed(&r, e, evt_auth_complete(e, 0, 0x0050u), 1 * S);
    r.s.phrase_id = BT_PHRASE_DS4_PAIRED_PLAYER1;
    bt_pair_session_adapter_lost(&r.s);
    assert(r.s.state == BT_PAIR_IDLE && !r.s.established && !r.s.adapter_ready);
    assert(r.s.phrase_id == BT_PHRASE_NONE && r.m.nnotified == 0);
    /* Eventos tardios sem adaptador nao fazem nada. */
    int n = r.m.ncmds;
    feed(&r, e, evt_disconnect(e, 0x0050u, 0x08), 2 * S);
    bt_pair_session_tick(&r.s, 60 * S);
    assert(r.m.ncmds == n && r.m.nnotified == 0);
    printf("[PASS] adaptador removido zera a sessao sem frase de DS4\n");
}

static void test_scan_releases_connected_ds4(void) {
    rig_t r;
    rig_init(&r, true);
    bt_pair_session_adapter_ready(&r.s, 0);
    uint8_t e[32];
    feed(&r, e, evt_conn_complete(e, 0, 0x0030u, ADDR_A), 100);
    feed(&r, e, evt_auth_complete(e, 0, 0x0030u), 120);
    feed(&r, e, evt_encrypt(e, 0, 0x0030u, 1), 130);
    assert(r.s.state == BT_PAIR_CONNECTED && r.m.inq_start == 0);

    bt_pair_session_on_command(&r.s, BT_IPC_CMD_START_SCAN, NULL, 200);
    assert(r.m.inq_start == 0);
    assert(count_op(&r.m, HCI_OP_DISCONNECT) == 1);
    assert(r.m.nnotified == 0);

    feed(&r, e, evt_disconnect(e, 0x0030u, 0x13), 300);
    assert(r.s.state == BT_PAIR_IDLE);
    assert(r.m.inq_start == 1);
    assert(r.m.nnotified == 0); /* pausa da busca não é queda do controle */
    bt_pair_session_tick(&r.s, 300 + BT_PAIR_RECONNECT_RETRY_MS);
    assert(count_op(&r.m, HCI_OP_CREATE_CONNECTION) == 1);

    bt_pair_session_on_command(&r.s, BT_IPC_CMD_STOP_SCAN, NULL, 400);
    assert(r.m.inq_stop == 1);
    assert(count_op(&r.m, HCI_OP_CREATE_CONNECTION) == 2);
    printf("[PASS] Y com o DS4 ligado solta o link, busca, e o stop religa\n");
}

static void test_scan_waits_until_both_links_drop(void) {
    rig_t r;
    rig_init(&r, true);
    bt_pair_session_adapter_ready(&r.s, 0);
    uint8_t e[32];
    feed(&r, e, evt_conn_complete(e, 0, 0x0030u, ADDR_A), 100);
    feed(&r, e, evt_auth_complete(e, 0, 0x0030u), 110);
    feed(&r, e, evt_encrypt(e, 0, 0x0030u, 1), 120);
    r.s.second.active = true;
    r.s.second.established = true;
    r.s.second.state = BT_PAIR_CONNECTED;
    r.s.second.acl_handle = 0x0031u;
    memcpy(r.s.second.peer, ADDR_B, 6u);

    bt_pair_session_on_command(&r.s, BT_IPC_CMD_START_SCAN, NULL, 200);
    assert(r.m.inq_start == 0);
    assert(count_op(&r.m, HCI_OP_DISCONNECT) == 2);

    feed(&r, e, evt_disconnect(e, 0x0030u, 0x13), 300);
    assert(r.m.inq_start == 0);
    feed(&r, e, evt_disconnect(e, 0x0031u, 0x13), 400);
    assert(r.m.inq_start == 1);
    printf("[PASS] a busca espera os dois links cairem\n");
}

static void test_pair_command_ignored_without_adapter(void) {
    rig_t r;
    rig_init(&r, false);
    bt_pair_session_on_command(&r.s, BT_IPC_CMD_PAIR_AND_CONNECT, NULL, 0);
    assert(!r.s.pair_active && r.m.inq_start == 0 && r.m.ncmds == 0);
    bt_pair_session_adapter_ready(&r.s, 0);
    bt_pair_session_on_command(&r.s, BT_IPC_CMD_CONNECT_PAIRED, NULL, 0);
    complete_setup(&r, 1);
    assert(r.m.inq_start == 0 && !r.s.pair_active);
    printf("[PASS] comando de parear ignorado sem adaptador; outros comandos nao abrem inquiry\n");
}

static void test_page_timeout_releases_handle(void) {
    rig_t r;
    uint8_t e[32];
    rig_init(&r, true);
    bt_pair_session_adapter_ready(&r.s, 0);
    feed(&r, e, evt_conn_complete(e, 0x04, 0, ADDR_A), 500);
    assert(count_op(&r.m, HCI_OP_DISCONNECT) == 0);
    feed(&r, e, evt_conn_complete(e, 0x04, 0x0002u, ADDR_A), 1000);
    assert(count_op(&r.m, HCI_OP_DISCONNECT) == 0);
    assert(r.s.orphan_handle == 0x0002u);
    assert(r.s.state == BT_PAIR_IDLE);
    bt_pair_session_tick(&r.s, 1000 + BT_PAIR_LISTEN_MS);
    assert(r.s.state == BT_PAIR_CONNECTING);
    uint8_t st[6] = { HCI_EVT_COMMAND_STATUS, 4, 0x0B, 1, 0, 0 };
    hci_put_le16(st + 4, HCI_OP_CREATE_CONNECTION);
    bt_pair_session_on_event(&r.s, st, sizeof st, 1000 + BT_PAIR_LISTEN_MS);
    const cmd_rec_t *d = last_op(&r.m, HCI_OP_DISCONNECT);
    assert(d && d->len >= 2 && d->params[0] == 0x02 && d->params[1] == 0x00);
    assert(r.s.state == BT_PAIR_IDLE);
    const int discs = count_op(&r.m, HCI_OP_DISCONNECT);
    feed(&r, e, evt_conn_complete(e, 0x08, 0x0003u, ADDR_A), 50000);
    assert(count_op(&r.m, HCI_OP_DISCONNECT) == discs);
    assert(r.s.orphan_handle == 0x0003u);
    printf("[PASS] timeout de pagina guarda o handle; a recusa 0x0B solta\n");
}

static void test_two_stored_stay_connected(void) {
    rig_t r;
    static const uint8_t key_b[16] = { 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9 };
    uint8_t e[32];
    rig_init(&r, true);
    assert(bt_manager_store_single_ds4(&r.ipc, ADDR_B, key_b, 5u, 2u, "DS4B") >= 0);
    bt_pair_session_adapter_ready(&r.s, 0);
    feed(&r, e, evt_conn_complete(e, 0, 0x0030u, ADDR_A), 100);
    feed(&r, e, evt_auth_complete(e, 0, 0x0030u), 110);
    feed(&r, e, evt_encrypt(e, 0, 0x0030u, 1), 120);
    assert(r.s.state == BT_PAIR_CONNECTED && r.s.established && r.s.acl_handle == 0x0030u);
    assert(r.s.second.state == BT_PAIR_CONNECTING);
    const cmd_rec_t *page = last_op(&r.m, HCI_OP_CREATE_CONNECTION);
    assert(page && memcmp(page->params, ADDR_B, 6) == 0);

    feed(&r, e, evt_conn_complete(e, 0, 0x0031u, ADDR_B), 200);
    feed(&r, e, evt_auth_complete(e, 0, 0x0031u), 210);
    assert(!r.s.second.established);
    assert(count_op(&r.m, HCI_OP_SET_CONN_ENCRYPT) == 0);
    feed(&r, e, evt_encrypt(e, 0, 0x0031u, 1), 220);
    assert(r.s.second.established && r.s.second.player == 2u && r.s.second.acl_handle == 0x0031u);
    assert(r.s.established && r.s.acl_handle == 0x0030u);
    assert(count_op(&r.m, HCI_OP_DISCONNECT) == 0);

    static const uint8_t phone[6] = { 7, 7, 7, 7, 7, 7 };
    feed(&r, e, evt_conn_request(e, phone, 0x5A020Cu), 300);
    const cmd_rec_t *rej = last_op(&r.m, HCI_OP_REJECT_CONN_REQ);
    assert(rej && memcmp(rej->params, phone, 6) == 0);
    assert(r.s.acl_handle == 0x0030u && r.s.second.acl_handle == 0x0031u);
    printf("[PASS] dois DS4 gravados ficam ligados ao mesmo tempo\n");
}

static void test_page_timeout_tries_the_other_stored(void) {
    rig_t r;
    static const uint8_t key_b[16] = { 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9 };
    uint8_t e[32];
    rig_init(&r, true);
    assert(bt_manager_store_single_ds4(&r.ipc, ADDR_B, key_b, 5u, 2u, "DS4B") >= 0);
    bt_pair_session_adapter_ready(&r.s, 0);
    const cmd_rec_t *first = last_op(&r.m, HCI_OP_CREATE_CONNECTION);
    assert(first && memcmp(first->params, ADDR_A, 6) == 0);
    feed(&r, e, evt_conn_complete(e, 0x04, 0, ADDR_A), 10 * S);
    bt_pair_session_tick(&r.s, 10 * S + BT_PAIR_LISTEN_MS);
    const cmd_rec_t *next = last_op(&r.m, HCI_OP_CREATE_CONNECTION);
    assert(next && memcmp(next->params, ADDR_B, 6) == 0);
    printf("[PASS] timeout de pagina chama o outro controle gravado\n");
}

static void test_page_timeout_does_not_drop_the_incoming(void) {
    rig_t r;
    static const uint8_t key_b[16] = { 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9 };
    uint8_t e[32];
    rig_init(&r, true);
    assert(bt_manager_store_single_ds4(&r.ipc, ADDR_B, key_b, 5u, 2u, "DS4B") >= 0);
    bt_pair_session_adapter_ready(&r.s, 0);
    feed(&r, e, evt_conn_complete(e, 0x04, 0x0001u, ADDR_A), 1000);
    feed(&r, e, evt_conn_request(e, ADDR_B, 0x000508u), 1010);
    const cmd_rec_t *acc = last_op(&r.m, HCI_OP_ACCEPT_CONN_REQ);
    assert(acc && memcmp(acc->params, ADDR_B, 6) == 0);
    assert(count_op(&r.m, HCI_OP_DISCONNECT) == 0);
    feed(&r, e, evt_conn_complete(e, 0, 0x0001u, ADDR_B), 1020);
    assert(r.s.second.state == BT_PAIR_AUTH && r.s.second.acl_handle == 0x0001u);
    assert(r.s.orphan_handle != 0x0001u);
    assert(count_op(&r.m, HCI_OP_DISCONNECT) == 0);
    printf("[PASS] timeout de pagina nao derruba o controle que entra\n");
}

static void test_stored_controller_calling_in_is_not_already_paired(void) {
    rig_t r;
    static const uint8_t key_b[16] = { 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9 };
    uint8_t e[32];
    rig_init(&r, true);
    assert(bt_manager_store_single_ds4(&r.ipc, ADDR_B, key_b, 5u, 2u, "DS4B") >= 0);
    bt_pair_session_adapter_ready(&r.s, 0);
    feed(&r, e, evt_conn_complete(e, 0x04, 0, ADDR_A), 1000);
    bt_pair_session_tick(&r.s, 1000 + BT_PAIR_LISTEN_MS);
    feed(&r, e, evt_conn_complete(e, 0x04, 0, ADDR_B), 2000 + BT_PAIR_LISTEN_MS);
    feed(&r, e, evt_conn_request(e, ADDR_B, 0x002508u), 3000 + BT_PAIR_LISTEN_MS);
    const cmd_rec_t *acc = last_op(&r.m, HCI_OP_ACCEPT_CONN_REQ);
    assert(acc && memcmp(acc->params, ADDR_B, 6) == 0);
    assert(count_op(&r.m, HCI_OP_REJECT_CONN_REQ) == 0);
    assert(r.m.nnotified == 0);
    feed(&r, e, evt_conn_complete(e, 0, 0x0002u, ADDR_B), 4000 + BT_PAIR_LISTEN_MS);
    feed(&r, e, evt_auth_complete(e, 0, 0x0002u), 4001 + BT_PAIR_LISTEN_MS);
    const cmd_rec_t *enc = last_op(&r.m, HCI_OP_SET_CONN_ENCRYPT);
    assert(enc && enc->params[2] == 0x01u);
    assert(count_op(&r.m, HCI_OP_DISCONNECT) == 0);
    printf("[PASS] controle gravado que liga sozinho entra sem aviso de ja pareado\n");
}

static void test_retry_skips_the_controller_already_up(void) {
    rig_t r;
    static const uint8_t key_b[16] = { 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9 };
    uint8_t e[32];
    rig_init(&r, true);
    assert(bt_manager_store_single_ds4(&r.ipc, ADDR_B, key_b, 5u, 2u, "DS4B") >= 0);
    bt_pair_session_adapter_ready(&r.s, 0);
    feed(&r, e, evt_conn_complete(e, 0x04, 0, ADDR_A), 1000);
    r.s.second.established = true;
    r.s.second.active = true;
    r.s.second.state = BT_PAIR_CONNECTED;
    memcpy(r.s.second.peer, ADDR_B, 6u);
    r.s.stored_turn = 1;
    bt_pair_session_tick(&r.s, 1000 + BT_PAIR_LISTEN_MS);
    const cmd_rec_t *page = last_op(&r.m, HCI_OP_CREATE_CONNECTION);
    assert(page && memcmp(page->params, ADDR_A, 6) == 0);
    printf("[PASS] a nova tentativa nao chama o controle que ja esta ligado\n");
}

int main(void) {
    test_par_05_reconnect_without_inquiry();
    test_link_key_reply_with_stored_key();
    test_auth_disallowed_keeps_link();
    test_reconnect_wait_not_blocking_on_mailbox();
    test_inquiry_only_after_pair_command();
    test_ds4_candidate_filter();
    test_par_02_pair_player1();
    test_par_01_pair_player2();
    test_pair_target_address();
    test_pair_connection_failure_rescans();
    test_pair_timeout_59_60_61();
    test_pair_timeout_during_connecting();
    test_par_03_second_ds4();
    test_same_address_is_not_second_ds4();
    test_incoming_connection_requests();
    test_page_timeout_releases_handle();
    test_two_stored_stay_connected();
    test_page_timeout_tries_the_other_stored();
    test_page_timeout_does_not_drop_the_incoming();
    test_stored_controller_calling_in_is_not_already_paired();
    test_retry_skips_the_controller_already_up();
    test_err_03_disconnect_mid_session();
    test_adapter_lost_resets_session();
    test_scan_releases_connected_ds4();
    test_scan_waits_until_both_links_drop();
    test_pair_command_ignored_without_adapter();
    return 0;
}
