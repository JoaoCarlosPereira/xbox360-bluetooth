#include "dashboard.h"
#include "dashboard_draw.h"

#include <stdio.h>

enum {
    BACKGROUND = 0x27282E,
    PANEL = 0x45464B,
    TRACK = 0x53545A,
    LINE = 0x73747B,
    WHITE = 0xFCFCF7,
    MUTED = 0xDDDEE1,
    INK = 0x1A290F,
    LIME = 0xD6F783,
    GREEN = 0x22630C,
    FOCUS = 0x4A880E,
    GOLD = 0xF2C343,
    MAX_ROWS = 5,
    LEFT = 64,
    PANEL_Y = 223,
    PANEL_HEIGHT = 352,
    NAV_WIDTH = 175,
    MAIN_X = 249,
    MAIN_WIDTH = 660,
    SIDE_X = 919,
    SIDE_WIDTH = 297
};

static void action(float x, char key, uint32_t color, const char *label) {
    char letter[] = {key, 0};
    dash_icon(DASH_CIRCLE, x, 638, 28, color);
    dash_text(DASH_BODY, x + 8, 659, 19, WHITE, letter);
    dash_text(DASH_BODY, x + 38, 659, 186, WHITE, label);
}

static const bt_paired_device_t *owner_of(const bt_ipc_state_t *ipc, unsigned player) {
    for (unsigned i = 0; i < BT_MAX_PAIRED_DEVICES; i++) {
        if (ipc->paired[i].valid && ipc->paired[i].player == player)
            return &ipc->paired[i];
    }
    return NULL;
}

