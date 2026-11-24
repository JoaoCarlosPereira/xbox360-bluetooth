#include "l2cap.h"
#include "../hci/hci_core.h"
#include <string.h>

static int send_l2cap_pdu(l2cap_hid_session_t *s, uint16_t remote_cid,
                          const uint8_t *payload, uint16_t len) {
    if (!s || !s->send_acl || (size_t)(4u + len) > 512u) {
        return -1;
    }
    uint8_t buf[512];
    hci_put_le16(buf + 0, len);
    hci_put_le16(buf + 2, remote_cid);
    if (len > 0 && payload) {
        memcpy(buf + 4, payload, len);
    }
    return s->send_acl(s->cb_ctx, s->acl_handle, buf, (uint16_t)(4u + len));
}

static uint8_t alloc_sig_id(l2cap_hid_session_t *s) {
    uint8_t id = s->next_sig_id++;
    if (s->next_sig_id == 0) {
        s->next_sig_id = 1;
    }
    return id;
}

static int send_sig_conn_req(l2cap_hid_session_t *s, l2cap_channel_t *ch) {
    uint8_t cmd[8];
    cmd[0] = L2CAP_CONN_REQ;
    cmd[1] = alloc_sig_id(s);
    hci_put_le16(cmd + 2, 4u);
    hci_put_le16(cmd + 4, ch->psm);
    hci_put_le16(cmd + 6, ch->local_cid);
    ch->state = L2CAP_CH_WAIT_CONN_RSP;
    return send_l2cap_pdu(s, L2CAP_CID_SIGNALING, cmd, sizeof(cmd));
}

static int send_sig_conn_rsp(l2cap_hid_session_t *s, uint8_t sig_id,
                             uint16_t dcid, uint16_t scid,
                             uint16_t result, uint16_t status) {
    uint8_t cmd[12];
    cmd[0] = L2CAP_CONN_RSP;
    cmd[1] = sig_id;
    hci_put_le16(cmd + 2, 8u);
    hci_put_le16(cmd + 4, dcid);
    hci_put_le16(cmd + 6, scid);
    hci_put_le16(cmd + 8, result);
    hci_put_le16(cmd + 10, status);
    return send_l2cap_pdu(s, L2CAP_CID_SIGNALING, cmd, sizeof(cmd));
}

static int send_sig_conf_req(l2cap_hid_session_t *s, l2cap_channel_t *ch) {
    uint8_t cmd[8];
    cmd[0] = L2CAP_CONF_REQ;
    cmd[1] = alloc_sig_id(s);
    hci_put_le16(cmd + 2, 4u);
    hci_put_le16(cmd + 4, ch->remote_cid);
    hci_put_le16(cmd + 6, 0x0000u); /* Flags: no continuation */
    return send_l2cap_pdu(s, L2CAP_CID_SIGNALING, cmd, sizeof(cmd));
}

static int send_sig_conf_rsp(l2cap_hid_session_t *s, uint8_t sig_id,
                             uint16_t scid, uint16_t result) {
    uint8_t cmd[10];
    cmd[0] = L2CAP_CONF_RSP;
    cmd[1] = sig_id;
    hci_put_le16(cmd + 2, 6u);
    hci_put_le16(cmd + 4, scid);
    hci_put_le16(cmd + 6, 0x0000u);
    hci_put_le16(cmd + 8, result);
    return send_l2cap_pdu(s, L2CAP_CID_SIGNALING, cmd, sizeof(cmd));
}

static l2cap_channel_t *find_by_local_cid(l2cap_hid_session_t *s, uint16_t cid) {
    if (cid == s->hid_ctrl.local_cid) return &s->hid_ctrl;
    if (cid == s->hid_intr.local_cid) return &s->hid_intr;
    return NULL;
}

static void maybe_mark_open(l2cap_channel_t *ch) {
    if (ch->local_conf_done && ch->remote_conf_done) {
        ch->state = L2CAP_CH_OPEN;
    }
}

void l2cap_hid_init(l2cap_hid_session_t *s, uint16_t acl_handle,
                    l2cap_send_acl_fn send_acl, l2cap_on_hid_data_fn on_hid_data,
                    void *cb_ctx) {
    if (!s) return;
    memset(s, 0, sizeof(*s));
    s->acl_handle  = acl_handle;
    s->next_sig_id = 1;
    s->send_acl    = send_acl;
    s->on_hid_data = on_hid_data;
    s->cb_ctx      = cb_ctx;

    s->hid_ctrl.psm       = L2CAP_PSM_HID_CONTROL;
    s->hid_ctrl.local_cid = L2CAP_LOCAL_CID_HID_CTRL;
    s->hid_intr.psm       = L2CAP_PSM_HID_INTERRUPT;
    s->hid_intr.local_cid = L2CAP_LOCAL_CID_HID_INTR;
}

int l2cap_hid_connect(l2cap_hid_session_t *s) {
    if (!s) return -1;
    return send_sig_conn_req(s, &s->hid_ctrl);
}

