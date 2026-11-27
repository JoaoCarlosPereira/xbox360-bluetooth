#include "bt_pair_session.h"

#include <string.h>

#define ACL_NONE            0xFFFFu
#define HCI_REASON_USER_END 0x13u /* Remote User Terminated Connection */
#define HCI_REASON_LOCAL    0x16u /* Connection Terminated by Local Host */
#define HCI_REASON_BAD_ADDR 0x0Fu /* Unacceptable BD_ADDR */
#define DS4_COD_MASK        0x1FFCu
#define DS4_COD_GAMEPAD     0x0508u /* periferico (5) / gamepad (0x02 << 2) */

static void note(bt_pair_session_t *s, const char *what, uint32_t value) {
    if (s->ops.log) s->ops.log(s->ops.ctx, what, value);
}

static bool addr_eq(const uint8_t *a, const uint8_t *b) {
    return memcmp(a, b, 6u) == 0;
}

static void send_cmd(bt_pair_session_t *s, uint16_t opcode, const uint8_t *params, uint8_t len) {
    if (s->ops.send_cmd && s->ops.send_cmd(s->ops.ctx, opcode, params, len) != 0) {
        note(s, "send_cmd failed", opcode);
    }
}

const uint8_t *bt_pair_session_stored_addr(const bt_pair_session_t *s) {
    if (!s || !s->ipc) return NULL;
    for (size_t i = 0; i < BT_MAX_PAIRED_DEVICES; i++) {
        if (s->ipc->paired[i].valid) return s->ipc->paired[i].bd_addr;
    }
    return NULL;
}

static int stored_count(const bt_pair_session_t *s) {
    return bt_manager_paired_count(s && s->ipc ? s->ipc : NULL);
}

static uint8_t first_free_player(const bt_ipc_state_t *ipc) {
    if (!ipc) return 0u;
    for (uint8_t p = 1u; p <= 4u; p++) {
        bool used = false;
        for (size_t i = 0; i < BT_MAX_PAIRED_DEVICES; i++) {
            if (ipc->paired[i].valid && ipc->paired[i].player == p) used = true;
        }
        if (!used) return p;
    }
    return 0u;
}

static uint32_t phrase_for_player(uint8_t player) {
    if (player == 1u) return BT_PHRASE_DS4_PAIRED_PLAYER1;
    if (player == 2u) return BT_PHRASE_DS4_PAIRED_PLAYER2;
    if (player == 3u) return BT_PHRASE_DS4_PAIRED_PLAYER3;
    if (player == 4u) return BT_PHRASE_DS4_PAIRED_PLAYER4;
    return BT_PHRASE_NONE;
}

static bool contains(const char *text, const char *needle) {
    for (size_t i = 0; text[i]; i++) {
        size_t j = 0;
        while (needle[j] && text[i + j] == needle[j]) j++;
        if (!needle[j]) return true;
    }
    return false;
}

bool bt_pair_is_ds4_candidate(const hci_inquiry_item_t *item) {
    if (!item) return false;
    return (item->class_of_device & DS4_COD_MASK) == DS4_COD_GAMEPAD ||
           contains(item->name, "Wireless Controller");
}

void bt_pair_session_init(bt_pair_session_t *s, bt_ipc_state_t *ipc, const bt_pair_ops_t *ops) {
    if (!s) return;
    memset(s, 0, sizeof(*s));
    s->ipc = ipc;
    if (ops) s->ops = *ops;
    s->acl_handle = ACL_NONE;
    s->second.acl_handle = ACL_NONE;
}

static void publish(bt_pair_session_t *s, uint32_t phrase_id, uint8_t player) {
    s->phrase_id = phrase_id;
    s->player = player;
    note(s, "phrase", phrase_id);
    if (s->ops.notify) s->ops.notify(s->ops.ctx, phrase_id);
}

static void clear_link(bt_pair_session_t *s) {
    s->acl_handle = ACL_NONE;
    s->established = false;
    s->encrypted = false;
    s->key_valid = false;
    s->incoming = false;
}

static void second_clear(bt_pair_session_t *s) {
    memset(&s->second, 0, sizeof s->second);
    s->second.acl_handle = ACL_NONE;
}

static const bt_paired_device_t *other_stored(const bt_pair_session_t *s) {
    if (!s->ipc) return NULL;
    for (size_t i = 0; i < BT_MAX_PAIRED_DEVICES; i++) {
        const bt_paired_device_t *d = &s->ipc->paired[i];
        if (!d->valid) continue;
        if (s->has_peer && addr_eq(d->bd_addr, s->peer)) continue;
        return d;
    }
    return NULL;
}

static void send_create_to(bt_pair_session_t *s, const uint8_t *addr) {
    uint8_t cp[13];
    memcpy(cp, addr, 6u);
    hci_put_le16(cp + 6, 0xCC18u);
    cp[8] = 0x01u;
    cp[9] = 0x00;
    hci_put_le16(cp + 10, 0);
    cp[12] = 0x01;
    send_cmd(s, HCI_OP_CREATE_CONNECTION, cp, sizeof cp);
}