static void chrome(const bt_ipc_state_t *ipc, const bt_ui_model_t *ui, bool mapping) {
    static const char *tabs[] = {"Controles", "Pareados", "Buscar"};
    const bt_ui_pane_t pane = ui->pane;
    const bool content_focused = ui->content_focused;
    const bool identify = ui->identify_mode;
    dash_rect(0, 0, 1280, 720, BACKGROUND);
    dash_art(DASH_SILVER, 0, 0, 1280, 720, 0xB7B9C0);

    dash_rect(64, 42, 6, 25, LIME);
    dash_text(DASH_LABEL, 82, 63, 550, WHITE, "Xbox 360  /  Acessórios");
    dash_text(DASH_TITLE, 64, 122, 780, WHITE, "Bluetooth Manager");
    dash_text(DASH_BODY, 1001, 58, 217, WHITE, "by João Carlos Pereira");

    dash_rect(932, 135, 284, 42, LINE);
    dash_rect(934, 137, 280, 38, 0x3B3C44);
    dash_icon(DASH_CIRCLE, 950, 150, 12, ipc->dongle_ready ? LIME : GOLD);
    dash_text(DASH_BODY, 974, 163, 224, WHITE,
              ipc->dongle_ready ? "Adaptador pronto" : "Adaptador offline");

    if (!mapping) {
        for (unsigned i = 0; i < sizeof(tabs) / sizeof(tabs[0]); i++) {
            const float x = 67 + i * 166;
            const bool active = pane == (bt_ui_pane_t)i;
            dash_text(DASH_LABEL, x + 7, 172, 140, active ? WHITE : MUTED, tabs[i]);
            if (active) dash_rect(x, 183, 115, 5, LIME);
        }
    }

    {
        const char *hint = content_focused ? "Esquerda volta ao menu" : "Direita entra no painel";
        uint32_t hint_color = MUTED;
        if (identify && pane == BT_UI_PANE_SLOTS) {
            hint = "Segure SHARE no controle que quer achar";
            hint_color = GOLD;
        } else if (pane == BT_UI_PANE_SCAN && bt_manager_paired_count(ipc) >= 4) {
            hint = "Quatro controles já pareados";
            hint_color = GOLD;
        }
        dash_text(DASH_BODY, MAIN_X, 208, MAIN_WIDTH, hint_color, hint);
    }

    dash_text(DASH_BODY, 64, 615, 1152, WHITE, ipc->status_msg);
    if (mapping) {
        dash_text(DASH_BODY, 64, 659, 500, WHITE, "Back   Cancelar remapeamento");
        return;
    }

    const bt_paired_device_t *slot_owner = NULL;
    if (pane == BT_UI_PANE_SLOTS && content_focused && ui->cursor >= 0 &&
        (uint32_t)ui->cursor < BT_MAX_ACTIVE_SLOTS)
        slot_owner = owner_of(ipc, (unsigned)ui->cursor + 1u);
    const bool paired_item = pane == BT_UI_PANE_PAIRED && content_focused && ui->cursor >= 0;
    bool paired_here = false;
    if (paired_item) {
        int seen = 0;
        for (unsigned i = 0; i < BT_MAX_PAIRED_DEVICES; i++) {
            if (!ipc->paired[i].valid) continue;
            if (seen == ui->cursor) { paired_here = true; break; }
            seen++;
        }
    }
    const bool live_card = pane == BT_UI_PANE_SLOTS && content_focused && ui->cursor >= 0 &&
                           ipc->pad_link == BT_LINK_UP &&
                           ipc->live_player == (uint8_t)(ui->cursor + 1);
    const bool slot_up = pane == BT_UI_PANE_SLOTS && content_focused && ui->cursor >= 0 &&
                         (uint32_t)ui->cursor < BT_MAX_ACTIVE_SLOTS &&
                         ipc->slots[ui->cursor].connected;

    float x = 64;
    if (pane == BT_UI_PANE_SCAN && content_focused && bt_manager_paired_count(ipc) < 4) {
        action(x, 'A', 0x347221, "Parear");
        x += 210;
    } else if (slot_owner) {
        action(x, 'A', 0x347221, "Trocar jogador");
        x += 230;
    } else if (paired_here) {
        action(x, 'A', 0x347221, "Conectar");
        x += 190;
    }
    action(x, 'Y', 0xA76608, ipc->scan_active ? "Parar busca" : "Buscar");
    x += 200;
    if (slot_up || live_card) {
        action(x, 'X', 0x23638E, "Desconectar");
        x += 210;
    } else if (paired_here) {
        action(x, 'X', 0x23638E, "Esquecer");
        x += 190;
    }
    if (pane == BT_UI_PANE_SLOTS) {
        action(x, 'L', 0x5B4B8A, identify ? "Parar" : "Identificar");
        x += 250;
    }
    action(x, 'B', 0x8F2C26, "Sair");
}

static void navigation(bt_ui_pane_t pane, bool focused) {
    static const char *labels[] = {"Controles", "Pareados", "Buscar"};
    static const dash_icon_t icons[] = {DASH_GAMEPAD, DASH_BLUETOOTH, DASH_SEARCH};
    for (unsigned i = 0; i < 3; i++) {
        const bool selected = pane == (bt_ui_pane_t)i;
        const float y = PANEL_Y + i * 119;
        if (selected && focused) {
            dash_rect(LEFT - 2, y - 2, NAV_WIDTH + 4, 118, WHITE);
        }
        dash_rect(LEFT, y, NAV_WIDTH, 114, selected ? FOCUS : GREEN);
        dash_art(DASH_GLEAM, LEFT, y, NAV_WIDTH, 114, selected ? 0xAAD823 : 0x428A15);
        dash_icon(icons[i], LEFT + 58, y + 12, 60, WHITE);
        dash_text(DASH_LABEL, LEFT + 16, y + 100, NAV_WIDTH - 32, WHITE, labels[i]);
    }
}

static const char *card_name(const bt_ipc_state_t *ipc, unsigned i) {
    const bt_paired_device_t *owner = owner_of(ipc, i + 1u);
    if (owner && owner->name[0]) return owner->name;
    if (ipc->slots[i].connected && ipc->slots[i].name[0]) return ipc->slots[i].name;
    if (owner) return "DualShock 4";
    return "Slot livre";
}

