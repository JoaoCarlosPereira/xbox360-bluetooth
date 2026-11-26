#include "x360_pad_inject.h"

/* Escrita do estado publicado: seq impar enquanto muda, par quando coerente. */
static void begin_write(x360_pad_inject_t *p) {
    __atomic_store_n(&p->seq, p->seq + 1u, __ATOMIC_RELEASE);
}

static void end_write(x360_pad_inject_t *p) {
    __atomic_store_n(&p->seq, p->seq + 1u, __ATOMIC_RELEASE);
}

static void clear_slot(x360_pad_inject_t *p, uint8_t player) {
    const x360_gamepad_state_t zero = { 0 };
    if (p->sink.write_slot) p->sink.write_slot(p->sink.ctx, (uint8_t)(player - 1u), &zero);
}

void x360_pad_inject_init(x360_pad_inject_t *p, const x360_pad_sink_t *sink) {
    if (!p) return;
    p->sink.ctx = NULL;
    p->sink.write_slot = NULL;
    p->sink.send_report = NULL;
    if (sink) p->sink = *sink;
    p->seq = 0u;
    p->active = false;
    p->player = 0u;
    p->packet = 0u;
    p->last = (x360_gamepad_state_t){ 0 };
}

bool x360_pad_inject_publish(x360_pad_inject_t *p, bool authenticated, uint8_t player,
                             const x360_gamepad_state_t *state, bool ps_pressed) {
    if (!p || !state || !p->sink.write_slot) return false;
    if (!authenticated || player < X360_PAD_PLAYER_MIN || player > X360_PAD_PLAYER_MAX)
        return false;

    /* Jogador mudou (ex.: oficial saiu): libera o slot antigo antes de ocupar o novo. */
    if (p->active && p->player != player) clear_slot(p, p->player);

    x360_gamepad_state_t out = *state;
    if (ps_pressed) out.buttons = (uint16_t)(out.buttons | X360_BTN_GUIDE);

    p->sink.write_slot(p->sink.ctx, (uint8_t)(player - 1u), &out);
    begin_write(p);
    p->active = true;
    p->player = player;
    p->packet++;
    p->last = out;
    end_write(p);
    return true;
}

void x360_pad_inject_drop(x360_pad_inject_t *p) {
    if (!p || !p->active) return;
    clear_slot(p, p->player);
    begin_write(p);
    p->active = false;
    p->player = 0u;
    p->packet++;
    p->last = (x360_gamepad_state_t){ 0 };
    end_write(p);
}

x360_pad_snapshot_t x360_pad_inject_snapshot(const x360_pad_inject_t *p) {
    x360_pad_snapshot_t s = { 0 };
    if (!p) return s;
    for (unsigned spin = 0; spin < 8u; spin++) {
        const uint32_t before = __atomic_load_n(&p->seq, __ATOMIC_ACQUIRE);
        if (before & 1u) continue;
        s.active = p->active;
        s.player = p->player;
        s.packet = p->packet;
        s.state = p->last;
        if (__atomic_load_n(&p->seq, __ATOMIC_ACQUIRE) == before) return s;
    }
    return s;
}

size_t x360_pad_inject_rumble(x360_pad_inject_t *p, uint8_t small_motor, uint8_t large_motor) {
    if (!p || !p->sink.send_report) return 0u;
    if (!x360_pad_inject_snapshot(p).active) return 0u;
    uint8_t report[DS4_BT_OUTPUT_REPORT_LEN];
    const size_t n = ds4_bt_build_rumble_report(report, sizeof report, small_motor, large_motor);
    if (n == 0u) return 0u;
    p->sink.send_report(p->sink.ctx, report, n);
    return n;
}

uint32_t x360_pad_inject_normalize_user(uint32_t user) {
    return (user & 0xFFu) == 0xFFu ? 0u : user;
}

/* true quando user e o do DS4 ativo; snap recebe a copia coerente. */
static bool owns_user(const x360_pad_inject_t *p, uint32_t user, x360_pad_snapshot_t *snap) {
    *snap = x360_pad_inject_snapshot(p);
    return snap->active && snap->player >= X360_PAD_PLAYER_MIN &&
           user == (uint32_t)(snap->player - 1u);
}

uint32_t x360_pad_inject_filter_capabilities(const x360_pad_inject_t *p, uint32_t user,
                                             uint32_t original_status, x360_pad_caps_t *caps) {
    user = x360_pad_inject_normalize_user(user);
    x360_pad_snapshot_t snap;
    if (!caps || original_status != X360_PAD_ERROR_DEVICE_NOT_CONNECTED ||
        !owns_user(p, user, &snap))
        return original_status;

    caps->type = X360_PAD_DEVTYPE_GAMEPAD;
    caps->subtype = X360_PAD_DEVSUBTYPE_GAMEPAD;
    caps->flags = 0u;
    caps->gamepad = snap.state;
    caps->vibration_left = 0u;
    caps->vibration_right = 0u;
    return X360_PAD_ERROR_SUCCESS;
}

uint32_t x360_pad_inject_filter_set_state(x360_pad_inject_t *p, uint32_t user,
                                          uint32_t original_status, uint16_t left_motor,
                                          uint16_t right_motor) {
    user = x360_pad_inject_normalize_user(user);
    x360_pad_snapshot_t snap;
    if (original_status != X360_PAD_ERROR_DEVICE_NOT_CONNECTED || !owns_user(p, user, &snap))
        return original_status;

    /* Esquerdo = motor grande, direito = pequeno; o DS4 recebe 8 bits. */
    (void)x360_pad_inject_rumble(p, (uint8_t)(right_motor >> 8), (uint8_t)(left_motor >> 8));
    return X360_PAD_ERROR_SUCCESS;
}