/* O primeiro já está no ar. O segundo gravado entra sem derrubar o primeiro. */
static void page_second(bt_pair_session_t *s) {
    const bt_paired_device_t *other;
    if (!s->adapter_ready || s->pair_active || s->user_held || s->scan_hold) return;
    if (s->state != BT_PAIR_CONNECTED || !s->established) return;
    if (s->second.established || s->second.state == BT_PAIR_CONNECTING ||
        s->second.state == BT_PAIR_AUTH)
        return;
    other = other_stored(s);
    if (!other) return;
    s->second.active = true;
    s->second.incoming = false;
    s->second.established = false;
    s->second.encrypted = false;
    s->second.retry_armed = false;
    s->second.acl_handle = ACL_NONE;
    memcpy(s->second.peer, other->bd_addr, 6u);
    s->second.player = (other->player >= 1u && other->player <= 4u) ? other->player : 0u;
    s->second.state = BT_PAIR_CONNECTING;
    send_create_to(s, s->second.peer);
    note(s, "connect second", s->second.peer[0]);
}

static bool links_down(const bt_pair_session_t *s) {
    const bool primary = s->state == BT_PAIR_IDLE && s->acl_handle == ACL_NONE;
    const bool second = !s->second.active && s->second.acl_handle == ACL_NONE &&
                        s->second.state != BT_PAIR_CONNECTING &&
                        s->second.state != BT_PAIR_AUTH &&
                        s->second.state != BT_PAIR_CONNECTED;
    return primary && second;
}

static void begin_held_scan(bt_pair_session_t *s) {
    if (!s->scan_hold || !links_down(s)) return;
    s->scan_hold = false;
    if (s->ops.start_inquiry)
        s->ops.start_inquiry(s->ops.ctx, BT_PAIR_INQUIRY_UNITS);
}

static void second_fail(bt_pair_session_t *s, uint64_t now_ms, uint8_t status) {
    const bool again = !s->user_held && !s->pair_active && !s->scan_hold && other_stored(s) != NULL;
    /* Timeout de pagina prende o radio por 10 s. Esperar mais deixa o controle
     * que ja esta ligado entregar os botoes entre as tentativas. */
    const uint32_t wait = (status == 0x04u || status == 0x08u)
                              ? BT_PAIR_LISTEN_MS : BT_PAIR_RECONNECT_RETRY_MS;
    second_clear(s);
    if (!again) {
        begin_held_scan(s);
        return;
    }
    s->second.retry_armed = true;
    s->second.retry_at_ms = now_ms + wait;
}

static void second_establish(bt_pair_session_t *s) {
    if (s->second.player < 1u || s->second.player > 4u) {
        const bt_paired_device_t *known = bt_manager_find_paired(s->ipc, s->second.peer);
        if (known && known->player >= 1u && known->player <= 4u)
            s->second.player = known->player;
    }
    s->second.established = true;
    s->second.state = BT_PAIR_CONNECTED;
    s->second.retry_armed = false;
    note(s, "second up", s->second.player);
}

static void accept_second(bt_pair_session_t *s, const uint8_t *addr) {
    const bt_paired_device_t *known = bt_manager_find_paired(s->ipc, addr);
    uint8_t reply[7];
    second_clear(s);
    s->second.active = true;
    s->second.incoming = true;
    s->second.state = BT_PAIR_CONNECTING;
    memcpy(s->second.peer, addr, 6u);
    if (known && known->player >= 1u && known->player <= 4u) s->second.player = known->player;
    memcpy(reply, addr, 6u);
    reply[6] = 0x00;
    send_cmd(s, HCI_OP_ACCEPT_CONN_REQ, reply, sizeof reply);
    note(s, "accept second", addr[0]);
}

static void send_create_connection(bt_pair_session_t *s) {
    uint8_t cp[13];
    memcpy(cp, s->peer, 6u);
    hci_put_le16(cp + 6, 0xCC18u); /* tipos de pacote DM/DH 1,3,5 */
    cp[8] = s->peer_psr;
    cp[9] = 0x00;
    hci_put_le16(cp + 10, s->peer_clk);
    cp[12] = 0x01; /* permite troca de papel */
    send_cmd(s, HCI_OP_CREATE_CONNECTION, cp, sizeof cp);
}

/* O primeiro da lista que não responde não pode esconder os outros. */
static bool addr_is_live(const bt_pair_session_t *s, const uint8_t *addr) {
    if (!addr) return false;
    if (s->established && s->has_peer && addr_eq(addr, s->peer)) return true;
    if (s->second.established && addr_eq(addr, s->second.peer)) return true;
    if ((s->second.state == BT_PAIR_CONNECTING || s->second.state == BT_PAIR_AUTH) &&
        addr_eq(addr, s->second.peer))
        return true;
    return false;
}

static const uint8_t *stored_by_turn(const bt_pair_session_t *s) {
    int count = 0;
    int seen = 0;
    int want;
    if (!s || !s->ipc) return NULL;
    for (size_t i = 0; i < BT_MAX_PAIRED_DEVICES; i++) {
        const bt_paired_device_t *d = &s->ipc->paired[i];
        if (d->valid && !addr_is_live(s, d->bd_addr)) count++;
    }
    if (count <= 0) return NULL;
    want = (int)(s->stored_turn % (uint8_t)count);
    for (size_t i = 0; i < BT_MAX_PAIRED_DEVICES; i++) {
        const bt_paired_device_t *d = &s->ipc->paired[i];
        if (!d->valid || addr_is_live(s, d->bd_addr)) continue;
        if (seen == want) return d->bd_addr;
        seen++;
    }
    return NULL;
}

