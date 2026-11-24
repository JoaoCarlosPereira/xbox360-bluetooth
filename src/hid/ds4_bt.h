#ifndef XBOX360_BT_DS4_H
#define XBOX360_BT_DS4_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Matches Xbox 360 XINPUT_GAMEPAD bitmask (xsdk/kernel.h:103-119) */
#define X360_BTN_DPAD_UP        0x0001u
#define X360_BTN_DPAD_DOWN      0x0002u
#define X360_BTN_DPAD_LEFT      0x0004u
#define X360_BTN_DPAD_RIGHT     0x0008u
#define X360_BTN_START          0x0010u
#define X360_BTN_BACK           0x0020u
#define X360_BTN_LEFT_THUMB     0x0040u
#define X360_BTN_RIGHT_THUMB    0x0080u
#define X360_BTN_LEFT_SHOULDER  0x0100u
#define X360_BTN_RIGHT_SHOULDER 0x0200u
#define X360_BTN_GUIDE          0x0400u
#define X360_BTN_A              0x1000u
#define X360_BTN_B              0x2000u
#define X360_BTN_X              0x4000u
#define X360_BTN_Y              0x8000u

typedef struct {
    uint16_t buttons;
    uint8_t  left_trigger;
    uint8_t  right_trigger;
    int16_t  thumb_lx;
    int16_t  thumb_ly;
    int16_t  thumb_rx;
    int16_t  thumb_ry;
} x360_gamepad_state_t;

/* Canonical raw digital button bits extracted from Bluetooth controller */
enum {
    BT_RAW_BIT_DPAD_UP = 0,
    BT_RAW_BIT_DPAD_DOWN,
    BT_RAW_BIT_DPAD_LEFT,
    BT_RAW_BIT_DPAD_RIGHT,
    BT_RAW_BIT_START,       /* Options */
    BT_RAW_BIT_BACK,        /* Share */
    BT_RAW_BIT_L3,
    BT_RAW_BIT_R3,
    BT_RAW_BIT_L1,
    BT_RAW_BIT_R1,
    BT_RAW_BIT_GUIDE,       /* PS */
    BT_RAW_BIT_CROSS,       /* A */
    BT_RAW_BIT_CIRCLE,      /* B */
    BT_RAW_BIT_SQUARE,      /* X */
    BT_RAW_BIT_TRIANGLE,    /* Y */
    BT_RAW_BIT_COUNT
};

#define BT_X360_MAP_SLOTS 15u

typedef struct {
    /* For each Xbox 360 target slot (0..14), which BT_RAW_BIT_* triggers it (0xFF = unmapped) */
    uint8_t  raw_bit_for_x360[BT_X360_MAP_SLOTS];
    bool     swap_triggers;
} bt_button_map_t;

void bt_button_map_init_ds4_default(bt_button_map_t *map);
uint16_t bt_x360_slot_bitmask(uint8_t slot_idx);
const char *bt_x360_slot_name(uint8_t slot_idx);
const char *bt_raw_bit_name(uint8_t raw_bit);

/**
 * Parse a DualShock 4 Bluetooth HIDP DATA|INPUT payload (0xA1 0x01 ... or 0xA1 0x11 ...).
 * Returns 0 on success, negative if unsupported or truncated.
 */
int ds4_bt_parse_hidp_report(const uint8_t *hidp, size_t len,
                             x360_gamepad_state_t *out_state);

/**
 * Same as ds4_bt_parse_hidp_report(), but applies a custom button mapping table
 * and optionally returns the unmapped raw physical button bitmask (*out_raw_mask).
 */
int ds4_bt_parse_hidp_mapped(const uint8_t *hidp, size_t len,
                             const bt_button_map_t *map,
                             x360_gamepad_state_t *out_state,
                             uint32_t *out_raw_mask);

#ifdef __cplusplus
}
#endif

#endif /* XBOX360_BT_DS4_H */