static const char *card_status(const bt_ipc_state_t *ipc, unsigned i, bool lit) {
    const bool live = ipc->live_player == (uint8_t)(i + 1u);
    if (lit) return "Este controle";
    if (live && ipc->pad_link == BT_LINK_UP) return "Conectado";
    if (live && ipc->pad_link == BT_LINK_CONNECTING) return "Conectando";
    if (owner_of(ipc, i + 1u) || ipc->slots[i].connected) return "Pareado";
    return "Livre";
}

static void player_summary(const bt_ipc_state_t *ipc, bool identify) {
    char head[24];
    snprintf(head, sizeof head, "%d de 4 pareados", bt_manager_paired_count(ipc));
    dash_rect(SIDE_X, PANEL_Y, SIDE_WIDTH, PANEL_HEIGHT, PANEL);
    dash_text(DASH_LABEL, SIDE_X + 19, PANEL_Y + 32, SIDE_WIDTH - 38, WHITE, "Jogadores");
    dash_text(DASH_BODY, SIDE_X + 19, PANEL_Y + 62, SIDE_WIDTH - 38, MUTED, head);
    for (unsigned i = 0; i < BT_MAX_ACTIVE_SLOTS; i++) {
        const bool lit = identify && ipc->identify_player == (uint8_t)(i + 1u);
        const float y = PANEL_Y + 96 + i * 58;
        const char player[] = {'J', (char)('1' + i), 0};
        const char *status = card_status(ipc, i, lit);
        dash_rect(SIDE_X, y, SIDE_WIDTH, 57, lit ? GOLD : TRACK);
        dash_rect(SIDE_X, y, SIDE_WIDTH, 1, LINE);
        dash_text(DASH_LABEL, SIDE_X + 16, y + 36, 42, lit ? INK : WHITE, player);
        dash_text(DASH_BODY, SIDE_X + 62, y + 35, SIDE_WIDTH - 78, lit ? INK : MUTED, status);
    }
}

static void player_cards(const bt_ipc_state_t *ipc, int cursor, bool identify) {
    for (unsigned i = 0; i < BT_MAX_ACTIVE_SLOTS; i++) {
        const bool selected = cursor == (int)i;
        const bool lit = identify && ipc->identify_player == (uint8_t)(i + 1u);
        const bt_paired_device_t *owner = owner_of(ipc, i + 1u);
        const float x = MAIN_X + (i % 2) * 334;
        const float y = PANEL_Y + (i / 2) * 180;
        const char number[] = {(char)('1' + i), 0};
        const uint32_t fg = (lit || selected) ? INK : WHITE;
        char detail[40];
        const char *status = card_status(ipc, i, lit);
        if (owner) {
            snprintf(detail, sizeof detail, "%s   %02X:%02X:%02X", status,
                     owner->bd_addr[2], owner->bd_addr[1], owner->bd_addr[0]);
        } else {
            snprintf(detail, sizeof detail, "%s", status);
        }
        if (selected) dash_rect(x - 3, y - 3, 332, 178, WHITE);
        dash_rect(x, y, 326, 172, lit ? GOLD : selected ? LIME : PANEL);
        dash_icon(DASH_GAMEPAD, x + 226, y + 16, 73, fg);
        dash_text(DASH_TITLE, x + 22, y + 72, 70, fg, number);
        dash_text(DASH_LABEL, x + 78, y + 58, 140, fg, "Jogador");
        dash_text(DASH_LABEL, x + 24, y + 118, 278, fg, card_name(ipc, i));
        dash_text(DASH_BODY, x + 24, y + 150, 278, (lit || selected) ? INK : MUTED, detail);
    }
}