static void connect_stored(bt_pair_session_t *s) {
    const uint8_t *stored = stored_by_turn(s);
    if (!stored) return;
    clear_link(s);
    memcpy(s->peer, stored, 6u);
    s->has_peer = true;
    const bt_paired_device_t *known = bt_manager_find_paired(s->ipc, stored);
    if (known && known->player >= 1u && known->player <= 4u) s->player = known->player;
    s->peer_psr = 0x01u; /* R1: sem inquiry nao ha modo de page scan medido */
    s->peer_clk = 0u;
    s->retry_armed = false;
    s->state = BT_PAIR_CONNECTING;
    send_create_connection(s);
    note(s, "connect stored", stored[0]);
}

static void arm_retry(bt_pair_session_t *s, uint64_t now_ms) {
    if (s->user_held) return;
    if (!bt_pair_session_stored_addr(s)) return;
    s->retry_armed = true;
    s->retry_at_ms = now_ms + BT_PAIR_RECONNECT_RETRY_MS;
}

static void maybe_start_inquiry(bt_pair_session_t *s) {
    if (!s->want_inquiry || !s->pair_active || s->setup_pending != 0) return;
    if (s->ops.start_inquiry &&
        s->ops.start_inquiry(s->ops.ctx, BT_PAIR_INQUIRY_UNITS) == 0) {
        s->want_inquiry = false;
        note(s, "inquiry started", 0);
    } else {
        note(s, "inquiry start deferred", 0); /* tentado de novo no tick */
    }
}

/* Conexao ou autenticacao falhou antes de a sessao se estabelecer. */
static void link_failed(bt_pair_session_t *s, uint64_t now_ms, uint8_t status) {
    clear_link(s);
    if (s->pair_active) {
        s->has_peer = false;
        s->state = BT_PAIR_SCANNING;
        s->want_inquiry = true;
        maybe_start_inquiry(s); /* o limite de 60 s e checado no tick */
    } else {
        s->state = BT_PAIR_IDLE;
        s->has_peer = false;
        if (s->scan_hold) {
            begin_held_scan(s);
            return;
        }
        /* 0x04 página, 0x08 conexão: o controle não atendeu. O próximo é o outro gravado.
         * A espera longa deixa o rádio em escuta: o controle liga sozinho nesse intervalo. */
        if (status == 0x04u || status == 0x08u) {
            s->stored_turn++;
            if (!s->user_held && bt_pair_session_stored_addr(s)) {
                s->retry_armed = true;
                s->retry_at_ms = now_ms + BT_PAIR_LISTEN_MS;
            }
        } else {
            arm_retry(s, now_ms);
        }
    }
}

static void disconnect_acl(bt_pair_session_t *s, uint16_t handle) {
    uint8_t p[3];
    hci_put_le16(p, handle);
    p[2] = HCI_REASON_USER_END;
    send_cmd(s, HCI_OP_DISCONNECT, p, sizeof p);
}

/* O RTL8761 devolve Connection Complete com erro e um handle real. Sem o
 * Disconnect, o Create Connection seguinte volta 0x0B (ACL já existe). */
static void release_orphan(bt_pair_session_t *s, uint16_t handle) {
    if (handle == 0 || handle == ACL_NONE) return;
    if (s->established && handle == s->acl_handle) return;
    if (s->second.established && handle == s->second.acl_handle) return;
    s->orphan_handle = handle;
    disconnect_acl(s, handle);
    note(s, "release handle", handle);
}

static void select_candidate(bt_pair_session_t *s, const hci_inquiry_item_t *item) {
    clear_link(s);
    memcpy(s->peer, item->bd_addr, 6u);
    s->has_peer = true;
    s->peer_psr = item->page_scan_rep_mode;
    s->peer_clk = (uint16_t)(item->clock_offset | 0x8000u);
    s->want_inquiry = false;
    if (s->ops.stop_inquiry) s->ops.stop_inquiry(s->ops.ctx);
    s->state = BT_PAIR_CONNECTING;
    send_create_connection(s);
    note(s, "candidate", item->bd_addr[0]);
}

void bt_pair_session_connect_known(bt_pair_session_t *s, const hci_inquiry_item_t *item,
                                   uint64_t now_ms) {
    if (!s || !item || !s->adapter_ready) return;
    if (bt_pair_session_stored_addr(s)) {
        bt_pair_session_on_command(s, BT_IPC_CMD_PAIR_AND_CONNECT, item->bd_addr, now_ms);
        return;
    }
    if (s->pair_active) return;
    s->pair_active = true;
    s->pair_start_ms = now_ms;
    s->phrase_id = BT_PHRASE_NONE;
    s->player = 0u;
    s->has_target = true;
    memcpy(s->target, item->bd_addr, 6u);
    select_candidate(s, item);
}

static void on_inquiry(bt_pair_session_t *s, const uint8_t *evt, size_t len) {
    if (!s->pair_active || s->state != BT_PAIR_SCANNING) return;
    hci_inquiry_item_t items[8];
    int n = hci_decode_inquiry_event(evt, len, items, 8);
    for (int i = 0; i < n; i++) {
        if (!bt_pair_is_ds4_candidate(&items[i])) continue;
        if (s->has_target && !addr_eq(items[i].bd_addr, s->target)) continue;
        select_candidate(s, &items[i]);
        return;
    }
}

