#include "ds4_bt.h"
#include <string.h>

static const uint16_t k_x360_slot_masks[BT_X360_MAP_SLOTS] = {
    X360_BTN_A,
    X360_BTN_B,
    X360_BTN_X,
    X360_BTN_Y,
    X360_BTN_LEFT_SHOULDER,
    X360_BTN_RIGHT_SHOULDER,
    X360_BTN_BACK,
    X360_BTN_START,
    X360_BTN_GUIDE,
    X360_BTN_LEFT_THUMB,
    X360_BTN_RIGHT_THUMB,
    X360_BTN_DPAD_UP,
    X360_BTN_DPAD_DOWN,
    X360_BTN_DPAD_LEFT,
    X360_BTN_DPAD_RIGHT
};

static const char *const k_x360_slot_names[BT_X360_MAP_SLOTS] = {
    "A", "B", "X", "Y", "LB", "RB", "Back", "Start",
    "Guide", "LS (L3)", "RS (R3)", "D-Pad Up", "D-Pad Down", "D-Pad Left", "D-Pad Right"
};

static const char *const k_raw_bit_names[BT_RAW_BIT_COUNT] = {
    "D-Up", "D-Down", "D-Left", "D-Right", "Options", "Share",
    "L3", "R3", "L1", "R1", "PS", "Cross", "Circle", "Square", "Triangle"
};

void bt_button_map_init_ds4_default(bt_button_map_t *map) {
    if (!map) return;
    map->raw_bit_for_x360[0]  = BT_RAW_BIT_CROSS;
    map->raw_bit_for_x360[1]  = BT_RAW_BIT_CIRCLE;
    map->raw_bit_for_x360[2]  = BT_RAW_BIT_SQUARE;
    map->raw_bit_for_x360[3]  = BT_RAW_BIT_TRIANGLE;
    map->raw_bit_for_x360[4]  = BT_RAW_BIT_L1;
    map->raw_bit_for_x360[5]  = BT_RAW_BIT_R1;
    map->raw_bit_for_x360[6]  = BT_RAW_BIT_BACK;
    map->raw_bit_for_x360[7]  = BT_RAW_BIT_START;
    map->raw_bit_for_x360[8]  = BT_RAW_BIT_GUIDE;
    map->raw_bit_for_x360[9]  = BT_RAW_BIT_L3;
    map->raw_bit_for_x360[10] = BT_RAW_BIT_R3;
    map->raw_bit_for_x360[11] = BT_RAW_BIT_DPAD_UP;
    map->raw_bit_for_x360[12] = BT_RAW_BIT_DPAD_DOWN;
    map->raw_bit_for_x360[13] = BT_RAW_BIT_DPAD_LEFT;
    map->raw_bit_for_x360[14] = BT_RAW_BIT_DPAD_RIGHT;
    map->swap_triggers = false;
}

uint16_t bt_x360_slot_bitmask(uint8_t slot_idx) {
    return (slot_idx < BT_X360_MAP_SLOTS) ? k_x360_slot_masks[slot_idx] : 0u;
}

const char *bt_x360_slot_name(uint8_t slot_idx) {
    return (slot_idx < BT_X360_MAP_SLOTS) ? k_x360_slot_names[slot_idx] : "?";
}

const char *bt_raw_bit_name(uint8_t raw_bit) {
    return (raw_bit < BT_RAW_BIT_COUNT) ? k_raw_bit_names[raw_bit] : "None";
}

static int16_t u8_to_s16_axis(uint8_t v, bool invert_y) {
    int32_t centered = (int32_t)v - 128;
    int32_t scaled = centered * 256;
    if (invert_y) {
        scaled = -scaled;
    }
    if (scaled > 32767) scaled = 32767;
    if (scaled < -32768) scaled = -32768;
    return (int16_t)scaled;
}

static uint32_t decode_hat_raw(uint8_t hat) {
    switch (hat & 0x0Fu) {
        case 0: return (1u << BT_RAW_BIT_DPAD_UP);
        case 1: return (1u << BT_RAW_BIT_DPAD_UP) | (1u << BT_RAW_BIT_DPAD_RIGHT);
        case 2: return (1u << BT_RAW_BIT_DPAD_RIGHT);
        case 3: return (1u << BT_RAW_BIT_DPAD_RIGHT) | (1u << BT_RAW_BIT_DPAD_DOWN);
        case 4: return (1u << BT_RAW_BIT_DPAD_DOWN);
        case 5: return (1u << BT_RAW_BIT_DPAD_DOWN) | (1u << BT_RAW_BIT_DPAD_LEFT);
        case 6: return (1u << BT_RAW_BIT_DPAD_LEFT);
        case 7: return (1u << BT_RAW_BIT_DPAD_LEFT) | (1u << BT_RAW_BIT_DPAD_UP);
        default: return 0u;
    }
}

