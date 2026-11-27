#include "dashboard.h"
#include "dashboard_draw.h"

#include <stdio.h>
#include <string.h>

enum {
    BACKGROUND  = 0x14171B,
    RIBBON_GLOW = 0x1B2D1E,
    PANEL       = 0x1F232A,
    PANEL_HI    = 0x292F38,
    TRACK       = 0x282D35,
    LINE        = 0x353B45,
    PILL_DIM    = 0x303640,
    WHITE       = 0xF4F1EA,
    MUTED       = 0x9FA7B2,
    DIM         = 0x6F7782,
    INK         = 0x16220D,
    LIME        = 0xD6F783,
    GOLD        = 0xE4C36A,
    GOLD_BG     = 0x30291A,
    GOLD_LINE   = 0x6B5A32,
    MAX_ROWS    = 5,
    LEFT        = 64,
    RIGHT_EDGE  = 1216,
    PANEL_Y     = 176,
    PANEL_H     = 368,
    NAV_W       = 186,
    MAIN_X      = 268,
    MAIN_W      = 636,
    SIDE_X      = 920,
    SIDE_W      = 296
};

/* A seleção anda até o alvo. No primeiro quadro ela já nasce no lugar. */
static float glide(float *place, float target, int *ready) {
    if (!*ready) {
        *place = target;
        *ready = 1;
        return target;
    }
    const float delta = target - *place;
    if (delta > 0.6f || delta < -0.6f) *place += delta * 0.24f;
    else *place = target;
    return *place;
}

static void action_circle(float x, char key, uint32_t color, const char *label) {
    char letter[] = {key, 0};
    dash_icon(DASH_CIRCLE, x, 636, 28, color);
    dash_text(DASH_BODY, x + 8, 657, 18, WHITE, letter);
    dash_text(DASH_BODY, x + 38, 657, 190, WHITE, label);
}

static void action_bumper(float x, const char *key, const char *label) {
    dash_pill(x, 637, 42, 26, PILL_DIM);
    dash_border(x + 2, 638, 38, 24, 1, LINE);
    const float kw = dash_text_width(DASH_BODY, key);
    dash_text(DASH_BODY, x + (42.0f - kw) * 0.5f, 657, 40, WHITE, key);
    dash_text(DASH_BODY, x + 52, 657, 170, WHITE, label);
}

static void status_badge(float x, float y, const char *label, uint32_t bg, uint32_t fg) {
    const float tw = dash_text_width(DASH_BODY, label);
    const float pw = tw + 26.0f;
    dash_pill(x, y, pw, 28, bg);
    dash_text(DASH_BODY, x + 13.0f, y + 21.0f, pw - 20.0f, fg, label);
}

static const bt_paired_device_t *owner_of(const bt_ipc_state_t *ipc, unsigned player) {
    for (unsigned i = 0; i < BT_MAX_PAIRED_DEVICES; i++) {
        if (ipc->paired[i].valid && ipc->paired[i].player == player)
            return &ipc->paired[i];
    }
    return NULL;
}

static const bt_paired_device_t *paired_at_index(const bt_ipc_state_t *ipc, int idx) {
    int seen = 0;
    for (unsigned i = 0; i < BT_MAX_PAIRED_DEVICES; i++) {
        if (!ipc->paired[i].valid) continue;
        if (seen == idx) return &ipc->paired[i];
        seen++;
    }
    return NULL;
}

static const char *card_name(const bt_ipc_state_t *ipc, unsigned i) {
    const bt_paired_device_t *owner = owner_of(ipc, i + 1u);
    if (owner && owner->name[0]) return owner->name;
    if (ipc->slots[i].connected && ipc->slots[i].name[0]) return ipc->slots[i].name;
    if (owner) return "DualShock 4";
    return i == 0u ? "Controle Oficial / Livre" : "Slot livre";
}

static const char *shown_name(const char *name, uint32_t cod) {
    if (name && name[0]) return name;
    const uint32_t major = (cod >> 8) & 0x1Fu;
    const uint32_t minor = (cod >> 2) & 0x3Fu;
    if (major == 0x05u && minor == 0x02u) return "Controle";
    if (major == 0x05u && minor == 0x10u) return "Teclado";
    if (major == 0x02u) return "Telefone";
    if (major == 0x04u) return "Áudio";
    if (major == 0x01u) return "Computador";
    return "Dispositivo Bluetooth";
}