static void empty_panel(bool searching, bool paired, bool none_found) {
    dash_rect(MAIN_X, PANEL_Y, MAIN_WIDTH, PANEL_HEIGHT, 0x12340F);
    dash_art(DASH_RIBBON, MAIN_X, PANEL_Y, MAIN_WIDTH, PANEL_HEIGHT, 0x98CD26);
    dash_art(DASH_GLEAM, MAIN_X, PANEL_Y, MAIN_WIDTH, PANEL_HEIGHT, 0x235612);
    dash_icon(paired ? DASH_GAMEPAD : DASH_BLUETOOTH, 733, 319, 132, WHITE);
    dash_text(DASH_BODY, 278, 280, 410, 0xE3F2D6, paired ? "BIBLIOTECA" : "BLUETOOTH");
    dash_text(DASH_TITLE, 276, 345, 435, WHITE, paired ? "Seus dispositivos" : "Encontre");
    if (!paired) dash_text(DASH_TITLE, 276, 396, 435, WHITE, "seu controle");
    dash_text(DASH_BODY, 278, 453, 445, WHITE, paired ? "Nenhum controle pareado."
              : searching ? "Procurando dispositivos próximos..."
              : none_found ? "Nenhum dispositivo encontrado."
              : "Coloque o controle em modo de pareamento.");
    dash_text(DASH_BODY, 278, 484, 445, WHITE, paired ? "Abra Buscar e aperte A no controle."
              : "No DualShock 4, segure SHARE + PS.");
}

static const char *shown_name(const char *name, uint32_t cod) {
    if (name && name[0]) return name;
    const uint32_t major = (cod >> 8) & 0x1Fu;
    const uint32_t minor = (cod >> 2) & 0x3Fu;
    if (major == 0x05u && minor == 0x02u) return "Controle";
    if (major == 0x05u && minor == 0x10u) return "Teclado";
    if (major == 0x02u) return "Telefone";
    if (major == 0x04u) return "Audio";
    if (major == 0x01u) return "Computador";
    return "Dispositivo Bluetooth";
}

