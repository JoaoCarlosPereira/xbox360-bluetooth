/*
 * BTManager.xex — Xbox 360 Bluetooth Connection & Pairing UI
 * Inspired by the original Xbox 360 NXE / Guide interface.
 * Built with xenosworks/xsdk (no proprietary Microsoft SDK).
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/present.h"
#include "xsdk/text.h"

#include "../manager/bt_manager.h"

/* Original Xbox 360 NXE / Guide inspired palette (0xAARRGGBB) */
#define COL_BG_DARK        0xFF12161C
#define COL_XBOX_GREEN     0xFF9BC848
#define COL_XBOX_DIM_GREEN 0xFF3E6B1E
#define COL_TEXT_PRIMARY   0xFFF0F4F8
#define COL_TEXT_MUTED     0xFF8A949E
#define COL_TEXT_WARN      0xFFE8A838

enum {
    BUFFERS = 2,
    SWAP_INTERVAL = 1,
    BEGIN_TIMEOUT_MS = 5000
};

static void draw_remap_screen(uint32_t width, uint32_t height,
                              const bt_ipc_state_t *ipc,
                              const bt_remap_wizard_t *wiz) {
    (void)height;
    xsdk_gpu_set_target(width, height);

    char title[96];
    snprintf(title, sizeof(title), "BUTTON MAPPING — PLAYER %u (%s)",
             (unsigned)(wiz->slot_idx + 1),
             ipc->slots[wiz->slot_idx].name[0] ? ipc->slots[wiz->slot_idx].name : "Controller");
    xsdk_text_draw(48, 32, 2, COL_XBOX_GREEN, title);

    char prompt[96];
    snprintf(prompt, sizeof(prompt), "Press the button on your Bluetooth controller for Xbox 360 [%s]",
             bt_x360_slot_name(wiz->step));
    xsdk_text_draw(48, 84, 1, COL_TEXT_WARN, prompt);

    const bt_button_map_t *map = &ipc->slots[wiz->slot_idx].map;
    for (uint8_t s = 0; s < BT_X360_MAP_SLOTS; s++) {
        int32_t col_x = (s < 8) ? 48 : 640;
        int32_t row_y = 140 + (int32_t)((s % 8) * 38);
        bool cur = (s == wiz->step);
        char line[96];
        snprintf(line, sizeof(line), "%s Xbox [%-10s]  <--  %s",
                 cur ? ">" : " ",
                 bt_x360_slot_name(s),
                 cur ? "<waiting...>" : bt_raw_bit_name(map->raw_bit_for_x360[s]));
        xsdk_text_draw(col_x, row_y, 1, cur ? COL_XBOX_GREEN : COL_TEXT_PRIMARY, line);
    }

    xsdk_text_draw(48, 656, 1, COL_XBOX_GREEN,
                   "[Back] Cancel Mapping");
}