/* Pareamento concluido (autenticacao OK): grava jogador e chave. */
static bool finish_pairing(bt_pair_session_t *s) {
    const uint8_t *stored = bt_pair_session_stored_addr(s);
    const bt_phase1_context_t ctx = {
        .event = BT_PHASE1_EV_PAIR_ATTEMPT,
        .official_on_player1 = s->ops.official_on_player1 &&
                               s->ops.official_on_player1(s->ops.ctx),
        .ds4_paired = stored != NULL,
        .candidate_bd_addr = s->peer,
        .stored_bd_addr = stored,
        .paired_count = (uint32_t)stored_count(s),
    };
    const bt_phase1_decision_t d = bt_phase1_decide(&ctx);
    if (!d.accept_pair) {
        if (d.phrase_id != BT_PHRASE_NONE) publish(s, d.phrase_id, d.player);
        return false;
    }
    uint8_t player = d.player;
    uint32_t phrase = d.phrase_id;
    if (player < 1u || player > 4u) {
        player = first_free_player(s->ipc);
        phrase = phrase_for_player(player);
    }
    if (player < 1u || player > 4u) {
        publish(s, BT_PHRASE_DS4_ALREADY_PAIRED, 0u);
        return false;
    }
    const int idx = bt_manager_store_single_ds4(s->ipc, s->peer, s->key, s->key_type,
                                                player, "DualShock 4");
    if (idx < 0) {
        note(s, "store failed", (uint32_t)idx);
        if (idx == -3) publish(s, BT_PHRASE_DS4_ALREADY_PAIRED, 0u);
        return false;
    }
    if (s->ops.persist) s->ops.persist(s->ops.ctx, s->ipc);
    s->pair_active = false;
    s->want_inquiry = false;
    publish(s, phrase, player);
    return true;
}

static void end_pairing_by_timeout(bt_pair_session_t *s, const bt_phase1_decision_t *d) {
    if (s->state == BT_PAIR_SCANNING) {
        if (s->ops.stop_inquiry) s->ops.stop_inquiry(s->ops.ctx);
    } else if (s->state == BT_PAIR_CONNECTING && s->has_peer && !s->incoming) {
        send_cmd(s, HCI_OP_CREATE_CONN_CANCEL, s->peer, 6u);
    } else if (s->state == BT_PAIR_AUTH && s->acl_handle != ACL_NONE) {
        disconnect_acl(s, s->acl_handle);
    }
    s->want_inquiry = false;
    s->pair_active = false;
    s->has_peer = false;
    s->state = BT_PAIR_IDLE;
    clear_link(s);
    publish(s, d->phrase_id, d->player);
}

static void second_ds4_notice(bt_pair_session_t *s, const uint8_t *addr, uint64_t now_ms) {
    if (s->second_notified && now_ms - s->last_second_ms < BT_PAIR_SECOND_DS4_MS) return;
    const bt_phase1_context_t ctx = {
        .event = BT_PHASE1_EV_PAIR_ATTEMPT,
        .ds4_paired = true,
        .candidate_bd_addr = addr,
        .stored_bd_addr = bt_pair_session_stored_addr(s),
    };
    const bt_phase1_decision_t d = bt_phase1_decide(&ctx);
    if (d.phrase_id == BT_PHRASE_NONE) return;
    s->second_notified = true;
    s->last_second_ms = now_ms;
    publish(s, d.phrase_id, d.player);
}

void bt_pair_session_adapter_ready(bt_pair_session_t *s, uint64_t now_ms) {
    (void)now_ms;
    if (!s) return;
    s->state = BT_PAIR_IDLE;
    s->pair_active = false;
    s->want_inquiry = false;
    s->has_peer = false;
    s->retry_armed = false;
    clear_link(s);
    second_clear(s);
    s->adapter_ready = true;
    s->setup_pending = BT_PAIR_SETUP_ALL;

    static const uint8_t event_mask[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x3F };
    static const uint8_t ssp_on = 0x01;
    static const uint8_t page_timeout[2] = { 0x00, 0x40 }; /* 0x4000 * 0,625 ms = 10,24 s */
    static const uint8_t page_scan_only = 0x02;
    send_cmd(s, HCI_OP_SET_EVENT_MASK, event_mask, sizeof event_mask);
    send_cmd(s, HCI_OP_WRITE_SIMPLE_PAIRING, &ssp_on, 1u);
    send_cmd(s, HCI_OP_WRITE_PAGE_TIMEOUT, page_timeout, sizeof page_timeout);
    send_cmd(s, HCI_OP_WRITE_SCAN_ENABLE, &page_scan_only, 1u);

    /* PAR-05: DS4 gravado conecta no ato, sem inquiry e sem o gerenciador. */
    connect_stored(s);
}

void bt_pair_session_adapter_lost(bt_pair_session_t *s) {
    if (!s) return;
    s->adapter_ready = false;
    s->setup_pending = 0;
    s->state = BT_PAIR_IDLE;
    s->pair_active = false;
    s->want_inquiry = false;
    s->has_peer = false;
    s->retry_armed = false;
    s->phrase_id = BT_PHRASE_NONE;
    s->player = 0u;
    s->orphan_handle = ACL_NONE;
    clear_link(s);
    second_clear(s);
}