uint32_t x360_pad_inject_filter_read_state(const x360_pad_inject_t *p, bool context_is_ds4,
                                           uint32_t original_status, uint32_t *packet_number,
                                           x360_gamepad_state_t *gamepad) {
    if (!context_is_ds4 || !gamepad) return original_status;
    const x360_pad_snapshot_t snap = x360_pad_inject_snapshot(p);
    if (!snap.active) return original_status;

    *gamepad = snap.state;
    if (packet_number) *packet_number = snap.packet;
    return X360_PAD_ERROR_SUCCESS;
}

/* Bit do botão Xbox -> VK_PAD_*. O guia (0x0400) não vira tecla. */
static const uint16_t k_pad_vk[16] = {
    0x5810u, 0x5811u, 0x5812u, 0x5813u, /* cima, baixo, esquerda, direita */
    0x5814u, 0x5815u, 0x5816u, 0x5817u, /* Start, Back, L3, R3 */
    0x5805u, 0x5804u, 0u,      0u,      /* LB, RB */
    0x5800u, 0x5801u, 0x5802u, 0x5803u  /* A, B, X, Y */
};

static uint16_t guide_nav_bits(const x360_gamepad_state_t *state) {
    uint16_t bits = (uint16_t)(state->buttons & (uint16_t)~X360_BTN_GUIDE);
    if (bits & (X360_BTN_DPAD_UP | X360_BTN_DPAD_DOWN | X360_BTN_DPAD_LEFT | X360_BTN_DPAD_RIGHT))
        return bits;
    if (state->thumb_ly > 20000) bits = (uint16_t)(bits | X360_BTN_DPAD_UP);
    else if (state->thumb_ly < -20000) bits = (uint16_t)(bits | X360_BTN_DPAD_DOWN);
    if (state->thumb_lx > 20000) bits = (uint16_t)(bits | X360_BTN_DPAD_RIGHT);
    else if (state->thumb_lx < -20000) bits = (uint16_t)(bits | X360_BTN_DPAD_LEFT);
    return bits;
}

static int lowest_set(uint16_t mask) {
    for (int bit = 0; bit < 16; bit++) {
        if (mask & (uint16_t)(1u << bit)) return bit;
    }
    return -1;
}

static uint32_t emit_key(x360_keystroke_t *out, int bit, uint16_t flags, uint8_t user) {
    if (bit < 0 || bit > 15 || k_pad_vk[bit] == 0u) return X360_PAD_ERROR_EMPTY;
    out->virtual_key = k_pad_vk[bit];
    out->unicode = 0u;
    out->flags = flags;
    out->user_index = user;
    out->hid_code = 0u;
    return X360_PAD_ERROR_SUCCESS;
}

uint32_t x360_pad_keystroke_next(x360_key_tracker_t *tracker, const x360_gamepad_state_t *state,
                                 uint8_t user_index, uint32_t now_ms, x360_keystroke_t *out) {
    if (!tracker || !state || !out) return X360_PAD_ERROR_EMPTY;
    const uint16_t cur = guide_nav_bits(state);
    tracker->pending_down = (uint16_t)(tracker->pending_down | (cur & (uint16_t)~tracker->level));
    tracker->pending_up = (uint16_t)(tracker->pending_up | (tracker->level & (uint16_t)~cur));
    tracker->level = cur;

    int bit = lowest_set(tracker->pending_up);
    if (bit >= 0) {
        tracker->pending_up = (uint16_t)(tracker->pending_up & (uint16_t)~(1u << bit));
        if (tracker->repeat_bit == (uint16_t)(1u << bit)) tracker->repeat_bit = 0u;
        return emit_key(out, bit, X360_KEY_UP, user_index);
    }
    bit = lowest_set(tracker->pending_down);
    if (bit >= 0) {
        tracker->pending_down = (uint16_t)(tracker->pending_down & (uint16_t)~(1u << bit));
        tracker->repeat_bit = (uint16_t)(1u << bit);
        tracker->next_repeat = now_ms + 450u;
        return emit_key(out, bit, X360_KEY_DOWN, user_index);
    }
    if (tracker->repeat_bit && (tracker->level & tracker->repeat_bit) &&
        now_ms >= tracker->next_repeat) {
        tracker->next_repeat = now_ms + 140u;
        bit = lowest_set(tracker->repeat_bit);
        return emit_key(out, bit, (uint16_t)(X360_KEY_DOWN | X360_KEY_REPEAT), user_index);
    }
    return X360_PAD_ERROR_EMPTY;
}

x360_pad_bind_verdict_t x360_pad_inject_check_bind(uint8_t decided_player, uint8_t returned_user) {
    if (decided_player >= X360_PAD_PLAYER_MIN && decided_player <= X360_PAD_PLAYER_MAX &&
        returned_user == (uint8_t)(decided_player - 1u))
        return X360_PAD_BIND_OK;
    /* Jogador 2, 3 ou 4 no usuario 0 tomou o slot do controle oficial. */
    if (decided_player > 1u && returned_user == 0u)
        return X360_PAD_BIND_OFFICIAL_SLOT;
    return X360_PAD_BIND_MISMATCH;
}
