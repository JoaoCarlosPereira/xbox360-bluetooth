#ifndef XBOX360_BT_L2CAP_H
#define XBOX360_BT_L2CAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define L2CAP_CID_SIGNALING     0x0001u
#define L2CAP_PSM_HID_CONTROL   0x0011u
#define L2CAP_PSM_HID_INTERRUPT 0x0013u

/* L2CAP Signaling Codes */
#define L2CAP_CMD_REJECT        0x01u
#define L2CAP_CONN_REQ          0x02u
#define L2CAP_CONN_RSP          0x03u
#define L2CAP_CONF_REQ          0x04u
#define L2CAP_CONF_RSP          0x05u
#define L2CAP_DISCONN_REQ       0x06u
#define L2CAP_DISCONN_RSP       0x07u

/* Local CIDs assigned by our stack */
#define L2CAP_LOCAL_CID_HID_CTRL 0x0040u
#define L2CAP_LOCAL_CID_HID_INTR 0x0041u

typedef enum {
    L2CAP_CH_CLOSED = 0,
    L2CAP_CH_WAIT_CONN_RSP,
    L2CAP_CH_WAIT_CONF,
    L2CAP_CH_OPEN
} l2cap_ch_state_t;

typedef struct {
    uint16_t         psm;
    uint16_t         local_cid;
    uint16_t         remote_cid;
    l2cap_ch_state_t state;
    bool             local_conf_done;
    bool             remote_conf_done;
} l2cap_channel_t;

typedef int (*l2cap_send_acl_fn)(void *ctx, uint16_t acl_handle,
                                 const uint8_t *acl_payload, uint16_t len);
typedef void (*l2cap_on_hid_data_fn)(void *ctx, uint16_t psm,
                                     const uint8_t *data, size_t len);

typedef struct {
    uint16_t             acl_handle;
    uint8_t              next_sig_id;
    l2cap_channel_t      hid_ctrl;
    l2cap_channel_t      hid_intr;
    l2cap_send_acl_fn    send_acl;
    l2cap_on_hid_data_fn on_hid_data;
    void                *cb_ctx;
} l2cap_hid_session_t;

void l2cap_hid_init(l2cap_hid_session_t *s, uint16_t acl_handle,
                    l2cap_send_acl_fn send_acl, l2cap_on_hid_data_fn on_hid_data,
                    void *cb_ctx);

/**
 * Initiate L2CAP HID Control (0x0011) and HID Interrupt (0x0013) connections.
 */
int l2cap_hid_connect(l2cap_hid_session_t *s);

/**
 * Feed a complete L2CAP PDU (after stripping the 4-byte HCI ACL header).
 */
void l2cap_hid_on_acl_payload(l2cap_hid_session_t *s,
                              const uint8_t *pdu, size_t pdu_len);

/**
 * Send raw HIDP payload on HID Control (PSM 0x0011) or HID Interrupt (PSM 0x0013).
 */
int l2cap_hid_send(l2cap_hid_session_t *s, uint16_t psm,
                   const uint8_t *data, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* XBOX360_BT_L2CAP_H */