static void draw_ui_frame(uint32_t width, uint32_t height,
                          const bt_ipc_state_t *ipc,
                          const bt_ui_model_t *ui) {
    (void)height;
    xsdk_gpu_set_target(width, height);

    /* Top NXE-style header */
    xsdk_text_draw(48, 32, 2, COL_XBOX_GREEN, "XBOX 360 BLUETOOTH MANAGER");

    char hdr[96];
    if (ipc->dongle_ready) {
        snprintf(hdr, sizeof(hdr), "Adapter: %04X:%04X  BD: %02X:%02X:%02X:%02X:%02X:%02X",
                 ipc->dongle_vid, ipc->dongle_pid,
                 ipc->dongle_bd_addr[5], ipc->dongle_bd_addr[4], ipc->dongle_bd_addr[3],
                 ipc->dongle_bd_addr[2], ipc->dongle_bd_addr[1], ipc->dongle_bd_addr[0]);
    } else {
        snprintf(hdr, sizeof(hdr), "Adapter: Waiting for bt360_plugin.xex / USB Dongle...");
    }
    xsdk_text_draw(48, 80, 1, ipc->dongle_ready ? COL_TEXT_PRIMARY : COL_TEXT_WARN, hdr);

    /* Pane 1: Active Player Slots (1..4) */
    uint32_t c_slots = (ui->pane == BT_UI_PANE_SLOTS) ? COL_XBOX_GREEN : COL_TEXT_MUTED;
    xsdk_text_draw(48, 130, 1, c_slots, "[1] ACTIVE CONTROLLERS (PLAYERS 1-4)");
    for (uint32_t i = 0; i < BT_MAX_ACTIVE_SLOTS; i++) {
        char line[96];
        bool sel = (ui->pane == BT_UI_PANE_SLOTS && ui->cursor == (int)i);
        uint32_t col = sel ? COL_XBOX_GREEN : COL_TEXT_PRIMARY;
        if (ipc->slots[i].connected) {
            snprintf(line, sizeof(line), "%s P%u: %-18s (%02X:%02X:%02X:%02X:%02X:%02X)",
                     sel ? ">" : " ", (unsigned)(i + 1),
                     ipc->slots[i].name[0] ? ipc->slots[i].name : "Controller",
                     ipc->slots[i].bd_addr[5], ipc->slots[i].bd_addr[4], ipc->slots[i].bd_addr[3],
                     ipc->slots[i].bd_addr[2], ipc->slots[i].bd_addr[1], ipc->slots[i].bd_addr[0]);
        } else {
            snprintf(line, sizeof(line), "%s P%u: (Empty)", sel ? ">" : " ", (unsigned)(i + 1));
            if (!sel) col = COL_TEXT_MUTED;
        }
        xsdk_text_draw(48, 165 + (int32_t)(i * 28), 1, col, line);
    }

    /* Pane 2: Paired Devices */
    uint32_t c_paired = (ui->pane == BT_UI_PANE_PAIRED) ? COL_XBOX_GREEN : COL_TEXT_MUTED;
    xsdk_text_draw(48, 310, 1, c_paired, "[2] PAIRED DEVICES (SAVED LINK KEYS)");
    int p_drawn = 0;
    for (size_t i = 0; i < BT_MAX_PAIRED_DEVICES; i++) {
        if (!ipc->paired[i].valid) continue;
        char line[96];
        bool sel = (ui->pane == BT_UI_PANE_PAIRED && ui->cursor == p_drawn);
        snprintf(line, sizeof(line), "%s %-20s %02X:%02X:%02X:%02X:%02X:%02X",
                 sel ? ">" : " ",
                 ipc->paired[i].name[0] ? ipc->paired[i].name : "Controller",
                 ipc->paired[i].bd_addr[5], ipc->paired[i].bd_addr[4], ipc->paired[i].bd_addr[3],
                 ipc->paired[i].bd_addr[2], ipc->paired[i].bd_addr[1], ipc->paired[i].bd_addr[0]);
        xsdk_text_draw(48, 345 + (p_drawn * 28), 1, sel ? COL_XBOX_GREEN : COL_TEXT_PRIMARY, line);
        p_drawn++;
    }
    if (p_drawn == 0) {
        xsdk_text_draw(48, 345, 1, COL_TEXT_MUTED, "  (No paired devices saved yet)");
    }

    /* Pane 3: Nearby Scan Results */
    uint32_t c_scan = (ui->pane == BT_UI_PANE_SCAN) ? COL_XBOX_GREEN : COL_TEXT_MUTED;
    xsdk_text_draw(660, 130, 1, c_scan,
                   ipc->scan_active ? "[3] NEARBY DEVICES (SCANNING...)" : "[3] NEARBY DEVICES");
    if (ipc->scan_count == 0) {
        xsdk_text_draw(660, 165, 1, COL_TEXT_MUTED, "  Press [Y] to scan (Share + PS on DS4)");
    } else {
        for (uint32_t i = 0; i < ipc->scan_count && i < 12; i++) {
            const hci_inquiry_item_t *it = &ipc->scan_items[i];
            char line[96];
            bool sel = (ui->pane == BT_UI_PANE_SCAN && ui->cursor == (int)i);
            snprintf(line, sizeof(line), "%s %-18s %02X:%02X:%02X:%02X:%02X:%02X (%d)",
                     sel ? ">" : " ",
                     it->name[0] ? it->name : "BT Device",
                     it->bd_addr[5], it->bd_addr[4], it->bd_addr[3],
                     it->bd_addr[2], it->bd_addr[1], it->bd_addr[0],
                     it->has_rssi ? (int)it->rssi : 0);
            xsdk_text_draw(660, 165 + (int32_t)(i * 28), 1,
                           sel ? COL_XBOX_GREEN : COL_TEXT_PRIMARY, line);
        }
    }

    /* Bottom Xbox 360 Guide-style button bar */
    xsdk_text_draw(48, 620, 1, COL_TEXT_MUTED, ipc->status_msg);
    xsdk_text_draw(48, 656, 1, COL_XBOX_GREEN,
                   "[A] Pair/Connect  [Y] Scan  [X] Forget  [Start] Map Buttons  [B] Exit");
}