int ds4_bt_parse_hidp_mapped(const uint8_t *hidp, size_t len,
                             const bt_button_map_t *map,
                             x360_gamepad_state_t *out_state,
                             uint32_t *out_raw_mask) {
    if (!hidp || !out_state || len < 11u) {
        return -1;
    }
    /* Bluetooth HIDP DATA (0xA0) | INPUT (0x01) = 0xA1 */
    if (hidp[0] != 0xA1u) {
        return -2;
    }

    const uint8_t *gp = NULL;
    if (hidp[1] == 0x01u && len >= 11u) {
        gp = hidp + 2u;
    } else if (hidp[1] == 0x11u && len >= 13u) {
        gp = hidp + 4u;
    } else {
        return -3;
    }

    bt_button_map_t default_map;
    if (!map) {
        bt_button_map_init_ds4_default(&default_map);
        map = &default_map;
    }

    memset(out_state, 0, sizeof(*out_state));

    out_state->thumb_lx = u8_to_s16_axis(gp[0], false);
    out_state->thumb_ly = u8_to_s16_axis(gp[1], true);
    out_state->thumb_rx = u8_to_s16_axis(gp[2], false);
    out_state->thumb_ry = u8_to_s16_axis(gp[3], true);

    uint8_t b0 = gp[4];
    uint8_t b1 = gp[5];
    uint8_t b2 = gp[6];

    uint32_t raw = decode_hat_raw(b0 & 0x0Fu);
    if (b0 & 0x10u) raw |= (1u << BT_RAW_BIT_SQUARE);
    if (b0 & 0x20u) raw |= (1u << BT_RAW_BIT_CROSS);
    if (b0 & 0x40u) raw |= (1u << BT_RAW_BIT_CIRCLE);
    if (b0 & 0x80u) raw |= (1u << BT_RAW_BIT_TRIANGLE);

    if (b1 & 0x01u) raw |= (1u << BT_RAW_BIT_L1);
    if (b1 & 0x02u) raw |= (1u << BT_RAW_BIT_R1);
    if (b1 & 0x10u) raw |= (1u << BT_RAW_BIT_BACK);
    if (b1 & 0x20u) raw |= (1u << BT_RAW_BIT_START);
    if (b1 & 0x40u) raw |= (1u << BT_RAW_BIT_L3);
    if (b1 & 0x80u) raw |= (1u << BT_RAW_BIT_R3);

    if (b2 & 0x01u) raw |= (1u << BT_RAW_BIT_GUIDE);

    if (out_raw_mask) {
        *out_raw_mask = raw;
    }

    uint16_t btns = 0;
    for (uint8_t s = 0; s < BT_X360_MAP_SLOTS; s++) {
        uint8_t rbit = map->raw_bit_for_x360[s];
        if (rbit < BT_RAW_BIT_COUNT && (raw & (1u << rbit))) {
            btns |= k_x360_slot_masks[s];
        }
    }

    out_state->buttons       = btns;
    out_state->left_trigger  = map->swap_triggers ? gp[8] : gp[7];
    out_state->right_trigger = map->swap_triggers ? gp[7] : gp[8];
    /* O analógico vem em zero em alguns quadros; o bit digital ainda é o gatilho. */
    if (out_state->left_trigger == 0 && (b1 & 0x04u)) out_state->left_trigger = 255u;
    if (out_state->right_trigger == 0 && (b1 & 0x08u)) out_state->right_trigger = 255u;
    return 0;
}

static uint32_t crc32_ieee(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++)
            crc = (crc & 1u) ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
    }
    return ~crc;
}

void ds4_bt_seal_output(uint8_t *report) {
    uint8_t covered[75];
    uint32_t crc;
    if (!report) return;
    covered[0] = 0xA2u;
    memcpy(covered + 1, report + 1, 74u);
    crc = crc32_ieee(covered, sizeof covered);
    report[75] = (uint8_t)crc;
    report[76] = (uint8_t)(crc >> 8);
    report[77] = (uint8_t)(crc >> 16);
    report[78] = (uint8_t)(crc >> 24);
}

size_t ds4_bt_build_rumble_report(uint8_t *out, size_t out_len,
                                  uint8_t small_motor, uint8_t large_motor) {
    if (!out || out_len < DS4_BT_OUTPUT_REPORT_LEN) {
        return 0u;
    }
    /* Offsets iguais a ds4_live.c: 0x52, 0x11, 0xC0, 0x00, flags 0x07, motores em 7 e 8 */
    memset(out, 0, DS4_BT_OUTPUT_REPORT_LEN);
    out[0] = 0x52u; /* HIDP SET_REPORT | OUTPUT */
    out[1] = 0x11u; /* Report ID */
    out[2] = 0xC0u;
    out[4] = 0x07u; /* habilita rumble/LED */
    out[7] = small_motor;
    out[8] = large_motor;
    ds4_bt_seal_output(out);
    return DS4_BT_OUTPUT_REPORT_LEN;
}

int ds4_bt_parse_hidp_report(const uint8_t *hidp, size_t len,
                             x360_gamepad_state_t *out_state) {
    return ds4_bt_parse_hidp_mapped(hidp, len, NULL, out_state, NULL);
}
