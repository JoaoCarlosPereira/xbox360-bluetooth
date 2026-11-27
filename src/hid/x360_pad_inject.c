#include "x360_pad_inject.h"

#include <string.h>

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
    p->button_level = 0u;
    p->button_release = 0u;
}

/* Mesma zona do controle oficial. A Aurora rola a capa com o analógico.
 * Sair do centro exige um pouco mais, para o polegar no direcional não
 * atravessar a zona a cada quadro. */
#define X360_PAD_LEFT_DEADZONE  7849
#define X360_PAD_RIGHT_DEADZONE 8689
#define X360_PAD_STICK_GATE     4096
#define X360_PAD_TRIGGER_FLOOR  30
#define X360_PAD_DPAD_MASK      0x000Fu
#define X360_PAD_DPAD_RELEASE   3u
#define X360_PAD_AXIS_HOLD      1536
#define X360_PAD_TRIGGER_HOLD   12

bool x360_pad_in_use(const x360_gamepad_state_t *state) {
    if (!state) return false;
    if (state->buttons != 0u) return true;
    if (state->left_trigger >= X360_PAD_TRIGGER_FLOOR ||
        state->right_trigger >= X360_PAD_TRIGGER_FLOOR)
        return true;
    if (state->thumb_lx > X360_PAD_LEFT_DEADZONE || state->thumb_lx < -X360_PAD_LEFT_DEADZONE)
        return true;
    if (state->thumb_ly > X360_PAD_LEFT_DEADZONE || state->thumb_ly < -X360_PAD_LEFT_DEADZONE)
        return true;
    if (state->thumb_rx > X360_PAD_RIGHT_DEADZONE || state->thumb_rx < -X360_PAD_RIGHT_DEADZONE)
        return true;
    if (state->thumb_ry > X360_PAD_RIGHT_DEADZONE || state->thumb_ry < -X360_PAD_RIGHT_DEADZONE)
        return true;
    return false;
}

