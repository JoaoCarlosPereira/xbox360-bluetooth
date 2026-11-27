#include "bt_hid_link.h"

#include "../hal/xbox360_usbd.h"
#include "../hci/hci_core.h"
#include "../l2cap/l2cap.h"
#include "../manager/bt_trace.h"

#include <string.h>

typedef struct {
    l2cap_hid_session_t link;
    bool up;
    bool reports_on;
    x360_gamepad_state_t *report;
    bool *ps;
    uint64_t last_output_ms;
    uint8_t output_seq;
    bool output_logged;
} hid_slot_t;

static hid_slot_t g_slots[BT_HID_SLOTS];
static uint8_t g_accum[1024];
static size_t g_accum_len;

static int send_acl(void *ctx, uint16_t acl_handle, const uint8_t *payload, uint16_t len) {
    uint8_t pkt[4 + 512];
    (void)ctx;
    if ((size_t)len + 4u > sizeof pkt) return -1;
    hci_put_le16(pkt, (uint16_t)((acl_handle & 0x0FFFu) | (0x2u << 12)));
    hci_put_le16(pkt + 2, len);
    memcpy(pkt + 4, payload, len);
    return x360_usbd_acl_tx(pkt, (uint16_t)(4u + len));
}

static void on_hid(void *ctx, uint16_t psm, const uint8_t *data, size_t len) {
    hid_slot_t *slot = ctx;
    x360_gamepad_state_t gp;
    if (!slot || psm != L2CAP_PSM_HID_INTERRUPT || !slot->report) return;
    if (ds4_bt_parse_hidp_report(data, len, &gp) != 0) return;
    *slot->report = gp;
    if (slot->ps) *slot->ps = (gp.buttons & X360_BTN_GUIDE) != 0;
}

/* O CRC cobre o byte 0xA2. Esse byte tem de ser o da transação no canal
 * de interrupção. 0x52 no canal de controle faz o DS4 recusar o CRC e desligar. */
static int send_sealed_output(hid_slot_t *slot, uint8_t *frame) {
    frame[0] = 0xA2u;
    ds4_bt_seal_output(frame);
    return l2cap_hid_send(&slot->link, L2CAP_PSM_HID_INTERRUPT, frame, DS4_BT_OUTPUT_REPORT_LEN);
}

static void send_output(hid_slot_t *slot) {
    uint8_t led[DS4_BT_OUTPUT_REPORT_LEN];
    int rc;
    if (slot->link.hid_intr.state != L2CAP_CH_OPEN) return;
    if (ds4_bt_build_rumble_report(led, sizeof led, 0, 0) == 0) return;
    led[2] = (uint8_t)(0xC0u | (slot->output_seq++ & 0x0Fu));
    led[10] = 0x80u; /* barra verde */
    rc = send_sealed_output(slot, led);
    if (!slot->output_logged) {
        slot->output_logged = true;
        bt_trace("hid: output keepalive rc=%d", rc);
    }
}

static void enable_reports(hid_slot_t *slot) {
    uint8_t get_feat[2] = { 0x43u, 0x02u };
    if (slot->reports_on || !slot->up) return;
    if (slot->link.hid_ctrl.state != L2CAP_CH_OPEN || slot->link.hid_intr.state != L2CAP_CH_OPEN)
        return;
    slot->reports_on = true;
    slot->last_output_ms = 0;
    bt_trace("hid: channels open handle=%04X", slot->link.acl_handle);
    l2cap_hid_send(&slot->link, L2CAP_PSM_HID_CONTROL, get_feat, sizeof get_feat);
}

static hid_slot_t *slot_at(int slot) {
    if (slot < 0 || slot >= BT_HID_SLOTS) return NULL;
    return &g_slots[slot];
}

void bt_hid_link_tick(uint64_t now_ms) {
    for (int i = 0; i < BT_HID_SLOTS; i++) {
        hid_slot_t *slot = &g_slots[i];
        if (!slot->reports_on || !slot->up) continue;
        if (slot->link.hid_intr.state != L2CAP_CH_OPEN) continue;
        if (slot->last_output_ms != 0 && now_ms - slot->last_output_ms < 1000u) continue;
        slot->last_output_ms = now_ms;
        send_output(slot);
    }
}