int main(void) {
    if (!NT_SUCCESS(xsdk_gpu_open())) return 1;

    uint32_t width = 0, height = 0;
    if (!NT_SUCCESS(xsdk_present_open(XSDK_DISPLAY_720P, BUFFERS, SWAP_INTERVAL, &width, &height))) {
        xsdk_gpu_close();
        return 1;
    }
    if (!NT_SUCCESS(xsdk_text_open())) {
        xsdk_present_close();
        xsdk_gpu_close();
        return 1;
    }

    /* Local preview/IPC state (to be backed by shared plugin IPC block on console) */
    bt_ipc_state_t local_ipc;
    bt_ipc_init(&local_ipc);
    bt_ui_model_t ui;
    bt_ui_model_init(&ui);
    bt_remap_wizard_t wiz = { 0 };

    uint16_t prev_btns = 0;
    while (!ui.exit_requested) {
        INPUT_STATE input = { 0 };
        uint16_t btns = 0;
        if (XamInputGetState(0, 0, &input) == ERROR_SUCCESS) {
            btns = input.gamepad.buttons;
        }
        uint16_t newly_pressed = (uint16_t)(btns & (uint16_t)(~prev_btns));
        prev_btns = btns;

        if (wiz.active) {
            if (newly_pressed & INPUT_GAMEPAD_BACK) {
                wiz.active = false;
            } else {
                bt_remap_wizard_feed_raw(&wiz, local_ipc.slot_raw_mask[wiz.slot_idx],
                                         &local_ipc.slots[wiz.slot_idx].map);
            }
        } else {
            if ((newly_pressed & INPUT_GAMEPAD_START) &&
                ui.pane == BT_UI_PANE_SLOTS &&
                ui.cursor >= 0 && (uint32_t)ui.cursor < BT_MAX_ACTIVE_SLOTS &&
                local_ipc.slots[ui.cursor].connected) {
                bt_remap_wizard_start(&wiz, (uint32_t)ui.cursor,
                                      local_ipc.slot_raw_mask[ui.cursor]);
            } else {
                bt_ui_handle_input(&ui, &local_ipc, newly_pressed);
            }
        }
        if (ui.exit_requested) break;

        uint32_t address = 0;
        if (!NT_SUCCESS(xsdk_present_begin(&address, BEGIN_TIMEOUT_MS))) break;

        if (wiz.active) {
            draw_remap_screen(width, height, &local_ipc, &wiz);
        } else {
            draw_ui_frame(width, height, &local_ipc, &ui);
        }
        xsdk_gpu_resolve(address, width, height, COL_BG_DARK);
        xsdk_present_end();
    }

    xsdk_text_close();
    xsdk_present_close();
    xsdk_gpu_close();
    return 0;
}
