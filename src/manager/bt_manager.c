#include "bt_manager.h"
#include "../hid/ds4_bt.h"
#include "bt_phase1.h"
#include <string.h>

void bt_ipc_init(bt_ipc_state_t *ipc) {
    if (!ipc) return;
    memset(ipc, 0, sizeof(*ipc));
    ipc->magic   = BT_IPC_MAGIC;
    ipc->version = BT_IPC_VERSION;
    memcpy(ipc->status_msg, "Ready", 6u);
}

int bt_manager_upsert_paired(bt_ipc_state_t *ipc, const uint8_t bd_addr[6],
                             const uint8_t link_key[16], uint8_t key_type,
                             uint32_t cod, const char *name) {
    if (!ipc || !bd_addr || !link_key) return -1;

    int free_idx = -1;
    for (size_t i = 0; i < BT_MAX_PAIRED_DEVICES; i++) {
        if (ipc->paired[i].valid && memcmp(ipc->paired[i].bd_addr, bd_addr, 6u) == 0) {
            free_idx = (int)i;
            break;
        }
        if (!ipc->paired[i].valid && free_idx < 0) {
            free_idx = (int)i;
        }
    }
    if (free_idx < 0) return -2;

    bt_paired_device_t *d = &ipc->paired[free_idx];
    d->valid           = true;
    d->key_type        = key_type;
    d->class_of_device = cod;
    memcpy(d->bd_addr, bd_addr, 6u);
    memcpy(d->link_key, link_key, 16u);
    if (name && name[0]) {
        size_t n = 0;
        while (n + 1 < sizeof(d->name) && name[n]) {
            d->name[n] = name[n];
            n++;
        }
        d->name[n] = '\0';
    }
    return free_idx;
}

const bt_paired_device_t *bt_manager_find_paired(const bt_ipc_state_t *ipc,
                                                 const uint8_t bd_addr[6]) {
    if (!ipc || !bd_addr) return NULL;
    for (size_t i = 0; i < BT_MAX_PAIRED_DEVICES; i++) {
        if (ipc->paired[i].valid && memcmp(ipc->paired[i].bd_addr, bd_addr, 6u) == 0) {
            return &ipc->paired[i];
        }
    }
    return NULL;
}

int bt_manager_forget_paired(bt_ipc_state_t *ipc, const uint8_t bd_addr[6]) {
    if (!ipc || !bd_addr) return -1;
    for (size_t i = 0; i < BT_MAX_PAIRED_DEVICES; i++) {
        if (ipc->paired[i].valid && memcmp(ipc->paired[i].bd_addr, bd_addr, 6u) == 0) {
            memset(&ipc->paired[i], 0, sizeof(ipc->paired[i]));
            return 0;
        }
    }
    return -2;
}

int bt_manager_paired_count(const bt_ipc_state_t *ipc) {
    int n = 0;
    if (!ipc) return 0;
    for (size_t i = 0; i < BT_MAX_PAIRED_DEVICES; i++) {
        if (ipc->paired[i].valid) n++;
    }
    return n;
}

static uint8_t free_player_except(const bt_ipc_state_t *ipc, uint8_t avoid) {
    for (uint8_t p = 1u; p <= 4u; p++) {
        if (p == avoid) continue;
        bool used = false;
        for (size_t i = 0; i < BT_MAX_PAIRED_DEVICES; i++) {
            if (ipc->paired[i].valid && ipc->paired[i].player == p) used = true;
        }
        if (!used) return p;
    }
    return 0u;
}

int bt_manager_store_single_ds4(bt_ipc_state_t *ipc, const uint8_t bd_addr[6],
                                const uint8_t link_key[16], uint8_t key_type,
                                uint8_t player, const char *name) {
    if (!ipc || !bd_addr || !link_key || player < 1u || player > 4u) return -1;

    int others = 0;
    int self = -1;
    uint8_t previous = 0u;
    for (size_t i = 0; i < BT_MAX_PAIRED_DEVICES; i++) {
        if (!ipc->paired[i].valid) continue;
        if (memcmp(ipc->paired[i].bd_addr, bd_addr, 6u) == 0) {
            self = (int)i;
            previous = ipc->paired[i].player;
        } else {
            others++;
        }
    }
    if (self < 0 && others >= 4) return -3;

    for (size_t i = 0; i < BT_MAX_PAIRED_DEVICES; i++) {
        if (!ipc->paired[i].valid || memcmp(ipc->paired[i].bd_addr, bd_addr, 6u) == 0) continue;
        if (ipc->paired[i].player != player) continue;
        ipc->paired[i].player = (previous >= 1u && previous <= 4u && previous != player)
                                    ? previous
                                    : free_player_except(ipc, player);
        break;
    }

    int idx = bt_manager_upsert_paired(ipc, bd_addr, link_key, key_type, 0x002508u, name);
    if (idx < 0) return idx;
    ipc->paired[idx].player = player;
    return idx;
}