void bt_pair_session_on_command(bt_pair_session_t *s, uint32_t command,
                                const uint8_t *addr, uint64_t now_ms) {
    if (!s || !s->adapter_ready) {
        if (s && s->ops.log) s->ops.log(s->ops.ctx, "command ignored", command);
        return;
    }

    /* Busca visivel: inquiry sem parear o primeiro DS4. A lista fica no transporte. */
    if (command == BT_IPC_CMD_STOP_SCAN) {
        s->scan_hold = false;
        if (s->ops.stop_inquiry) s->ops.stop_inquiry(s->ops.ctx);
        if (s->state == BT_PAIR_IDLE) {
            s->user_held = false;
            connect_stored(s);
        } else {
            /* O Disconnect ainda não voltou. A queda reagenda o controle gravado. */
            s->user_held = false;
        }
        return;
    }
    if (command == BT_IPC_CMD_START_SCAN) {
        if (s->pair_active) return;
        /* O rádio é um ACL. Com o DS4 ligado a busca não sai; solta e procura. */
        s->user_held = true;
        if (links_down(s)) {
            if (s->ops.start_inquiry)
                s->ops.start_inquiry(s->ops.ctx, BT_PAIR_INQUIRY_UNITS);
            return;
        }
        s->scan_hold = true;
        if (s->state == BT_PAIR_CONNECTING && s->has_peer)
            send_cmd(s, HCI_OP_CREATE_CONN_CANCEL, s->peer, 6u);
        else if (s->acl_handle != ACL_NONE)
            disconnect_acl(s, s->acl_handle);
        if (s->second.state == BT_PAIR_CONNECTING && !s->second.incoming)
            send_cmd(s, HCI_OP_CREATE_CONN_CANCEL, s->second.peer, 6u);
        else if (s->second.acl_handle != ACL_NONE)
            disconnect_acl(s, s->second.acl_handle);
        s->second.retry_armed = false;
        return;
    }

    if (command == BT_IPC_CMD_DISCONNECT_SLOT) {
        if (addr && s->second.active && addr_eq(addr, s->second.peer)) {
            s->second.retry_armed = false;
            if (s->second.acl_handle != ACL_NONE) disconnect_acl(s, s->second.acl_handle);
            else second_clear(s);
            return;
        }
        s->user_held = true;
        s->retry_armed = false;
        if (s->acl_handle != ACL_NONE) disconnect_acl(s, s->acl_handle);
        return;
    }
    if (command == BT_IPC_CMD_FORGET_PAIRED && addr) {
        const bool ours = s->has_peer && addr_eq(addr, s->peer);
        const bool second = s->second.active && addr_eq(addr, s->second.peer);
        bt_manager_forget_paired(s->ipc, addr);
        if (s->ops.persist) s->ops.persist(s->ops.ctx, s->ipc);
        if (second) {
            s->second.retry_armed = false;
            if (s->second.acl_handle != ACL_NONE) disconnect_acl(s, s->second.acl_handle);
            else second_clear(s);
        }
        if (ours) {
            s->user_held = true;
            s->retry_armed = false;
            if (s->acl_handle != ACL_NONE) disconnect_acl(s, s->acl_handle);
        }
        return;
    }
    if (command == BT_IPC_CMD_CONNECT_PAIRED) {
        if (s->state == BT_PAIR_IDLE && !s->pair_active) {
            s->user_held = false;
            if (addr && bt_manager_find_paired(s->ipc, addr)) {
                clear_link(s);
                memcpy(s->peer, addr, 6u);
                s->has_peer = true;
                s->peer_psr = 0x01u;
                s->peer_clk = 0u;
                s->retry_armed = false;
                s->state = BT_PAIR_CONNECTING;
                const bt_paired_device_t *known = bt_manager_find_paired(s->ipc, addr);
                if (known && known->player >= 1u && known->player <= 4u)
                    s->player = known->player;
                send_create_connection(s);
                note(s, "connect chosen", addr[0]);
            } else {
                connect_stored(s);
            }
        }
        return;
    }
    if (command != BT_IPC_CMD_PAIR_AND_CONNECT) return;

    const uint8_t *stored = bt_pair_session_stored_addr(s);
    if (stored) {
        const bt_phase1_context_t ctx = {
            .event = BT_PHASE1_EV_PAIR_ATTEMPT,
            .ds4_paired = true,
            .candidate_bd_addr = addr,
            .stored_bd_addr = stored,
            .paired_count = (uint32_t)stored_count(s),
        };
        const bt_phase1_decision_t d = bt_phase1_decide(&ctx);
        const bool extra = d.accept_pair && addr && !addr_eq(addr, stored) &&
                           s->state == BT_PAIR_IDLE && !s->pair_active;
        if (!extra) {
            if (d.phrase_id != BT_PHRASE_NONE) {
                publish(s, d.phrase_id, d.player);
            } else if (s->state == BT_PAIR_IDLE) {
                s->user_held = false;
                connect_stored(s); /* mesmo endereco: so reconecta */
            }
            return;
        }
        /* Ainda cabe um controle e o radio esta livre: pareia o endereco novo. */
    }

    if (s->pair_active) return;
    s->user_held = false;
    s->pair_active = true;
    s->pair_start_ms = now_ms;
    s->phrase_id = BT_PHRASE_NONE;
    s->player = 0u;
    s->has_peer = false;
    s->has_target = addr != NULL;
    if (addr) memcpy(s->target, addr, 6u);
    clear_link(s);
    s->state = BT_PAIR_SCANNING;
    s->want_inquiry = true;
    maybe_start_inquiry(s);
}