static void chrome(const bt_ipc_state_t *ipc, const bt_ui_model_t *ui, bool mapping) {
    const bt_ui_pane_t pane = ui->pane;
    const bool content_focused = ui->content_focused;
    const bool identify = ui->identify_mode;
    const int active_bt = bt_manager_active_bt_count(ipc);

    /* Fundo escuro calmo + faixa atmosférica discreta do Xbox */
    dash_rect(0, 0, 1280, 720, BACKGROUND);
    dash_art(DASH_RIBBON, 160, 0, 1120, 420, RIBBON_GLOW);
    dash_rect(0, 0, 1280, 3, LIME);

    /* Cabeçalho esquerdo */
    dash_text(DASH_BODY, LEFT, 54, 420, MUTED, "XBOX 360  ·  ACESSÓRIOS");
    dash_text(DASH_TITLE, LEFT, 108, 420, WHITE, "Bluetooth");

    /* Crédito ao autor alinhado à direita no topo */
    {
        static const char credit[] = "João Carlos Pereira  ·  github.com/JoaoCarlosPereira";
        const float cw = dash_text_width(DASH_BODY, credit);
        dash_text(DASH_BODY, (float)RIGHT_EDGE - cw, 54, cw + 4.0f, MUTED, credit);
    }

    /* Pílula de status global + contador explícito 0..2/2 BT ativos */
    {
        char pill_text[64];
        if (ipc->dongle_ready) {
            snprintf(pill_text, sizeof pill_text, "Adaptador pronto  ·  %d/%u BT ativos",
                     active_bt, (unsigned)BT_MAX_ACTIVE_BT_LINKS);
        } else {
            snprintf(pill_text, sizeof pill_text, "Adaptador offline");
        }
        const float tw = dash_text_width(DASH_BODY, pill_text);
        const float pw = tw + 52.0f;
        const float px = (float)RIGHT_EDGE - pw;
        dash_pill(px, 76, pw, 38, PANEL);
        dash_icon(DASH_CIRCLE, px + 16.0f, 89.0f, 12.0f, ipc->dongle_ready ? LIME : GOLD);
        dash_text(DASH_BODY, px + 36.0f, 102.0f, tw + 8.0f, WHITE, pill_text);
    }

    /* Dica contextual discreta acima do painel central */
    {
        const char *hint = content_focused ? "B ou Esquerda volta ao menu" : "A ou Direita entra no painel";
        uint32_t hint_color = MUTED;
        if (identify && pane == BT_UI_PANE_SLOTS) {
            hint = "Modo identificar: segure SHARE no DualShock 4";
            hint_color = GOLD;
        } else if (pane == BT_UI_PANE_SCAN && bt_manager_paired_count(ipc) >= 4) {
            hint = "Quatro controles pareados (limite da biblioteca)";
            hint_color = GOLD;
        }
        dash_text(DASH_BODY, MAIN_X, 158, MAIN_W, hint_color, hint);
    }

    /* Barra de toast/status acima do rodapé */
    {
        const bool warn = !ipc->dongle_ready || strstr(ipc->status_msg, "Limite") != NULL ||
                          strstr(ipc->status_msg, "Falha") != NULL ||
                          strstr(ipc->status_msg, "desconectado") != NULL;
        dash_rect(LEFT, 566, RIGHT_EDGE - LEFT, 44, PANEL);
        dash_rect(LEFT, 566, 5, 44, warn ? GOLD : LIME);
        dash_text(DASH_BODY, LEFT + 20, 595, RIGHT_EDGE - LEFT - 36, WHITE, ipc->status_msg);
    }

    if (mapping) {
        dash_text(DASH_BODY, LEFT, 657, 500, WHITE, "Back   Cancelar remapeamento");
        return;
    }

    const bt_paired_device_t *slot_owner = NULL;
    if (pane == BT_UI_PANE_SLOTS && content_focused && ui->cursor >= 0 &&
        (uint32_t)ui->cursor < BT_MAX_ACTIVE_SLOTS)
        slot_owner = owner_of(ipc, (unsigned)ui->cursor + 1u);
    const bt_paired_device_t *paired_sel =
        (pane == BT_UI_PANE_PAIRED && content_focused) ? paired_at_index(ipc, ui->cursor) : NULL;
    const bool live_card = pane == BT_UI_PANE_SLOTS && content_focused && ui->cursor >= 0 &&
                           (uint32_t)ui->cursor < BT_MAX_ACTIVE_SLOTS &&
                           (ipc->player_link[ui->cursor] == BT_LINK_UP ||
                            ipc->slots[ui->cursor].connected);

    float x = (float)LEFT;
    if (!content_focused) {
        action_circle(x, 'A', 0x347221, "Abrir");
        x += 165.0f;
    } else if (pane == BT_UI_PANE_SCAN && bt_manager_paired_count(ipc) < 4) {
        action_circle(x, 'A', 0x347221, "Parear");
        x += 180.0f;
    } else if (slot_owner) {
        action_circle(x, 'A', 0x347221, "Trocar jogador");
        x += 225.0f;
    } else if (paired_sel) {
        action_circle(x, 'A', 0x347221, "Conectar");
        x += 185.0f;
    }
    action_circle(x, 'Y', 0xA76608, ipc->scan_active ? "Parar busca" : "Buscar");
    x += ipc->scan_active ? 210.0f : 165.0f;
    if (live_card) {
        action_circle(x, 'X', 0x23638E, "Desconectar");
        x += 205.0f;
    } else if (paired_sel) {
        action_circle(x, 'X', 0x23638E, "Esquecer");
        x += 185.0f;
    }
    if (pane == BT_UI_PANE_SLOTS) {
        action_bumper(x, "LB", identify ? "Parar" : "Identificar");
        x += 205.0f;
    }
    action_circle(x, 'B', 0x8F2C26, (content_focused || identify) ? "Voltar" : "Sair");
}