int bt_manager_set_player(bt_ipc_state_t *ipc, const uint8_t bd_addr[6], uint8_t player) {
    if (!ipc || !bd_addr || player < 1u || player > 4u) return -1;
    int self = -1;
    for (size_t i = 0; i < BT_MAX_PAIRED_DEVICES; i++) {
        if (ipc->paired[i].valid && memcmp(ipc->paired[i].bd_addr, bd_addr, 6u) == 0) {
            self = (int)i;
            break;
        }
    }
    if (self < 0) return -1;
    const uint8_t previous = ipc->paired[self].player;
    if (previous == player) return self;
    for (size_t i = 0; i < BT_MAX_PAIRED_DEVICES; i++) {
        if ((int)i == self || !ipc->paired[i].valid) continue;
        if (ipc->paired[i].player != player) continue;
        ipc->paired[i].player = (previous >= 1u && previous <= 4u) ? previous : 0u;
        break;
    }
    ipc->paired[self].player = player;
    return self;
}

size_t bt_manager_build_link_key_reply(const bt_ipc_state_t *ipc, const uint8_t bd_addr[6],
                                       uint8_t *out_buf, size_t out_cap) {
    const bt_paired_device_t *d = bt_manager_find_paired(ipc, bd_addr);
    if (!d) return 0;
    return hci_build_link_key_reply(out_buf, out_cap, d->bd_addr, d->link_key);
}

int bt_manager_assign_slot(bt_ipc_state_t *ipc, const uint8_t bd_addr[6],
                           uint16_t acl_handle, const char *name) {
    if (!ipc || !bd_addr) return -1;

    int free_slot = -1;
    for (size_t i = 0; i < BT_MAX_ACTIVE_SLOTS; i++) {
        if (ipc->slots[i].connected && memcmp(ipc->slots[i].bd_addr, bd_addr, 6u) == 0) {
            free_slot = (int)i;
            break;
        }
        if (!ipc->slots[i].connected && free_slot < 0) {
            free_slot = (int)i;
        }
    }
    if (free_slot < 0) return -2;

    bt_player_slot_t *s = &ipc->slots[free_slot];
    memset(s, 0, sizeof(*s));
    s->connected  = true;
    s->acl_handle = acl_handle;
    memcpy(s->bd_addr, bd_addr, 6u);
    bt_button_map_init_ds4_default(&s->map);
    if (name && name[0]) {
        size_t n = 0;
        while (n + 1 < sizeof(s->name) && name[n]) {
            s->name[n] = name[n];
            n++;
        }
        s->name[n] = '\0';
    }
    return free_slot;
}

int bt_manager_release_slot(bt_ipc_state_t *ipc, uint32_t slot_idx) {
    if (!ipc || slot_idx >= BT_MAX_ACTIVE_SLOTS) return -1;
    memset(&ipc->slots[slot_idx], 0, sizeof(ipc->slots[slot_idx]));
    return 0;
}

int bt_manager_find_slot_by_handle(const bt_ipc_state_t *ipc, uint16_t acl_handle) {
    if (!ipc) return -1;
    for (size_t i = 0; i < BT_MAX_ACTIVE_SLOTS; i++) {
        if (ipc->slots[i].connected && ipc->slots[i].acl_handle == acl_handle) {
            return (int)i;
        }
    }
    return -1;
}

static char hex_digit(uint8_t v) {
    v &= 0x0Fu;
    return (char)(v < 10u ? ((unsigned)'0' + v) : ((unsigned)'A' + (v - 10u)));
}