void bt_hid_link_bind(int slot, x360_gamepad_state_t *out_report, bool *out_ps) {
    hid_slot_t *s = slot_at(slot);
    if (!s) return;
    s->report = out_report;
    s->ps = out_ps;
}

int bt_hid_link_start(int slot, uint16_t acl_handle) {
    hid_slot_t *s = slot_at(slot);
    if (!s) return -1;
    bt_hid_link_stop(slot);
    l2cap_hid_init(&s->link, acl_handle, send_acl, on_hid, s);
    /* Cada ACL tem os próprios CIDs. Os dois iguais fazem a resposta cair no outro. */
    s->link.hid_ctrl.local_cid = (uint16_t)(L2CAP_LOCAL_CID_HID_CTRL + (uint16_t)slot * 2u);
    s->link.hid_intr.local_cid = (uint16_t)(L2CAP_LOCAL_CID_HID_INTR + (uint16_t)slot * 2u);
    s->up = true;
    if (l2cap_hid_connect(&s->link) != 0) {
        bt_trace("hid: connect send failed handle=%04X", acl_handle);
        s->up = false;
        return -1;
    }
    bt_trace("hid: connect slot=%d handle=%04X", slot, acl_handle);
    return 0;
}

void bt_hid_link_stop(int slot) {
    hid_slot_t *s = slot_at(slot);
    if (!s) return;
    s->up = false;
    s->reports_on = false;
    s->last_output_ms = 0;
    s->output_logged = false;
}

static hid_slot_t *slot_for_handle(uint16_t handle) {
    const uint16_t id = (uint16_t)(handle & 0x0FFFu);
    for (int i = 0; i < BT_HID_SLOTS; i++) {
        if (!g_slots[i].up) continue;
        if ((g_slots[i].link.acl_handle & 0x0FFFu) == id) return &g_slots[i];
    }
    return NULL;
}

void bt_hid_link_on_acl(const uint8_t *data, size_t len) {
    if (!data || !len) return;
    /* Um pedaço incompleto de um ACL não pode ser colado no pacote do outro. */
    if (len >= 4u && g_accum_len >= 4u) {
        const uint16_t incoming = (uint16_t)(hci_get_le16(data) & 0x0FFFu);
        const uint16_t pending = (uint16_t)(hci_get_le16(g_accum) & 0x0FFFu);
        if (incoming != pending) g_accum_len = 0;
    }
    if (g_accum_len + len > sizeof g_accum) g_accum_len = 0;
    if (g_accum_len + len > sizeof g_accum) return;
    memcpy(g_accum + g_accum_len, data, len);
    g_accum_len += len;
    while (g_accum_len >= 4u) {
        const uint16_t handle = hci_get_le16(g_accum);
        const uint16_t dlen = hci_get_le16(g_accum + 2);
        size_t rest;
        hid_slot_t *slot;
        if ((handle == 0 && dlen == 0) || dlen > 1000u) {
            g_accum_len = 0;
            break;
        }
        if (g_accum_len < 4u + dlen) break;
        slot = slot_for_handle(handle);
        if (slot) {
            l2cap_hid_on_acl_payload(&slot->link, g_accum + 4, dlen);
            enable_reports(slot);
        }
        rest = g_accum_len - (4u + dlen);
        if (rest) memmove(g_accum, g_accum + 4 + dlen, rest);
        g_accum_len = rest;
    }
}

int bt_hid_link_output(int slot, const uint8_t *data, size_t len) {
    hid_slot_t *s = slot_at(slot);
    uint8_t frame[DS4_BT_OUTPUT_REPORT_LEN];
    if (!s || !s->up || !data || !len || s->link.hid_intr.state != L2CAP_CH_OPEN) return -1;
    if (len != DS4_BT_OUTPUT_REPORT_LEN) return -1;
    memcpy(frame, data, len);
    return send_sealed_output(s, frame);
}