static void navigation(bt_ui_pane_t pane, bool focused) {
    static const char *labels[] = {"Controles", "Pareados", "Buscar"};
    static const dash_icon_t icons[] = {DASH_GAMEPAD, DASH_BLUETOOTH, DASH_SEARCH};
    static float mark;
    static int ready;
    const float item_h = 84.0f;
    const float stride = 96.0f;
    const float target = (float)PANEL_Y + (float)pane * stride;
    const float ymark = glide(&mark, target, &ready);
    const bool settled = ymark > target - 1.0f && ymark < target + 1.0f;

    for (unsigned i = 0; i < 3; i++) {
        const float y = (float)PANEL_Y + (float)i * stride;
        const bool selected = pane == (bt_ui_pane_t)i;
        const bool lit = focused && settled && selected;
        const uint32_t bg = lit ? LIME : (selected ? PANEL_HI : PANEL);
        const uint32_t fg = lit ? INK : (selected ? WHITE : MUTED);
        dash_rect(LEFT, y, NAV_W, item_h, bg);
        if (selected && !lit) dash_border(LEFT, y, NAV_W, item_h, 1, LINE);
        dash_icon(icons[i], LEFT + 18, y + 24, 34, lit ? INK : (selected ? WHITE : DIM));
        dash_text(DASH_LABEL, LEFT + 62, y + 50, NAV_W - 72, fg, labels[i]);
    }
    if (!(focused && settled))
        dash_rect(LEFT, ymark, 6, item_h, LIME);
}

static void draw_ring_of_light(float x, float y, float size, unsigned player_idx,
                               bool active, bool connecting, bool lit_identify) {
    static const dash_icon_t quads[4] = {DASH_ROL_Q1, DASH_ROL_Q2, DASH_ROL_Q3, DASH_ROL_Q4};
    dash_icon(DASH_ROL_RING, x, y, size, lit_identify ? INK : LINE);
    if (player_idx < 4u && (active || connecting || lit_identify)) {
        const uint32_t qcol = lit_identify ? INK : (connecting ? GOLD : LIME);
        dash_icon(quads[player_idx], x, y, size, qcol);
    }
}