static int parse_hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

static size_t append_str(char *dst, size_t cap, size_t pos, const char *s) {
    while (*s && pos + 1 < cap) {
        dst[pos++] = *s++;
    }
    return pos;
}

static size_t append_hex_bytes(char *dst, size_t cap, size_t pos,
                               const uint8_t *bytes, size_t n, bool reverse) {
    for (size_t i = 0; i < n && pos + 2 < cap; i++) {
        uint8_t b = reverse ? bytes[n - 1u - i] : bytes[i];
        dst[pos++] = hex_digit((uint8_t)(b >> 4));
        dst[pos++] = hex_digit(b);
    }
    return pos;
}

size_t bt_manager_export_ini(const bt_ipc_state_t *ipc, char *out_buf, size_t cap) {
    if (!ipc || !out_buf || cap < 2) return 0;
    size_t pos = 0;
    pos = append_str(out_buf, cap, pos, "# bt360 paired controllers\n");

    for (size_t i = 0; i < BT_MAX_PAIRED_DEVICES; i++) {
        const bt_paired_device_t *d = &ipc->paired[i];
        if (!d->valid) continue;

        /* Format: dev=<BD_ADDR_12HEX>,<LINKKEY_32HEX>,<KEYTYPE_2HEX>,<NAME>\n */
        pos = append_str(out_buf, cap, pos, "dev=");
        pos = append_hex_bytes(out_buf, cap, pos, d->bd_addr, 6u, true);
        pos = append_str(out_buf, cap, pos, ",");
        pos = append_hex_bytes(out_buf, cap, pos, d->link_key, 16u, false);
        pos = append_str(out_buf, cap, pos, ",");
        pos = append_hex_bytes(out_buf, cap, pos, &d->key_type, 1u, false);
        pos = append_str(out_buf, cap, pos, ",");
        pos = append_str(out_buf, cap, pos, d->name[0] ? d->name : "Controller");
        pos = append_str(out_buf, cap, pos, "\n");

        /* Format: player=<BD_ADDR_12HEX>,<PLAYER_2HEX>\n (jogador 1..4) */
        if (d->player >= 1u && d->player <= 4u) {
            pos = append_str(out_buf, cap, pos, "player=");
            pos = append_hex_bytes(out_buf, cap, pos, d->bd_addr, 6u, true);
            pos = append_str(out_buf, cap, pos, ",");
            pos = append_hex_bytes(out_buf, cap, pos, &d->player, 1u, false);
            pos = append_str(out_buf, cap, pos, "\n");
        }
    }
    out_buf[pos] = '\0';
    return pos;
}

static bool parse_hex_seq(const char *s, uint8_t *out, size_t nbytes, bool reverse) {
    for (size_t i = 0; i < nbytes; i++) {
        int hi = parse_hex_nibble(s[i * 2u]);
        int lo = parse_hex_nibble(s[i * 2u + 1u]);
        if (hi < 0 || lo < 0) return false;
        uint8_t b = (uint8_t)((hi << 4) | lo);
        out[reverse ? (nbytes - 1u - i) : i] = b;
    }
    return true;
}