static uint32_t isqrt_u64(uint64_t n) {
    uint64_t r = 0;
    uint64_t bit = 1ull << 62;
    while (bit > n) bit >>= 2;
    while (bit != 0u) {
        if (n >= r + bit) {
            n -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)r;
}

/* Centro vira zero. Fora da zona, o curso restante ocupa o eixo inteiro.
 * Com o eixo já em zero, a entrada só acontece depois da zona mais a folga. */
static void calm_stick(int16_t *x, int16_t *y, int16_t prev_x, int16_t prev_y, int32_t dead) {
    const int64_t lx = *x;
    const int64_t ly = *y;
    const uint64_t mag2 = (uint64_t)(lx * lx + ly * ly);
    const uint32_t mag = isqrt_u64(mag2);
    int32_t gate = dead;
    int32_t neu;
    if (prev_x == 0 && prev_y == 0) gate = dead + X360_PAD_STICK_GATE;
    if (mag <= (uint32_t)gate) {
        *x = 0;
        *y = 0;
        return;
    }
    if (mag >= 32767u) return;
    neu = (int32_t)(((int64_t)(mag - (uint32_t)dead) * 32767) / (32767 - dead));
    *x = (int16_t)(lx * neu / (int32_t)mag);
    *y = (int16_t)(ly * neu / (int32_t)mag);
}

static uint8_t calm_trigger(uint8_t v) {
    return v < X360_PAD_TRIGGER_FLOOR ? 0u : v;
}

static int16_t hold_near(int16_t v, int16_t prev, int32_t tol) {
    int32_t d = (int32_t)v - (int32_t)prev;
    if (d < 0) d = -d;
    return d <= tol ? prev : v;
}

static uint8_t hold_trigger(uint8_t v, uint8_t prev) {
    int d = (int)v - (int)prev;
    if (d < 0) d = -d;
    return d <= X360_PAD_TRIGGER_HOLD ? prev : v;
}

/* O polegar parado muda o eixo a cada quadro. Segurar o valor anterior
 * impede que o teclado trate isso como um Y novo. */
static void settle_analogs(x360_gamepad_state_t *pad, const x360_gamepad_state_t *prev) {
    pad->thumb_lx = hold_near(pad->thumb_lx, prev->thumb_lx, X360_PAD_AXIS_HOLD);
    pad->thumb_ly = hold_near(pad->thumb_ly, prev->thumb_ly, X360_PAD_AXIS_HOLD);
    pad->thumb_rx = hold_near(pad->thumb_rx, prev->thumb_rx, X360_PAD_AXIS_HOLD);
    pad->thumb_ry = hold_near(pad->thumb_ry, prev->thumb_ry, X360_PAD_AXIS_HOLD);
    pad->left_trigger = hold_trigger(pad->left_trigger, prev->left_trigger);
    pad->right_trigger = hold_trigger(pad->right_trigger, prev->right_trigger);
}

static void calm_pad(x360_gamepad_state_t *pad, const x360_gamepad_state_t *prev) {
    calm_stick(&pad->thumb_lx, &pad->thumb_ly, prev->thumb_lx, prev->thumb_ly, X360_PAD_LEFT_DEADZONE);
    calm_stick(&pad->thumb_rx, &pad->thumb_ry, prev->thumb_rx, prev->thumb_ry, X360_PAD_RIGHT_DEADZONE);
    pad->left_trigger = calm_trigger(pad->left_trigger);
    pad->right_trigger = calm_trigger(pad->right_trigger);
}

/* Um quadro com o botão solto não conta. Três seguidos soltam.
 * O guia entra e sai na hora. Troca de botão também entra na hora. */
static uint16_t stable_buttons(x360_pad_inject_t *p, uint16_t buttons) {
    const uint16_t guide = (uint16_t)(buttons & X360_BTN_GUIDE);
    const uint16_t raw = (uint16_t)(buttons & (uint16_t)~X360_BTN_GUIDE);
    const uint16_t prev = p->button_level;
    const uint16_t rose = (uint16_t)(raw & (uint16_t)~prev);
    const uint16_t fell = (uint16_t)(prev & (uint16_t)~raw);
    if (fell == 0u) {
        p->button_release = 0u;
        p->button_level = raw;
        return (uint16_t)(raw | guide);
    }
    if (rose != 0u) {
        p->button_level = raw;
        p->button_release = 0u;
        return (uint16_t)(raw | guide);
    }
    if (++p->button_release < X360_PAD_DPAD_RELEASE)
        return (uint16_t)(prev | guide);
    p->button_level = raw;
    p->button_release = 0u;
    return (uint16_t)(raw | guide);
}

bool x360_pad_inject_publish(x360_pad_inject_t *p, bool authenticated, uint8_t player,
                             const x360_gamepad_state_t *state, bool ps_pressed) {
    if (!p || !state || !p->sink.write_slot) return false;
    if (!authenticated || player < X360_PAD_PLAYER_MIN || player > X360_PAD_PLAYER_MAX)
        return false;

    /* Jogador mudou (ex.: oficial saiu): libera o slot antigo antes de ocupar o novo. */
    if (p->active && p->player != player) {
        clear_slot(p, p->player);
        p->last = (x360_gamepad_state_t){ 0 };
        p->button_level = 0u;
        p->button_release = 0u;
    }

    x360_gamepad_state_t out = *state;
    if (ps_pressed) out.buttons = (uint16_t)(out.buttons | X360_BTN_GUIDE);
    calm_pad(&out, &p->last);
    out.buttons = stable_buttons(p, out.buttons);
    settle_analogs(&out, &p->last);
    /* O pacote oficial só muda quando o estado muda. Publicar igual a cada
     * 10 ms fazia um toque na seta avançar uma capa por leitura. */
    if (p->active && p->player == player && memcmp(&out, &p->last, sizeof out) == 0)
        return true;

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
    p->button_level = 0u;
    p->button_release = 0u;
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

static int16_t axis_or_rest(int16_t official, int16_t ds4) {
    return official != 0 ? official : ds4;
}

void x360_pad_overlay_player1(x360_gamepad_state_t *slot, const x360_gamepad_state_t *ds4,
                              bool official_present) {
    if (!slot || !ds4) return;
    x360_gamepad_state_t extra = *ds4;
    extra.buttons = (uint16_t)(extra.buttons & (uint16_t)~X360_BTN_GUIDE);
    if (!official_present) {
        *slot = extra;
        return;
    }
    slot->buttons = (uint16_t)(slot->buttons | extra.buttons);
    if (slot->left_trigger == 0u) slot->left_trigger = extra.left_trigger;
    if (slot->right_trigger == 0u) slot->right_trigger = extra.right_trigger;
    slot->thumb_lx = axis_or_rest(slot->thumb_lx, extra.thumb_lx);
    slot->thumb_ly = axis_or_rest(slot->thumb_ly, extra.thumb_ly);
    slot->thumb_rx = axis_or_rest(slot->thumb_rx, extra.thumb_rx);
    slot->thumb_ry = axis_or_rest(slot->thumb_ry, extra.thumb_ry);
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
        tracker->next_repeat = now_ms + 700u;
        return emit_key(out, bit, X360_KEY_DOWN, user_index);
    }
    /* A, B, X e Y disparam uma vez. Repetir o Y enche o teclado de espaços
     * e a tela deixa de responder. Só o direcional repete. */
    if ((tracker->repeat_bit & X360_PAD_DPAD_MASK) && (tracker->level & tracker->repeat_bit) &&
        now_ms >= tracker->next_repeat) {
        tracker->next_repeat = now_ms + 280u;
        bit = lowest_set(tracker->repeat_bit);
        return emit_key(out, bit, (uint16_t)(X360_KEY_DOWN | X360_KEY_REPEAT), user_index);
    }
    return X360_PAD_ERROR_EMPTY;
}

uint16_t x360_pad_face_bit(uint16_t virtual_key) {
    if (virtual_key == 0x5800u) return X360_BTN_A;
    if (virtual_key == 0x5801u) return X360_BTN_B;
    if (virtual_key == 0x5802u) return X360_BTN_X;
    if (virtual_key == 0x5803u) return X360_BTN_Y;
    return 0u;
}

uint32_t x360_pad_keystroke_filter(x360_key_tracker_t *tracker, const x360_gamepad_state_t *ds4,
                                   uint32_t original_status, x360_keystroke_t *key,
                                   uint8_t user_index, uint32_t now_ms) {
    x360_keystroke_t ignored;
    const uint16_t held = ds4 ? guide_nav_bits(ds4) : 0u;
    const uint16_t face = (original_status == 0u && key) ? x360_pad_face_bit(key->virtual_key) : 0u;
    if (face != 0u && (held & face) != 0u) {
        const uint32_t ours = x360_pad_keystroke_next(tracker, ds4, user_index, now_ms, key);
        if (ours != X360_PAD_ERROR_SUCCESS && key) memset(key, 0, sizeof *key);
        return ours;
    }
    if (original_status == 0u) {
        if (ds4) (void)x360_pad_keystroke_next(tracker, ds4, user_index, now_ms, &ignored);
        return original_status;
    }
    if (!ds4 || x360_pad_keystroke_next(tracker, ds4, user_index, now_ms, key) != 0u)
        return original_status;
    return X360_PAD_ERROR_SUCCESS;
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