static void on_command_complete(bt_pair_session_t *s, const uint8_t *evt, size_t len) {
    hci_cmd_complete_t cc;
    if (hci_parse_command_complete(evt, len, &cc) != 0) return;
    switch (cc.opcode) {
    case HCI_OP_SET_EVENT_MASK:       s->setup_pending &= (uint8_t)~BT_PAIR_SETUP_EVENT_MASK; break;
    case HCI_OP_WRITE_SIMPLE_PAIRING: s->setup_pending &= (uint8_t)~BT_PAIR_SETUP_SSP;        break;
    case HCI_OP_WRITE_PAGE_TIMEOUT:   s->setup_pending &= (uint8_t)~BT_PAIR_SETUP_PAGE_TO;    break;
    case HCI_OP_WRITE_SCAN_ENABLE:    s->setup_pending &= (uint8_t)~BT_PAIR_SETUP_SCAN;       break;
    default: return;
    }
    if (s->setup_pending == 0) maybe_start_inquiry(s);
}

static void on_connection_request(bt_pair_session_t *s, const uint8_t *p, uint64_t now_ms) {
    const uint8_t *addr = p;
    const uint32_t cod = (uint32_t)p[6] | ((uint32_t)p[7] << 8) | ((uint32_t)p[8] << 16);
    const bool ds4_like = (cod & DS4_COD_MASK) == DS4_COD_GAMEPAD;
    const uint8_t *stored = bt_pair_session_stored_addr(s);
    uint8_t reply[7];
    memcpy(reply, addr, 6u);

    const bool known = bt_manager_find_paired(s->ipc, addr) != NULL;
    const bool primary_this = s->has_peer && addr_eq(addr, s->peer);
    bool accept = false;
    /* O gravado que estamos chamando também pode ligar sozinho. Recusar isso
     * publica "já existe um DualShock 4" e o controle desiste. */
    if (known && primary_this &&
        (s->state == BT_PAIR_IDLE || s->state == BT_PAIR_CONNECTING)) {
        accept = true;
    } else if (stored && addr_eq(addr, stored) &&
               (s->state == BT_PAIR_IDLE || s->state == BT_PAIR_CONNECTING)) {
        accept = true;
    } else if (s->pair_active && s->state == BT_PAIR_SCANNING && ds4_like &&
               (!s->has_target || addr_eq(addr, s->target)) &&
               stored_count(s) < 4) {
        accept = true;
        if (s->ops.stop_inquiry) s->ops.stop_inquiry(s->ops.ctx);
    } else if (!s->pair_active && known && !primary_this &&
               !s->second.established && s->second.state != BT_PAIR_CONNECTING &&
               s->second.state != BT_PAIR_AUTH) {
        accept_second(s, addr);
        return;
    } else if (!known && stored && ds4_like) {
        second_ds4_notice(s, addr, now_ms);
    }

    if (!accept) {
        reply[6] = HCI_REASON_BAD_ADDR;
        send_cmd(s, HCI_OP_REJECT_CONN_REQ, reply, sizeof reply);
        return;
    }
    clear_link(s);
    memcpy(s->peer, addr, 6u);
    s->has_peer = true;
    s->incoming = true;
    s->peer_psr = 0x01u;
    s->peer_clk = 0u;
    s->retry_armed = false;
    s->want_inquiry = false;
    s->state = BT_PAIR_CONNECTING;
    reply[6] = 0x00; /* assume o papel de master */
    send_cmd(s, HCI_OP_ACCEPT_CONN_REQ, reply, sizeof reply);
}

static void on_connection_complete(bt_pair_session_t *s, const uint8_t *p, uint64_t now_ms) {
    const uint8_t status = p[0];
    const uint16_t handle = (uint16_t)(hci_get_le16(p + 1) & 0x0FFFu);
    const uint8_t *addr = p + 3;
    if (p[9] != 0x01u) return; /* so ACL */
    /* 0x04 esgota a página, 0x08 estoura a conexão e 0x16 é o nosso próprio
     * Disconnect. Cortar o handle nesse instante derruba o controle que entra
     * e reaproveita o número. O handle fica anotado e só sai no 0x0B. */
    if (status == 0x00u && handle == s->orphan_handle)
        s->orphan_handle = ACL_NONE;
    else if (status != 0x00u && status != 0x04u && status != 0x08u && status != 0x16u)
        release_orphan(s, handle);
    else if (status != 0x00u && handle != 0 && handle != ACL_NONE)
        s->orphan_handle = handle;

    if (s->second.active && addr_eq(addr, s->second.peer) &&
        s->second.state == BT_PAIR_CONNECTING) {
        if (status != 0x00u) {
            note(s, "second connect failed", status);
            second_fail(s, now_ms, status);
            return;
        }
        s->second.acl_handle = handle;
        s->second.state = BT_PAIR_AUTH;
        uint8_t auth2[2];
        hci_put_le16(auth2, handle);
        send_cmd(s, HCI_OP_AUTH_REQUESTED, auth2, sizeof auth2);
        return;
    }
    if (!s->has_peer || !addr_eq(addr, s->peer) || s->state != BT_PAIR_CONNECTING) {
        if (status == 0x00u) disconnect_acl(s, handle); /* conexao que nao e nossa */
        return;
    }
    if (status != 0x00u) {
        if (s->incoming) return; /* a página antiga; o controle já entrou */
        note(s, "connect failed", status);
        link_failed(s, now_ms, status);
        return;
    }
    s->acl_handle = handle;
    s->state = BT_PAIR_AUTH;
    uint8_t auth[2];
    hci_put_le16(auth, handle);
    send_cmd(s, HCI_OP_AUTH_REQUESTED, auth, sizeof auth);
}