static void player_cards(const bt_ipc_state_t *ipc, int cursor, bool identify) {
    for (unsigned i = 0; i < BT_MAX_ACTIVE_SLOTS; i++) {
        const bool focused = cursor == (int)i;
        const bool lit = identify && ipc->identify_player == (uint8_t)(i + 1u);
        const bt_paired_device_t *owner = owner_of(ipc, i + 1u);
        const uint8_t link = ipc->player_link[i];
        const bool is_up = link == BT_LINK_UP || ipc->slots[i].connected;
        const bool is_conn = link == BT_LINK_CONNECTING;

        const float x = (float)MAIN_X + (float)(i % 2) * 326.0f;
        const float y = (float)PANEL_Y + (float)(i / 2) * 190.0f;
        const float w = 310.0f;
        const float h = 176.0f;
        const char number[] = {(char)('1' + i), 0};
        char title[16];
        snprintf(title, sizeof title, "Jogador %u", i + 1u);

        const uint32_t bg = lit ? GOLD : (focused ? PANEL_HI : PANEL);
        const uint32_t fg = lit ? INK : ((is_up || is_conn || owner || focused) ? WHITE : MUTED);
        const uint32_t sub = lit ? INK : MUTED;

        dash_rect(x, y, w, h, bg);
        if (!focused) dash_border(x, y, w, h, 1, LINE);

        /* Ring of Light no canto superior direito */
        draw_ring_of_light(x + w - 68.0f, y + 18.0f, 50.0f, i, is_up, is_conn, lit);

        /* Número grande + título + nome do controle */
        dash_text(DASH_TITLE, x + 20.0f, y + 66.0f, 44.0f, fg, number);
        dash_text(DASH_LABEL, x + 64.0f, y + 48.0f, 175.0f, fg, title);
        dash_text(DASH_BODY, x + 64.0f, y + 76.0f, 178.0f, sub, card_name(ipc, i));

        /* Badge de estado em pílula */
        if (lit) {
            status_badge(x + 20.0f, y + 102.0f, "ESTE CONTROLE", INK, GOLD);
        } else if (is_up) {
            status_badge(x + 20.0f, y + 102.0f, "BT CONECTADO", LIME, INK);
        } else if (is_conn) {
            status_badge(x + 20.0f, y + 102.0f, "CONECTANDO", GOLD, INK);
        } else if (owner) {
            status_badge(x + 20.0f, y + 102.0f, "PAREADO", PILL_DIM, WHITE);
        } else {
            status_badge(x + 20.0f, y + 102.0f, i == 0u ? "SEM BT" : "LIVRE", PILL_DIM, MUTED);
        }

        /* Sufixo MAC quando há controle pareado no slot */
        if (owner) {
            char mac[16];
            snprintf(mac, sizeof mac, "%02X:%02X:%02X",
                     owner->bd_addr[2], owner->bd_addr[1], owner->bd_addr[0]);
            dash_text(DASH_BODY, x + 22.0f, y + 156.0f, 160.0f, sub, mac);
        }
    }

    /* Focus Ring multi-fator animado (borda perimetral 4px verde-lima) */
    if (cursor >= 0 && cursor < (int)BT_MAX_ACTIVE_SLOTS) {
        static float fx;
        static float fy;
        static int rx;
        static int ry;
        const float tx = (float)MAIN_X + (float)(cursor % 2) * 326.0f;
        const float ty = (float)PANEL_Y + (float)(cursor / 2) * 190.0f;
        dash_border(glide(&fx, tx, &rx), glide(&fy, ty, &ry), 310.0f, 176.0f, 4.0f, LIME);
    }
}