int bt_manager_import_ini(bt_ipc_state_t *ipc, const char *ini_text, size_t len) {
    if (!ipc || !ini_text) return -1;
    int loaded = 0;
    size_t pos = 0;

    while (pos < len) {
        size_t line_start = pos;
        while (pos < len && ini_text[pos] != '\n' && ini_text[pos] != '\r') {
            pos++;
        }
        size_t line_len = pos - line_start;
        while (pos < len && (ini_text[pos] == '\n' || ini_text[pos] == '\r')) {
            pos++;
        }

        const char *line = ini_text + line_start;
        /* Minimum: "dev=" (4) + bd(12) + ","(1) + lk(32) + ","(1) + kt(2) + ","(1) = 53 */
        if (line_len >= 53u && memcmp(line, "dev=", 4u) == 0 &&
            line[16] == ',' && line[49] == ',' && line[52] == ',') {
            uint8_t bd[6], lk[16], kt = 0;
            if (parse_hex_seq(line + 4u, bd, 6u, true) &&
                parse_hex_seq(line + 17u, lk, 16u, false) &&
                parse_hex_seq(line + 50u, &kt, 1u, false)) {
                char name[32];
                size_t nlen = line_len - 53u;
                if (nlen >= sizeof(name)) nlen = sizeof(name) - 1u;
                memcpy(name, line + 53u, nlen);
                name[nlen] = '\0';
                if (bt_manager_paired_count(ipc) >= 4 &&
                    bt_manager_find_paired(ipc, bd) == NULL) {
                    /* Quatro enderecos ja ocupam a lista. */
                } else if (bt_manager_upsert_paired(ipc, bd, lk, kt, 0x002508u, name) >= 0) {
                    loaded++;
                }
            }
        } else if (line_len >= 22u && memcmp(line, "player=", 7u) == 0 && line[19] == ',') {
            uint8_t bd[6], pl = 0;
            if (parse_hex_seq(line + 7u, bd, 6u, true) &&
                parse_hex_seq(line + 20u, &pl, 1u, false) && pl >= 1u && pl <= 4u) {
                for (size_t i = 0; i < BT_MAX_PAIRED_DEVICES; i++) {
                    if (ipc->paired[i].valid && memcmp(ipc->paired[i].bd_addr, bd, 6u) == 0) {
                        ipc->paired[i].player = pl;
                        break;
                    }
                }
            }
        }
    }
    return loaded;
}

size_t bt_scan_pack(uint8_t *dst, size_t cap, const hci_inquiry_item_t *items,
                    uint32_t count, bool scanning) {
    if (!dst || cap < BT_SCAN_FILE_BYTES) return 0;
    if (!items) count = 0;
    if (count > BT_MAX_SCAN_ITEMS) count = BT_MAX_SCAN_ITEMS;
    memset(dst, 0, BT_SCAN_FILE_BYTES);
    const uint32_t magic = BT_SCAN_FILE_MAGIC;
    const uint32_t active = scanning ? 1u : 0u;
    memcpy(dst + 0, &magic, 4u);
    memcpy(dst + 4, &count, 4u);
    memcpy(dst + 8, &active, 4u);
    for (uint32_t i = 0; i < count; i++) {
        uint8_t *p = dst + 16u + (size_t)i * 44u;
        memcpy(p, items[i].bd_addr, 6u);
        memcpy(p + 8, &items[i].class_of_device, 4u);
        memcpy(p + 12, items[i].name, 32u);
        p[43] = 0;
    }
    return BT_SCAN_FILE_BYTES;
}

bool bt_scan_apply(bt_ipc_state_t *ipc, const uint8_t *src, size_t len) {
    if (!ipc || !src || len < 16u) return false;
    uint32_t magic = 0, count = 0, scanning = 0;
    memcpy(&magic, src + 0, 4u);
    memcpy(&count, src + 4, 4u);
    memcpy(&scanning, src + 8, 4u);
    if (magic != BT_SCAN_FILE_MAGIC) return false;
    if (count > BT_MAX_SCAN_ITEMS) count = BT_MAX_SCAN_ITEMS;
    if (len < 16u + (size_t)count * 44u) return false;
    memset(ipc->scan_items, 0, sizeof ipc->scan_items);
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *p = src + 16u + (size_t)i * 44u;
        hci_inquiry_item_t *dst = &ipc->scan_items[i];
        memcpy(dst->bd_addr, p, 6u);
        memcpy(&dst->class_of_device, p + 8, 4u);
        memcpy(dst->name, p + 12, 31u);
        dst->name[31] = '\0';
    }
    ipc->scan_count = count;
    ipc->scan_active = scanning != 0;
    return true;
}

void bt_ipc_issue_cmd(bt_ipc_state_t *ipc, bt_ipc_cmd_t cmd,
                      const uint8_t *bd_addr, uint32_t slot) {
    if (!ipc) return;
    ipc->cmd = (uint32_t)cmd;
    ipc->cmd_slot = slot;
    if (bd_addr) {
        memcpy(ipc->cmd_bd_addr, bd_addr, 6u);
    } else {
        memset(ipc->cmd_bd_addr, 0, 6u);
    }
    ipc->cmd_seq++;
}

