#include "bt_hid_link.h"

#include "../hal/xbox360_usbd.h"
#include "../hci/hci_core.h"
#include "../l2cap/l2cap.h"
#include "../manager/bt_trace.h"

#include <string.h>

static l2cap_hid_session_t link;
static bool up;
static bool reports_on;
static uint8_t accum[1024];
static size_t accum_len;
static x360_gamepad_state_t *report;
static bool *ps;
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
    x360_gamepad_state_t gp;
    (void)ctx;
    if (psm != L2CAP_PSM_HID_INTERRUPT || !report) return;
    if (ds4_bt_parse_hidp_report(data, len, &gp) != 0) return;
    *report = gp;
    if (ps) *ps = (gp.buttons & X360_BTN_GUIDE) != 0;
}

static uint64_t last_output_ms;
static uint8_t output_seq;
static bool output_logged;

/* O CRC cobre o byte 0xA2. Esse byte tem de ser o da transação no canal
 * de interrupção. 0x52 no canal de controle faz o DS4 recusar o CRC e desligar. */
static int send_sealed_output(uint8_t *frame) {
    frame[0] = 0xA2u;
    ds4_bt_seal_output(frame);
    return l2cap_hid_send(&link, L2CAP_PSM_HID_INTERRUPT, frame, DS4_BT_OUTPUT_REPORT_LEN);
}

static void send_output(void) {
    uint8_t led[DS4_BT_OUTPUT_REPORT_LEN];
    int rc;
    if (link.hid_intr.state != L2CAP_CH_OPEN) return;
    if (ds4_bt_build_rumble_report(led, sizeof led, 0, 0) == 0) return;
    led[2] = (uint8_t)(0xC0u | (output_seq++ & 0x0Fu));
    led[10] = 0x80u; /* barra verde */
    rc = send_sealed_output(led);
    if (!output_logged) {
        output_logged = true;
        bt_trace("hid: output keepalive rc=%d", rc);
    }
}

static void enable_reports(void) {
    uint8_t get_feat[2] = { 0x43u, 0x02u };
    if (reports_on || !up) return;
    if (link.hid_ctrl.state != L2CAP_CH_OPEN || link.hid_intr.state != L2CAP_CH_OPEN) return;
    reports_on = true;
    last_output_ms = 0;
    bt_trace("hid: channels open");
    l2cap_hid_send(&link, L2CAP_PSM_HID_CONTROL, get_feat, sizeof get_feat);
}

void bt_hid_link_tick(uint64_t now_ms) {
    if (!reports_on || !up) return;
    if (link.hid_intr.state != L2CAP_CH_OPEN) return;
    if (last_output_ms != 0 && now_ms - last_output_ms < 1000u) return;
    last_output_ms = now_ms;
    send_output();
}

void bt_hid_link_bind(x360_gamepad_state_t *out_report, bool *out_ps) {
    report = out_report;
    ps = out_ps;
}

int bt_hid_link_start(uint16_t acl_handle) {
    bt_hid_link_stop();
    l2cap_hid_init(&link, acl_handle, send_acl, on_hid, NULL);
    up = true;
    if (l2cap_hid_connect(&link) != 0) {
        bt_trace("hid: connect send failed handle=%04X", acl_handle);
        up = false;
        return -1;
    }
    bt_trace("hid: connect handle=%04X", acl_handle);
    return 0;
}

void bt_hid_link_stop(void) {
    up = false;
    reports_on = false;
    last_output_ms = 0;
    output_logged = false;
    accum_len = 0;
}

void bt_hid_link_on_acl(const uint8_t *data, size_t len) {
    if (!up || !data || !len) return;
    if (accum_len + len > sizeof accum) accum_len = 0;
    if (accum_len + len > sizeof accum) return;
    memcpy(accum + accum_len, data, len);
    accum_len += len;
    while (accum_len >= 4u) {
        const uint16_t handle = hci_get_le16(accum);
        const uint16_t dlen = hci_get_le16(accum + 2);
        size_t rest;
        if ((handle == 0 && dlen == 0) || dlen > 1000u) {
            accum_len = 0;
            break;
        }
        if (accum_len < 4u + dlen) break;
        l2cap_hid_on_acl_payload(&link, accum + 4, dlen);
        rest = accum_len - (4u + dlen);
        if (rest) memmove(accum, accum + 4 + dlen, rest);
        accum_len = rest;
        enable_reports();
    }
}

int bt_hid_link_output(const uint8_t *data, size_t len) {
    uint8_t frame[DS4_BT_OUTPUT_REPORT_LEN];
    if (!up || !data || !len || link.hid_intr.state != L2CAP_CH_OPEN) return -1;
    if (len != DS4_BT_OUTPUT_REPORT_LEN) return -1;
    memcpy(frame, data, len);
    return send_sealed_output(frame);
}