static void devices(const bt_ipc_state_t *ipc, int cursor, bool paired, bool focused) {
    unsigned count = ipc->scan_count > BT_MAX_SCAN_ITEMS ? BT_MAX_SCAN_ITEMS : ipc->scan_count;
    unsigned pair_indices[BT_MAX_PAIRED_DEVICES];
    if (paired) {
        count = 0;
        for (unsigned i = 0; i < BT_MAX_PAIRED_DEVICES; i++) {
            if (ipc->paired[i].valid) pair_indices[count++] = i;
        }
    }
    if (!count) {
        empty_panel(ipc->scan_active, paired, ipc->scan_finished_empty);
        return;
    }

    const bool has_cursor = focused && cursor >= 0 && (unsigned)cursor < count;
    const unsigned selected = has_cursor ? (unsigned)cursor : count;
    const unsigned window = has_cursor ? selected : 0;
    const unsigned start = window < MAX_ROWS ? 0 : window - MAX_ROWS + 1;
    dash_rect(MAIN_X, PANEL_Y, MAIN_WIDTH, PANEL_HEIGHT, PANEL);
    dash_text(DASH_LABEL, MAIN_X + 22, PANEL_Y + 40, 545, WHITE, paired ? "Dispositivos pareados" : "Dispositivos próximos");
    char position[24];
    if (has_cursor) snprintf(position, sizeof position, "%u / %u", selected + 1, count);
    else snprintf(position, sizeof position, "%u", count);
    dash_text(DASH_BODY, MAIN_X + MAIN_WIDTH - 89, PANEL_Y + 39, 77, MUTED, position);
    for (unsigned i = start; i < count && i < start + MAX_ROWS; i++) {
        const char *name = paired ? ipc->paired[pair_indices[i]].name : ipc->scan_items[i].name;
        const uint32_t cod = paired ? ipc->paired[pair_indices[i]].class_of_device
                                    : ipc->scan_items[i].class_of_device;
        const uint8_t *addr = paired ? ipc->paired[pair_indices[i]].bd_addr : ipc->scan_items[i].bd_addr;
        const uint32_t fg = i == selected ? INK : WHITE;
        const float y = PANEL_Y + 58 + (i - start) * 55;
        char address[18];
        snprintf(address, sizeof address, "%02X:%02X:%02X:%02X:%02X:%02X",
                 addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
        dash_rect(MAIN_X + 16, y, MAIN_WIDTH - 32, 48, i == selected ? LIME : TRACK);
        dash_text(DASH_LABEL, MAIN_X + 32, y + 31, 390, fg, shown_name(name, cod));
        if (paired) {
            const uint8_t player = ipc->paired[pair_indices[i]].player;
            const bool live = player >= 1u && player == ipc->live_player;
            char right[20];
            const char *text;
            uint32_t color = fg;
            if (live && ipc->pad_link == BT_LINK_UP) text = "Conectado";
            else if (live && ipc->pad_link == BT_LINK_CONNECTING) text = "Conectando";
            else if (player >= 1u && player <= 4u) {
                snprintf(right, sizeof right, "Jogador %u", player);
                text = right;
                color = i == selected ? INK : MUTED;
            } else {
                text = "Sem jogador";
                color = i == selected ? INK : MUTED;
            }
            if (live) color = i == selected ? INK : (ipc->pad_link == BT_LINK_UP ? LIME : GOLD);
            dash_text(DASH_BODY, MAIN_X + 434, y + 30, 207, color, text);
        } else if (bt_manager_find_paired(ipc, addr)) {
            dash_text(DASH_BODY, MAIN_X + 434, y + 30, 207, i == selected ? INK : GOLD, "Já pareado");
        } else {
            dash_text(DASH_BODY, MAIN_X + 434, y + 30, 207, fg, address);
        }
    }
}

static void remap(const bt_ipc_state_t *ipc, const bt_remap_wizard_t *wizard) {
    const bt_player_slot_t *slot = &ipc->slots[wizard->slot_idx];
    dash_text(DASH_LABEL, 64, 174, 1152, LIME, "Remapeamento");
    dash_text(DASH_BODY, 64, 208, 1152, WHITE, "Pressione o botão do controle para a ação destacada.");
    for (unsigned i = 0; i < BT_X360_MAP_SLOTS; i++) {
        const float x = 64 + (i / 8) * 584;
        const float y = 231 + (i % 8) * 43;
        const bool active = i == wizard->step;
        const uint32_t fg = active ? INK : WHITE;
        dash_rect(x, y, 568, 37, active ? LIME : PANEL);
        dash_text(DASH_BODY, x + 16, y + 26, 250, fg, bt_x360_slot_name(i));
        dash_text(DASH_BODY, x + 278, y + 26, 268, fg,
                  active ? "Aguardando botão" : bt_raw_bit_name(slot->map.raw_bit_for_x360[i]));
    }
}

void dashboard_draw(const bt_ipc_state_t *ipc, const bt_ui_model_t *ui,
                    const bt_remap_wizard_t *wizard) {
    chrome(ipc, ui, wizard->active);
    if (wizard->active) {
        remap(ipc, wizard);
    } else {
        navigation(ui->pane, !ui->content_focused);
        if (ui->content_focused && ui->pane != BT_UI_PANE_SLOTS)
            dash_rect(MAIN_X - 3, PANEL_Y - 3, MAIN_WIDTH + 6, PANEL_HEIGHT + 6, WHITE);
        if (ui->pane == BT_UI_PANE_SLOTS) {
            player_cards(ipc, ui->content_focused ? ui->cursor : -1, ui->identify_mode);
        } else {
            devices(ipc, ui->cursor, ui->pane == BT_UI_PANE_PAIRED, ui->content_focused);
        }
        player_summary(ipc, ui->identify_mode);
    }
}