static void on_link_key_request(bt_pair_session_t *s, const uint8_t *addr) {
    uint8_t buf[HCI_MAX_CMD_SIZE];
    const size_t n = bt_manager_build_link_key_reply(s->ipc, addr, buf, sizeof buf);
    if (n > HCI_CMD_HDR_SIZE) {
        send_cmd(s, HCI_OP_LINK_KEY_REQ_REPLY, buf + HCI_CMD_HDR_SIZE,
                 (uint8_t)(n - HCI_CMD_HDR_SIZE));
    } else {
        send_cmd(s, HCI_OP_LINK_KEY_REQ_NEG_REPLY, addr, 6u);
    }
}

static bool pairing_with(const bt_pair_session_t *s, const uint8_t *addr) {
    return s->pair_active && s->has_peer && addr_eq(addr, s->peer) &&
           (s->state == BT_PAIR_CONNECTING || s->state == BT_PAIR_AUTH);
}

/* Conclui a sessão. ask_encrypt só quando o rádio ainda não cifrou sozinho. */
static void establish_link(bt_pair_session_t *s, uint64_t now_ms, bool ask_encrypt) {
    if (s->pair_active && (!s->key_valid || !finish_pairing(s))) {
        note(s, "pairing rejected", s->key_valid);
        disconnect_acl(s, s->acl_handle);
        link_failed(s, now_ms, 0);
        return;
    }
    s->established = true;
    s->state = BT_PAIR_CONNECTED;
    /* Reconexão não passa por finish_pairing; o jogador fica o que foi gravado. */
    if (s->player < 1u || s->player > 4u) {
        const bt_paired_device_t *stored = bt_manager_find_paired(s->ipc, s->peer);
        if (stored && stored->player >= 1u && stored->player <= 4u)
            s->player = stored->player;
    }
    if (ask_encrypt && !s->encrypted) {
        uint8_t enc[3];
        hci_put_le16(enc, s->acl_handle);
        enc[2] = 0x01;
        send_cmd(s, HCI_OP_SET_CONN_ENCRYPT, enc, sizeof enc);
    }
    page_second(s);
}

static void on_auth_complete(bt_pair_session_t *s, const uint8_t *p, uint64_t now_ms) {
    const uint8_t status = p[0];
    const uint16_t handle = (uint16_t)(hci_get_le16(p + 1) & 0x0FFFu);
    if (s->second.state == BT_PAIR_AUTH && handle == s->second.acl_handle) {
        if (status == 0x0Cu || status == 0x00u) {
            note(s, status == 0x0Cu ? "second auth deferred" : "second auth ok", status);
            if (s->second.encrypted) second_establish(s);
            else if (status == 0x00u && s->second.incoming) {
                /* Quem entra não cifra sozinho. Sem este pedido o DS4 cai com 0x13. */
                uint8_t enc[3];
                hci_put_le16(enc, handle);
                enc[2] = 0x01u;
                send_cmd(s, HCI_OP_SET_CONN_ENCRYPT, enc, sizeof enc);
            }
            return;
        }
        note(s, "second auth failed", status);
        disconnect_acl(s, handle);
        second_fail(s, now_ms, status);
        return;
    }
    if (s->state != BT_PAIR_AUTH || handle != s->acl_handle) return;

    /* 0x0C: o comando cruzou com a autenticação que o rádio já fazia.
     * 0x00 na reconexão: a cifragem chega sozinha. Pedir Set Connection Encryption
     * nesse instante faz o DS4 cair com reason 0x13. O pareamento novo ainda pede. */
    if (status == 0x0Cu || (status == 0x00u && !s->pair_active)) {
        note(s, status == 0x0Cu ? "auth deferred" : "auth ok", status);
        if (s->encrypted) establish_link(s, now_ms, false);
        return;
    }
    if (status != 0x00u) {
        note(s, "auth failed", status);
        disconnect_acl(s, handle);
        link_failed(s, now_ms, status);
        return;
    }
    establish_link(s, now_ms, true);
}

static void on_encryption_change(bt_pair_session_t *s, const uint8_t *p, uint64_t now_ms) {
    const uint8_t status = p[0];
    const uint16_t handle = (uint16_t)(hci_get_le16(p + 1) & 0x0FFFu);
    if (handle == s->second.acl_handle && s->second.active) {
        s->second.encrypted = status == 0x00u && p[3] != 0;
        note(s, "second encrypt", status);
        if (s->second.encrypted && s->second.state == BT_PAIR_AUTH) second_establish(s);
        return;
    }
    if (handle != s->acl_handle) return;
    s->encrypted = status == 0x00u && p[3] != 0;
    note(s, "encrypt", status);
    if (s->encrypted && s->state == BT_PAIR_AUTH && !(s->pair_active && !s->key_valid))
        establish_link(s, now_ms, false);
}