static void inspector_panel(const bt_ipc_state_t *ipc, const bt_ui_model_t *ui) {
    const int active_bt = bt_manager_active_bt_count(ipc);
    const int paired_n = bt_manager_paired_count(ipc);

    dash_rect(SIDE_X, PANEL_Y, SIDE_W, PANEL_H, PANEL);
    dash_border(SIDE_X, PANEL_Y, SIDE_W, PANEL_H, 1, LINE);
    dash_rect(SIDE_X, PANEL_Y, SIDE_W, 3, LIME);

    /* Medidor explícito da capacidade de 2 controles Bluetooth ativos */
    dash_text(DASH_BODY, SIDE_X + 18, PANEL_Y + 34, SIDE_W - 36, MUTED, "CONEXÕES BLUETOOTH");
    for (int s = 0; s < (int)BT_MAX_ACTIVE_BT_LINKS; s++) {
        const float bx = (float)SIDE_X + 18.0f + (float)s * 133.0f;
        const uint32_t col = s < active_bt ? LIME : TRACK;
        dash_rect(bx, PANEL_Y + 46, 127, 14, col);
        if (s >= active_bt) dash_border(bx, PANEL_Y + 46, 127, 14, 1, LINE);
    }
    {
        char cap[40];
        snprintf(cap, sizeof cap, "%d / %u ativos (máximo)", active_bt,
                 (unsigned)BT_MAX_ACTIVE_BT_LINKS);
        dash_text(DASH_BODY, SIDE_X + 18, PANEL_Y + 84, SIDE_W - 36,
                  active_bt >= (int)BT_MAX_ACTIVE_BT_LINKS ? LIME : WHITE, cap);
    }
    dash_rect(SIDE_X + 18, PANEL_Y + 100, SIDE_W - 36, 1, LINE);

    if (ui->pane == BT_UI_PANE_SLOTS) {
        const unsigned idx = (ui->cursor >= 0 && ui->cursor < (int)BT_MAX_ACTIVE_SLOTS)
                                 ? (unsigned)ui->cursor
                                 : 0u;
        const bt_paired_device_t *owner = owner_of(ipc, idx + 1u);
        const uint8_t link = ipc->player_link[idx];
        const bool is_up = link == BT_LINK_UP || ipc->slots[idx].connected;
        const bool is_conn = link == BT_LINK_CONNECTING;

        char head[32];
        snprintf(head, sizeof head, "JOGADOR %u SELECIONADO", idx + 1u);
        dash_text(DASH_BODY, SIDE_X + 18, PANEL_Y + 130, SIDE_W - 36, MUTED, head);
        dash_text(DASH_LABEL, SIDE_X + 18, PANEL_Y + 162, SIDE_W - 36, WHITE, card_name(ipc, idx));

        const char *st = is_up ? "Conectado via Bluetooth"
                         : is_conn ? "Conectando via Bluetooth"
                         : owner ? "Pareado (desconectado)"
                                 : "Sem controle Bluetooth";
        dash_icon(DASH_CIRCLE, SIDE_X + 18, PANEL_Y + 178, 10,
                  is_up ? LIME : (is_conn ? GOLD : DIM));
        dash_text(DASH_BODY, SIDE_X + 36, PANEL_Y + 190, SIDE_W - 54, WHITE, st);

        if (owner) {
            char mac[32];
            snprintf(mac, sizeof mac, "MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                     owner->bd_addr[5], owner->bd_addr[4], owner->bd_addr[3],
                     owner->bd_addr[2], owner->bd_addr[1], owner->bd_addr[0]);
            dash_text(DASH_BODY, SIDE_X + 18, PANEL_Y + 222, SIDE_W - 36, MUTED, mac);
        } else {
            char lib[36];
            snprintf(lib, sizeof lib, "Biblioteca: %d de 4 pareados", paired_n);
            dash_text(DASH_BODY, SIDE_X + 18, PANEL_Y + 222, SIDE_W - 36, MUTED, lib);
        }

        /* Caixa de aviso / dica na base do inspetor */
        const bool full = active_bt >= (int)BT_MAX_ACTIVE_BT_LINKS;
        dash_rect(SIDE_X + 18, PANEL_Y + 256, SIDE_W - 36, 94, full ? GOLD_BG : TRACK);
        dash_border(SIDE_X + 18, PANEL_Y + 256, SIDE_W - 36, 94, 1, full ? GOLD_LINE : LINE);
        if (full) {
            dash_text(DASH_BODY, SIDE_X + 30, PANEL_Y + 284, SIDE_W - 60, GOLD,
                      "Limite de 2 controles BT");
            dash_text(DASH_BODY, SIDE_X + 30, PANEL_Y + 309, SIDE_W - 60, GOLD,
                      "ativos. Desconecte um");
            dash_text(DASH_BODY, SIDE_X + 30, PANEL_Y + 334, SIDE_W - 60, GOLD,
                      "para conectar outro.");
        } else {
            dash_text(DASH_BODY, SIDE_X + 30, PANEL_Y + 284, SIDE_W - 60, WHITE,
                      "Aperte A em um card para");
            dash_text(DASH_BODY, SIDE_X + 30, PANEL_Y + 309, SIDE_W - 60, WHITE,
                      "trocar o jogador, ou LB");
            dash_text(DASH_BODY, SIDE_X + 30, PANEL_Y + 334, SIDE_W - 60, WHITE,
                      "e segure SHARE no DS4.");
        }
    } else if (ui->pane == BT_UI_PANE_PAIRED) {
        const bt_paired_device_t *sel = paired_at_index(ipc, ui->cursor);
        char lib[36];
        snprintf(lib, sizeof lib, "BIBLIOTECA (%d DE 4)", paired_n);
        dash_text(DASH_BODY, SIDE_X + 18, PANEL_Y + 130, SIDE_W - 36, MUTED, lib);
        if (sel) {
            dash_text(DASH_LABEL, SIDE_X + 18, PANEL_Y + 162, SIDE_W - 36, WHITE,
                      shown_name(sel->name, sel->class_of_device));
            char mac[32];
            snprintf(mac, sizeof mac, "MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                     sel->bd_addr[5], sel->bd_addr[4], sel->bd_addr[3],
                     sel->bd_addr[2], sel->bd_addr[1], sel->bd_addr[0]);
            dash_text(DASH_BODY, SIDE_X + 18, PANEL_Y + 194, SIDE_W - 36, MUTED, mac);
            char pl[32];
            if (sel->player >= 1u && sel->player <= 4u)
                snprintf(pl, sizeof pl, "Slot salvo: Jogador %u", sel->player);
            else
                snprintf(pl, sizeof pl, "Sem jogador definido");
            dash_text(DASH_BODY, SIDE_X + 18, PANEL_Y + 224, SIDE_W - 36, WHITE, pl);
        } else {
            dash_text(DASH_BODY, SIDE_X + 18, PANEL_Y + 164, SIDE_W - 36, MUTED,
                      "Nenhum controle salvo.");
        }
        const bool full = active_bt >= (int)BT_MAX_ACTIVE_BT_LINKS;
        dash_rect(SIDE_X + 18, PANEL_Y + 256, SIDE_W - 36, 94, full ? GOLD_BG : TRACK);
        dash_border(SIDE_X + 18, PANEL_Y + 256, SIDE_W - 36, 94, 1, full ? GOLD_LINE : LINE);
        dash_text(DASH_BODY, SIDE_X + 30, PANEL_Y + 284, SIDE_W - 60, full ? GOLD : WHITE,
                  full ? "2/2 BT já estão ativos." : "Você pode salvar até 4");
        dash_text(DASH_BODY, SIDE_X + 30, PANEL_Y + 309, SIDE_W - 60, full ? GOLD : WHITE,
                  full ? "Desconecte um em Controles" : "controles e conectar");
        dash_text(DASH_BODY, SIDE_X + 30, PANEL_Y + 334, SIDE_W - 60, full ? GOLD : WHITE,
                  full ? "para ligar este controle." : "até 2 ao mesmo tempo.");
    } else {
        dash_text(DASH_BODY, SIDE_X + 18, PANEL_Y + 130, SIDE_W - 36, MUTED, "MODO PAREAMENTO");
        dash_text(DASH_LABEL, SIDE_X + 18, PANEL_Y + 162, SIDE_W - 36, WHITE, "DualShock 4");
        dash_text(DASH_BODY, SIDE_X + 18, PANEL_Y + 194, SIDE_W - 36, WHITE,
                  "1. Segure SHARE + PS até");
        dash_text(DASH_BODY, SIDE_X + 18, PANEL_Y + 219, SIDE_W - 36, WHITE,
                  "   a luz piscar rápido.");
        dash_text(DASH_BODY, SIDE_X + 18, PANEL_Y + 244, SIDE_W - 36, WHITE,
                  "2. Aperte Y para buscar.");

        dash_rect(SIDE_X + 18, PANEL_Y + 266, SIDE_W - 36, 84, TRACK);
        dash_border(SIDE_X + 18, PANEL_Y + 266, SIDE_W - 36, 84, 1, LINE);
        dash_icon(DASH_CIRCLE, SIDE_X + 30, PANEL_Y + 286, 10,
                  ipc->scan_active ? LIME : DIM);
        dash_text(DASH_BODY, SIDE_X + 48, PANEL_Y + 298, SIDE_W - 70, WHITE,
                  ipc->scan_active ? "Buscando dispositivos..." : "Busca parada");
        dash_text(DASH_BODY, SIDE_X + 30, PANEL_Y + 330, SIDE_W - 60, MUTED,
                  "Selecione e aperte A.");
    }
}