static void handle_signaling(l2cap_hid_session_t *s, const uint8_t *sig, size_t sig_len) {
    size_t off = 0;
    while (off + 4u <= sig_len) {
        uint8_t  code    = sig[off];
        uint8_t  id      = sig[off + 1u];
        uint16_t cmd_len = hci_get_le16(sig + off + 2u);
        if (off + 4u + cmd_len > sig_len) break;

        const uint8_t *p = sig + off + 4u;

        if (code == L2CAP_CONN_REQ && cmd_len >= 4u) {
            uint16_t psm  = hci_get_le16(p);
            uint16_t scid = hci_get_le16(p + 2u);
            l2cap_channel_t *ch = (psm == L2CAP_PSM_HID_CONTROL)   ? &s->hid_ctrl :
                                  (psm == L2CAP_PSM_HID_INTERRUPT) ? &s->hid_intr : NULL;
            if (ch) {
                ch->remote_cid = scid;
                ch->state = L2CAP_CH_WAIT_CONF;
                send_sig_conn_rsp(s, id, ch->local_cid, scid, 0x0000u, 0x0000u);
                send_sig_conf_req(s, ch);
            } else {
                send_sig_conn_rsp(s, id, 0x0000u, scid, 0x0002u, 0x0000u); /* PSM not supported */
            }
        } else if (code == L2CAP_CONN_RSP && cmd_len >= 8u) {
            uint16_t dcid   = hci_get_le16(p);
            uint16_t scid   = hci_get_le16(p + 2u);
            uint16_t result = hci_get_le16(p + 4u);
            l2cap_channel_t *ch = find_by_local_cid(s, scid);
            if (ch && result == 0x0000u) {
                ch->remote_cid = dcid;
                ch->state = L2CAP_CH_WAIT_CONF;
                send_sig_conf_req(s, ch);
                if (ch == &s->hid_ctrl && s->hid_intr.state == L2CAP_CH_CLOSED) {
                    send_sig_conn_req(s, &s->hid_intr);
                }
            }
        } else if (code == L2CAP_CONF_REQ && cmd_len >= 4u) {
            uint16_t dcid = hci_get_le16(p);
            l2cap_channel_t *ch = find_by_local_cid(s, dcid);
            if (ch) {
                send_sig_conf_rsp(s, id, ch->remote_cid, 0x0000u);
                ch->remote_conf_done = true;
                maybe_mark_open(ch);
            }
        } else if (code == L2CAP_CONF_RSP && cmd_len >= 6u) {
            uint16_t scid   = hci_get_le16(p);
            uint16_t result = hci_get_le16(p + 4u);
            l2cap_channel_t *ch = find_by_local_cid(s, scid);
            if (ch && result == 0x0000u) {
                ch->local_conf_done = true;
                maybe_mark_open(ch);
            }
        } else if (code == L2CAP_DISCONN_REQ && cmd_len >= 4u) {
            uint16_t dcid = hci_get_le16(p);
            uint16_t scid = hci_get_le16(p + 2u);
            l2cap_channel_t *ch = find_by_local_cid(s, dcid);
            if (ch) {
                ch->state = L2CAP_CH_CLOSED;
                ch->local_conf_done = false;
                ch->remote_conf_done = false;
            }
            uint8_t rsp[8] = { L2CAP_DISCONN_RSP, id, 4, 0, 0, 0, 0, 0 };
            hci_put_le16(rsp + 4, dcid);
            hci_put_le16(rsp + 6, scid);
            send_l2cap_pdu(s, L2CAP_CID_SIGNALING, rsp, sizeof(rsp));
        }

        off += 4u + cmd_len;
    }
}

void l2cap_hid_on_acl_payload(l2cap_hid_session_t *s,
                              const uint8_t *pdu, size_t pdu_len) {
    if (!s || !pdu || pdu_len < 4u) return;

    uint16_t len = hci_get_le16(pdu);
    uint16_t cid = hci_get_le16(pdu + 2u);
    if (4u + (size_t)len > pdu_len) return;

    const uint8_t *payload = pdu + 4u;
    if (cid == L2CAP_CID_SIGNALING) {
        handle_signaling(s, payload, len);
    } else if (s->on_hid_data) {
        l2cap_channel_t *ch = find_by_local_cid(s, cid);
        if (ch) {
            s->on_hid_data(s->cb_ctx, ch->psm, payload, len);
        }
    }
}

int l2cap_hid_send(l2cap_hid_session_t *s, uint16_t psm,
                   const uint8_t *data, uint16_t len) {
    if (!s) return -1;
    l2cap_channel_t *ch = (psm == L2CAP_PSM_HID_CONTROL)   ? &s->hid_ctrl :
                          (psm == L2CAP_PSM_HID_INTERRUPT) ? &s->hid_intr : NULL;
    if (!ch || ch->state != L2CAP_CH_OPEN) {
        return -2;
    }
    return send_l2cap_pdu(s, ch->remote_cid, data, len);
}