static int pane_item_count(const bt_ipc_state_t *ipc, bt_ui_pane_t pane) {
    if (pane == BT_UI_PANE_SLOTS) return (int)BT_MAX_ACTIVE_SLOTS;
    if (pane == BT_UI_PANE_PAIRED) {
        int c = 0;
        for (size_t i = 0; i < BT_MAX_PAIRED_DEVICES; i++) {
            if (ipc->paired[i].valid) c++;
        }
        return c > 0 ? c : 1;
    }
    return (ipc->scan_count > 0) ? (int)ipc->scan_count : 1;
}

static const bt_paired_device_t *paired_at(const bt_ipc_state_t *ipc, int idx) {
    int c = 0;
    for (size_t i = 0; i < BT_MAX_PAIRED_DEVICES; i++) {
        if (ipc->paired[i].valid) {
            if (c == idx) return &ipc->paired[i];
            c++;
        }
    }
    return NULL;
}

bool bt_manager_has_stored_ds4(const bt_ipc_state_t *ipc) {
    if (!ipc) return false;
    for (size_t i = 0; i < BT_MAX_PAIRED_DEVICES; i++) {
        if (ipc->paired[i].valid) return true;
    }
    return false;
}

bool bt_ui_apply_reply_phrase(bt_ipc_state_t *ipc, uint32_t phrase_id) {
    if (!ipc) return false;
    if (phrase_id < BT_PHRASE_DS4_PAIRED_PLAYER1 || phrase_id > BT_PHRASE_DS4_DISCONNECTED)
        return false;
    const char *text = bt_phase1_phrase_text(phrase_id);
    if (!text) return false;
    size_t n = 0;
    while (text[n]) n++;
    if (n >= sizeof(ipc->status_msg)) return false; /* nunca truncar a frase da tabela */
    memcpy(ipc->status_msg, text, n + 1u);
    return true;
}

void bt_ui_model_init(bt_ui_model_t *m) {
    if (!m) return;
    m->pane = BT_UI_PANE_SCAN;
    m->cursor = 0;
    m->content_focused = false;
    m->exit_requested = false;
    m->identify_mode = false;
}

static void move_pane(bt_ui_model_t *m, int delta) {
    int pane = (int)m->pane + delta;
    if (pane < (int)BT_UI_PANE_SLOTS || pane > (int)BT_UI_PANE_SCAN) return;
    m->pane = (bt_ui_pane_t)pane;
    m->cursor = 0;
}

/* Menu à esquerda é uma coluna. Controles é uma grade 2x2. Listas são uma coluna. */
static void move_focus(bt_ui_model_t *m, const bt_ipc_state_t *ipc, uint16_t btns) {
    if (!m->content_focused) {
        if (btns & X360_BTN_DPAD_UP) move_pane(m, -1);
        if (btns & X360_BTN_DPAD_DOWN) move_pane(m, 1);
        if (btns & X360_BTN_DPAD_RIGHT) {
            m->content_focused = true;
            m->cursor = 0;
        }
        return;
    }

    int count = pane_item_count(ipc, m->pane);
    if (m->pane == BT_UI_PANE_SLOTS) {
        if ((btns & X360_BTN_DPAD_UP) && m->cursor >= 2) m->cursor -= 2;
        if ((btns & X360_BTN_DPAD_DOWN) && m->cursor + 2 < count) m->cursor += 2;
        if (btns & X360_BTN_DPAD_LEFT) {
            if (m->cursor % 2 == 1) m->cursor -= 1;
            else m->content_focused = false;
        }
        if ((btns & X360_BTN_DPAD_RIGHT) && (m->cursor % 2) == 0 && m->cursor + 1 < count)
            m->cursor += 1;
        return;
    }

    if ((btns & X360_BTN_DPAD_UP) && m->cursor > 0) m->cursor--;
    if ((btns & X360_BTN_DPAD_DOWN) && m->cursor + 1 < count) m->cursor++;
    if (btns & X360_BTN_DPAD_LEFT) m->content_focused = false;
}