static void on_disconnection_complete(bt_pair_session_t *s, const uint8_t *p, uint64_t now_ms) {
    const uint16_t handle = (uint16_t)(hci_get_le16(p + 1) & 0x0FFFu);
    const uint8_t reason = p[3];
    if (s->second.active && s->second.acl_handle != ACL_NONE && handle == s->second.acl_handle) {
        note(s, "second down", reason);
        second_fail(s, now_ms, reason);
        return;
    }
    if (s->acl_handle == ACL_NONE || handle != s->acl_handle) return;

    if (s->state == BT_PAIR_CONNECTED && s->established) {
        const bool for_scan = s->scan_hold;
        clear_link(s);
        s->state = BT_PAIR_IDLE;
        if (!for_scan && reason != HCI_REASON_LOCAL) {
            const bt_phase1_context_t ctx = { .event = BT_PHASE1_EV_DS4_DISCONNECT };
            const bt_phase1_decision_t d = bt_phase1_decide(&ctx);
            publish(s, d.phrase_id, d.player);
        }
        if (for_scan) {
            begin_held_scan(s);
            return;
        }
        arm_retry(s, now_ms);
        return;
    }
    link_failed(s, now_ms, 0); /* caiu antes de autenticar */
}

void bt_pair_session_on_event(bt_pair_session_t *s, const uint8_t *evt, size_t len,
                              uint64_t now_ms) {
    if (!s || !evt || !s->adapter_ready || len < HCI_EVT_HDR_SIZE) return;
    const uint8_t code = evt[0];
    const size_t plen = evt[1];
    if (len < HCI_EVT_HDR_SIZE + plen) return;
    const uint8_t *p = evt + HCI_EVT_HDR_SIZE;

    switch (code) {
    case HCI_EVT_COMMAND_COMPLETE:
        on_command_complete(s, evt, len);
        break;
    case HCI_EVT_COMMAND_STATUS: {
        hci_cmd_status_t cs;
        if (hci_parse_command_status(evt, len, &cs) == 0 &&
            cs.opcode == HCI_OP_CREATE_CONNECTION && cs.status != 0x00u) {
            if (cs.status == 0x0Bu) release_orphan(s, s->orphan_handle);
            if (s->state == BT_PAIR_CONNECTING) {
                note(s, "create connection refused", cs.status);
                link_failed(s, now_ms, cs.status);
            } else if (s->second.state == BT_PAIR_CONNECTING) {
                note(s, "second create refused", cs.status);
                second_fail(s, now_ms, cs.status);
            }
        }
        break;
    }
    case HCI_EVT_INQUIRY_RESULT:
    case HCI_EVT_INQUIRY_RESULT_RSSI:
    case HCI_EVT_EXTENDED_INQUIRY_RESULT:
        on_inquiry(s, evt, len);
        break;
    case HCI_EVT_INQUIRY_COMPLETE:
        if (s->pair_active && s->state == BT_PAIR_SCANNING) {
            s->want_inquiry = true;
            maybe_start_inquiry(s);
        }
        break;
    case HCI_EVT_CONNECTION_REQUEST:
        if (plen >= 10u) on_connection_request(s, p, now_ms);
        break;
    case HCI_EVT_CONNECTION_COMPLETE:
        if (plen >= 11u) on_connection_complete(s, p, now_ms);
        break;
    case HCI_EVT_LINK_KEY_REQUEST:
        if (plen >= 6u) on_link_key_request(s, p);
        break;
    case HCI_EVT_IO_CAPABILITY_REQUEST:
        if (plen >= 6u && pairing_with(s, p)) {
            uint8_t rep[9];
            memcpy(rep, p, 6u);
            rep[6] = 0x03; /* NoInputNoOutput */
            rep[7] = 0x00; /* sem OOB */
            rep[8] = 0x04; /* dedicated bonding, sem MITM */
            send_cmd(s, HCI_OP_IO_CAP_REQ_REPLY, rep, sizeof rep);
        }
        break;
    case HCI_EVT_USER_CONFIRM_REQUEST:
        if (plen >= 6u && pairing_with(s, p)) {
            send_cmd(s, HCI_OP_USER_CONFIRM_REQ_REPLY, p, 6u);
        }
        break;
    case HCI_EVT_LINK_KEY_NOTIFICATION:
        if (plen >= 23u && pairing_with(s, p)) {
            memcpy(s->key, p + 6, 16u);
            s->key_type = p[22];
            s->key_valid = true;
        }
        break;
    case HCI_EVT_AUTH_COMPLETE:
        if (plen >= 3u) on_auth_complete(s, p, now_ms);
        break;
    case HCI_EVT_ENCRYPTION_CHANGE:
        if (plen >= 4u) on_encryption_change(s, p, now_ms);
        break;
    case HCI_EVT_DISCONNECTION_COMPLETE:
        if (plen >= 4u) on_disconnection_complete(s, p, now_ms);
        break;
    default:
        break;
    }
}

void bt_pair_session_tick(bt_pair_session_t *s, uint64_t now_ms) {
    if (!s || !s->adapter_ready) return;

    if (s->pair_active) {
        const uint64_t elapsed_ms = now_ms > s->pair_start_ms ? now_ms - s->pair_start_ms : 0u;
        const uint64_t sec = elapsed_ms / 1000u;
        const bt_phase1_context_t ctx = {
            .event = BT_PHASE1_EV_PAIR_TICK,
            .pair_elapsed_sec = sec > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)sec,
        };
        const bt_phase1_decision_t d = bt_phase1_decide(&ctx);
        if (d.phrase_id == BT_PHRASE_DS4_PAIR_FAILED) {
            end_pairing_by_timeout(s, &d);
            return;
        }
        maybe_start_inquiry(s);
    }

    if (s->retry_armed && !s->pair_active && s->state == BT_PAIR_IDLE &&
        now_ms >= s->retry_at_ms) {
        connect_stored(s);
    }
    if (s->second.retry_armed && now_ms >= s->second.retry_at_ms) page_second(s);
}