static void empty_panel(bool searching, bool paired, bool none_found) {
    dash_rect(MAIN_X, PANEL_Y, MAIN_W, PANEL_H, PANEL);
    dash_border(MAIN_X, PANEL_Y, MAIN_W, PANEL_H, 1, LINE);
    dash_rect(MAIN_X, PANEL_Y, 5, PANEL_H, LIME);
    dash_icon(paired ? DASH_GAMEPAD : DASH_BLUETOOTH, MAIN_X + MAIN_W - 140, PANEL_Y + 120, 96, LIME);
    dash_text(DASH_BODY, MAIN_X + 30, PANEL_Y + 58, 410, LIME, paired ? "BIBLIOTECA" : "BLUETOOTH");
    dash_text(DASH_TITLE, MAIN_X + 28, PANEL_Y + 124, 435, WHITE,
              paired ? "Seus controles" : "Encontre");
    if (!paired) dash_text(DASH_TITLE, MAIN_X + 28, PANEL_Y + 174, 435, WHITE, "seu controle");
    dash_text(DASH_BODY, MAIN_X + 30, PANEL_Y + 234, 445, WHITE,
              paired ? "Nenhum controle pareado ainda."
              : searching ? "Procurando dispositivos próximos..."
              : none_found ? "Nenhum dispositivo encontrado."
              : "Coloque o controle em modo de pareamento.");
    dash_text(DASH_BODY, MAIN_X + 30, PANEL_Y + 266, 445, MUTED,
              paired ? "Abra Buscar e aperte Y para procurar."
                     : "No DualShock 4, segure SHARE + PS.");
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

    const int pane_slot = paired ? 0 : 1;
    const bool has_cursor = focused && cursor >= 0 && (unsigned)cursor < count;
    const unsigned selected = has_cursor ? (unsigned)cursor : count;
    const unsigned window = has_cursor ? selected : 0;
    const unsigned start = window < MAX_ROWS ? 0 : window - MAX_ROWS + 1;
    static float row_mark[2];
    static int row_ready[2];
    static unsigned row_window[2];
    float row_y = 0;
    if (has_cursor) {
        if (row_window[pane_slot] != start) {
            row_ready[pane_slot] = 0;
            row_window[pane_slot] = start;
        }
        row_y = glide(&row_mark[pane_slot],
                      (float)PANEL_Y + 60.0f + (float)(selected - start) * 58.0f,
                      &row_ready[pane_slot]);
    }
    dash_rect(MAIN_X, PANEL_Y, MAIN_W, PANEL_H, PANEL);
    dash_border(MAIN_X, PANEL_Y, MAIN_W, PANEL_H, 1, LINE);
    dash_text(DASH_LABEL, MAIN_X + 22, PANEL_Y + 40, 500, WHITE,
              paired ? "Dispositivos pareados" : "Dispositivos próximos");
    char position[24];
    if (has_cursor) snprintf(position, sizeof position, "%u / %u", selected + 1, count);
    else snprintf(position, sizeof position, "%u", count);
    const float pw = dash_text_width(DASH_BODY, position);
    dash_text(DASH_BODY, (float)(MAIN_X + MAIN_W - 22) - pw, PANEL_Y + 39, pw + 4.0f, MUTED, position);

    for (unsigned i = start; i < count && i < start + MAX_ROWS; i++) {
        const float y = (float)PANEL_Y + 60.0f + (float)(i - start) * 58.0f;
        dash_rect(MAIN_X + 16, y, MAIN_W - 32, 50, TRACK);
    }
    if (has_cursor) {
        dash_rect(MAIN_X + 16, row_y, MAIN_W - 32, 50, PANEL_HI);
        dash_border(MAIN_X + 16, row_y, MAIN_W - 32, 50, 3, LIME);
    }
    for (unsigned i = start; i < count && i < start + MAX_ROWS; i++) {
        const char *name = paired ? ipc->paired[pair_indices[i]].name : ipc->scan_items[i].name;
        const uint32_t cod = paired ? ipc->paired[pair_indices[i]].class_of_device
                                    : ipc->scan_items[i].class_of_device;
        const uint8_t *addr = paired ? ipc->paired[pair_indices[i]].bd_addr : ipc->scan_items[i].bd_addr;
        const float y = (float)PANEL_Y + 60.0f + (float)(i - start) * 58.0f;
        const bool on_mark = has_cursor && row_y > y - 20.0f && row_y < y + 20.0f;
        const uint32_t fg = WHITE;
        char address[18];
        snprintf(address, sizeof address, "%02X:%02X:%02X:%02X:%02X:%02X",
                 addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
        dash_text(DASH_LABEL, MAIN_X + 32, y + 33, 370, on_mark ? LIME : fg, shown_name(name, cod));
        if (paired) {
            const uint8_t player = ipc->paired[pair_indices[i]].player;
            const uint8_t link = (player >= 1u && player <= 4u) ? ipc->player_link[player - 1u]
                                                                : BT_LINK_DOWN;
            if (link == BT_LINK_UP) {
                status_badge(MAIN_X + MAIN_W - 182, y + 11, "BT CONECTADO", LIME, INK);
            } else if (link == BT_LINK_CONNECTING) {
                status_badge(MAIN_X + MAIN_W - 172, y + 11, "CONECTANDO", GOLD, INK);
            } else {
                char right[20];
                if (player >= 1u && player <= 4u)
                    snprintf(right, sizeof right, "Jogador %u", player);
                else
                    snprintf(right, sizeof right, "Sem jogador");
                const float rw = dash_text_width(DASH_BODY, right);
                dash_text(DASH_BODY, (float)(MAIN_X + MAIN_W - 30) - rw, y + 32, rw + 4.0f, MUTED, right);
            }
        } else if (bt_manager_find_paired(ipc, addr)) {
            status_badge(MAIN_X + MAIN_W - 150, y + 11, "JÁ PAREADO", GOLD_BG, GOLD);
        } else {
            const float aw = dash_text_width(DASH_BODY, address);
            dash_text(DASH_BODY, (float)(MAIN_X + MAIN_W - 30) - aw, y + 32, aw + 4.0f, MUTED, address);
        }
    }
}

static void remap(const bt_ipc_state_t *ipc, const bt_remap_wizard_t *wizard) {
    const bt_player_slot_t *slot = &ipc->slots[wizard->slot_idx];
    dash_text(DASH_LABEL, LEFT, 160, 1152, LIME, "Remapeamento");
    dash_text(DASH_BODY, LEFT, 192, 1152, WHITE,
              "Pressione o botão do controle para a ação destacada.");
    for (unsigned i = 0; i < BT_X360_MAP_SLOTS; i++) {
        const float x = (float)LEFT + (float)(i / 8) * 584.0f;
        const float y = 214.0f + (float)(i % 8) * 42.0f;
        const bool active = i == wizard->step;
        const uint32_t fg = active ? INK : WHITE;
        dash_rect(x, y, 568, 36, active ? LIME : PANEL);
        dash_text(DASH_BODY, x + 16, y + 25, 250, fg, bt_x360_slot_name(i));
        dash_text(DASH_BODY, x + 278, y + 25, 268, fg,
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
        if (ui->pane == BT_UI_PANE_SLOTS) {
            player_cards(ipc, ui->content_focused ? ui->cursor : -1, ui->identify_mode);
        } else {
            devices(ipc, ui->cursor, ui->pane == BT_UI_PANE_PAIRED, ui->content_focused);
        }
        inspector_panel(ipc, ui);
    }
}