void bt_ui_handle_input(bt_ui_model_t *m, bt_ipc_state_t *ipc, uint16_t btns) {
    if (!m || !ipc) return;

    if (btns & X360_BTN_B) {
        m->exit_requested = true;
        return;
    }
    move_focus(m, ipc, btns);

    if (btns & X360_BTN_Y) {
        bt_ipc_issue_cmd(ipc, ipc->scan_active ? BT_IPC_CMD_STOP_SCAN : BT_IPC_CMD_START_SCAN,
                         NULL, 0);
    }
    if ((btns & X360_BTN_LEFT_SHOULDER) && m->pane == BT_UI_PANE_SLOTS)
        m->identify_mode = !m->identify_mode;
    /* Ate quatro controles. A na grade Controles troca o jogador do card focado. */
    if ((btns & X360_BTN_A) && m->content_focused) {
        if (m->pane == BT_UI_PANE_SCAN) {
            if (bt_manager_paired_count(ipc) < 4) {
                const bool on_item = m->cursor >= 0 && (uint32_t)m->cursor < ipc->scan_count;
                bt_ipc_issue_cmd(ipc, BT_IPC_CMD_PAIR_AND_CONNECT,
                                 on_item ? ipc->scan_items[m->cursor].bd_addr : NULL, 0);
            }
        } else if (m->pane == BT_UI_PANE_SLOTS && m->cursor >= 0 &&
                   (uint32_t)m->cursor < BT_MAX_ACTIVE_SLOTS) {
            const uint8_t here = (uint8_t)(m->cursor + 1);
            const bt_paired_device_t *owner = NULL;
            for (size_t i = 0; i < BT_MAX_PAIRED_DEVICES; i++) {
                if (ipc->paired[i].valid && ipc->paired[i].player == here) {
                    owner = &ipc->paired[i];
                    break;
                }
            }
            if (owner) {
                const uint8_t next = here == 4u ? 1u : (uint8_t)(here + 1u);
                if (bt_manager_set_player(ipc, owner->bd_addr, next) >= 0)
                    bt_ipc_issue_cmd(ipc, BT_IPC_CMD_SET_PLAYER, owner->bd_addr, next);
            }
        } else if (m->pane == BT_UI_PANE_PAIRED) {
            const bt_paired_device_t *pd = paired_at(ipc, m->cursor);
            if (pd) {
                bt_ipc_issue_cmd(ipc, BT_IPC_CMD_CONNECT_PAIRED, pd->bd_addr, 0);
            }
        }
    }
    if ((btns & X360_BTN_X) && m->content_focused) {
        if (m->pane == BT_UI_PANE_SLOTS && m->cursor >= 0 &&
            (uint32_t)m->cursor < BT_MAX_ACTIVE_SLOTS &&
            (ipc->slots[m->cursor].connected ||
             (ipc->pad_link == BT_LINK_UP &&
              ipc->live_player == (uint8_t)(m->cursor + 1)))) {
            bt_ipc_issue_cmd(ipc, BT_IPC_CMD_DISCONNECT_SLOT, NULL, (uint32_t)m->cursor);
        } else if (m->pane == BT_UI_PANE_PAIRED) {
            const bt_paired_device_t *pd = paired_at(ipc, m->cursor);
            if (pd) {
                bt_ipc_issue_cmd(ipc, BT_IPC_CMD_FORGET_PAIRED, pd->bd_addr, 0);
            }
        }
    }
}

void bt_remap_wizard_start(bt_remap_wizard_t *w, uint32_t slot_idx, uint32_t current_raw_mask) {
    if (!w) return;
    w->active        = true;
    w->slot_idx      = slot_idx;
    w->step          = 0;
    w->prev_raw_mask = current_raw_mask;
    w->last_raw_mask = current_raw_mask;
}

bool bt_remap_wizard_feed_raw(bt_remap_wizard_t *w, uint32_t raw_mask, bt_button_map_t *map) {
    if (!w || !w->active || !map) return false;

    uint32_t newly_pressed = raw_mask & ~w->prev_raw_mask;
    w->prev_raw_mask = raw_mask;
    w->last_raw_mask = raw_mask;
    if (newly_pressed == 0) return false;

    for (uint8_t bit = 0; bit < BT_RAW_BIT_COUNT; bit++) {
        if (newly_pressed & (1u << bit)) {
            map->raw_bit_for_x360[w->step] = bit;
            w->step++;
            if (w->step >= BT_X360_MAP_SLOTS) {
                w->active = false;
            }
            return true;
        }
    }
    return false;
}
