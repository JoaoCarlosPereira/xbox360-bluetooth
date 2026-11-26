#include "../chipset/rtl8761b.h"
#include "../hci/hci_core.h"
#include "../hid/ds4_bt.h"
#include "../l2cap/l2cap.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    l2cap_hid_session_t *peer_remote_sim;
    uint16_t             last_handle;
    uint8_t              last_pdu[512];
    uint16_t             last_len;
    int                  acl_tx_count;
    x360_gamepad_state_t last_gp;
    int                  gp_count;
} test_l2cap_env_t;

static int mock_send_acl(void *ctx, uint16_t acl_handle,
                         const uint8_t *acl_payload, uint16_t len) {
    test_l2cap_env_t *env = (test_l2cap_env_t *)ctx;
    env->last_handle = acl_handle;
    env->last_len = len;
    memcpy(env->last_pdu, acl_payload, len);
    env->acl_tx_count++;
    return 0;
}

static void on_hid_data(void *ctx, uint16_t psm,
                        const uint8_t *data, size_t len) {
    test_l2cap_env_t *env = (test_l2cap_env_t *)ctx;
    if (psm == L2CAP_PSM_HID_INTERRUPT) {
        if (ds4_bt_parse_hidp_report(data, len, &env->last_gp) == 0) {
            env->gp_count++;
        }
    }
}

static void feed_sig(l2cap_hid_session_t *s, const uint8_t *sig, uint16_t sig_len) {
    uint8_t pdu[128];
    hci_put_le16(pdu + 0, sig_len);
    hci_put_le16(pdu + 2, L2CAP_CID_SIGNALING);
    memcpy(pdu + 4, sig, sig_len);
    l2cap_hid_on_acl_payload(s, pdu, (size_t)(4u + sig_len));
}

static void test_l2cap_and_ds4(void) {
    test_l2cap_env_t env;
    memset(&env, 0, sizeof(env));

    l2cap_hid_session_t sess;
    l2cap_hid_init(&sess, 0x000Bu, mock_send_acl, on_hid_data, &env);
    assert(l2cap_hid_connect(&sess) == 0);
    assert(sess.hid_ctrl.state == L2CAP_CH_WAIT_CONN_RSP);

    /* Simulate DS4 accepting HID Control (remote_cid = 0x0050), which auto-requests HID Intr */
    uint8_t conn_rsp_ctrl[12] = { L2CAP_CONN_RSP, 1, 8, 0,  0x50, 0x00,  0x40, 0x00,  0, 0,  0, 0 };
    feed_sig(&sess, conn_rsp_ctrl, sizeof(conn_rsp_ctrl));
    assert(sess.hid_ctrl.state == L2CAP_CH_WAIT_CONF);
    assert(sess.hid_intr.state == L2CAP_CH_WAIT_CONN_RSP);

    /* Simulate DS4 accepting HID Interrupt (remote_cid = 0x0051) */
    uint8_t conn_rsp_intr[12] = { L2CAP_CONN_RSP, 2, 8, 0,  0x51, 0x00,  0x41, 0x00,  0, 0,  0, 0 };
    feed_sig(&sess, conn_rsp_intr, sizeof(conn_rsp_intr));
    assert(sess.hid_intr.state == L2CAP_CH_WAIT_CONF);

    /* Simulate DS4 CONF_REQ + CONF_RSP for both channels */
    uint8_t conf_req_ctrl[8] = { L2CAP_CONF_REQ, 10, 4, 0,  0x40, 0x00,  0, 0 };
    uint8_t conf_rsp_ctrl[10] = { L2CAP_CONF_RSP, 2, 6, 0,  0x40, 0x00,  0, 0,  0, 0 };
    feed_sig(&sess, conf_req_ctrl, sizeof(conf_req_ctrl));
    feed_sig(&sess, conf_rsp_ctrl, sizeof(conf_rsp_ctrl));
    assert(sess.hid_ctrl.state == L2CAP_CH_OPEN);

    uint8_t conf_req_intr[8] = { L2CAP_CONF_REQ, 11, 4, 0,  0x41, 0x00,  0, 0 };
    uint8_t conf_rsp_intr[10] = { L2CAP_CONF_RSP, 4, 6, 0,  0x41, 0x00,  0, 0,  0, 0 };
    feed_sig(&sess, conf_req_intr, sizeof(conf_req_intr));
    feed_sig(&sess, conf_rsp_intr, sizeof(conf_rsp_intr));
    assert(sess.hid_intr.state == L2CAP_CH_OPEN);

    /* Simulate DS4 Bluetooth Extended Input Report 0x11 arriving on HID Interrupt */
    uint8_t pdu[4 + 16];
    memset(pdu, 0, sizeof(pdu));
    hci_put_le16(pdu + 0, 16u);
    hci_put_le16(pdu + 2, L2CAP_LOCAL_CID_HID_INTR);
    pdu[4]  = 0xA1; /* HIDP DATA | INPUT */
    pdu[5]  = 0x11; /* DS4 BT Report ID 0x11 */
    pdu[6]  = 0xC0;
    pdu[7]  = 0x00;
    pdu[8]  = 0x80; /* LX center */
    pdu[9]  = 0x00; /* LY full up (0x00 -> +32767 after Y-invert) */
    pdu[10] = 0xFF; /* RX full right */
    pdu[11] = 0x80; /* RY center */
    pdu[12] = 0x20 | 0x00; /* Cross (A) + D-pad Up (0) */
    pdu[13] = 0x01 | 0x20; /* L1 + Options (Start) */
    pdu[14] = 0x01;        /* PS Button (Guide) */
    pdu[15] = 0x7F;        /* L2 = 127 */
    pdu[16] = 0xFF;        /* R2 = 255 */

    l2cap_hid_on_acl_payload(&sess, pdu, sizeof(pdu));
    assert(env.gp_count == 1);
    assert((env.last_gp.buttons & X360_BTN_A) != 0);
    assert((env.last_gp.buttons & X360_BTN_DPAD_UP) != 0);
    assert((env.last_gp.buttons & X360_BTN_LEFT_SHOULDER) != 0);
    assert((env.last_gp.buttons & X360_BTN_START) != 0);
    assert((env.last_gp.buttons & X360_BTN_GUIDE) != 0);
    assert(env.last_gp.left_trigger == 0x7F);
    assert(env.last_gp.right_trigger == 0xFF);
    assert(env.last_gp.thumb_lx == 0);
    assert(env.last_gp.thumb_ly == 32767);
    assert(env.last_gp.thumb_rx > 32000);

    printf("[PASS] L2CAP HID Control/Interrupt handshake + DS4 BT 0x11 report parser\n");
}

/* Offsets de referencia: ds4_live.c (HIDP SET_REPORT 0x52, ID 0x11, flags, motores) */
#define RUM_OFF_HIDP   0
#define RUM_OFF_ID     1
#define RUM_OFF_FLAGS  4
#define RUM_OFF_SMALL  7
#define RUM_OFF_LARGE  8

static void test_ds4_rumble_report(void) {
    uint8_t buf[DS4_BT_OUTPUT_REPORT_LEN];

    /* [RUM-01] motor grande 200 e motor pequeno 10 */
    memset(buf, 0xEE, sizeof(buf));
    size_t n = ds4_bt_build_rumble_report(buf, sizeof(buf), 10u, 200u);
    assert(n == DS4_BT_OUTPUT_REPORT_LEN);
    assert(buf[RUM_OFF_HIDP] == 0x52u);
    assert(buf[RUM_OFF_ID] == 0x11u);
    assert(buf[RUM_OFF_FLAGS] == 0x07u);
    assert(buf[RUM_OFF_SMALL] == 10u);
    assert(buf[RUM_OFF_LARGE] == 200u);
    printf("[PASS] RUM-01 motor grande 200 e pequeno 10 geram 0x11 com os dois valores\n");

    /* [RUM-01] os dois motores em 0 */
    memset(buf, 0xEE, sizeof(buf));
    n = ds4_bt_build_rumble_report(buf, sizeof(buf), 0u, 0u);
    assert(n == DS4_BT_OUTPUT_REPORT_LEN);
    assert(buf[RUM_OFF_ID] == 0x11u);
    assert(buf[RUM_OFF_SMALL] == 0u);
    assert(buf[RUM_OFF_LARGE] == 0u);
    assert(buf[75] == 0x3eu && buf[76] == 0x39u && buf[77] == 0x2au && buf[78] == 0x51u);
    printf("[PASS] RUM-01 os dois motores em 0 geram 0x11 com os dois em 0\n");

    /* [RUM-01] buffer menor que o quadro nao e escrito */
    uint8_t sentinel[DS4_BT_OUTPUT_REPORT_LEN];
    memset(sentinel, 0xEE, sizeof(sentinel));
    memset(buf, 0xEE, sizeof(buf));
    n = ds4_bt_build_rumble_report(buf, DS4_BT_OUTPUT_REPORT_LEN - 1u, 10u, 200u);
    assert(n == 0u);
    assert(memcmp(buf, sentinel, sizeof(buf)) == 0);
    assert(ds4_bt_build_rumble_report(NULL, sizeof(buf), 10u, 200u) == 0u);
    printf("[PASS] RUM-01 buffer menor que o quadro nao e escrito\n");
}

static int dummy_rtl_cmd(void *ctx, uint16_t opcode, const uint8_t *params, uint8_t param_len) {
    int *count = (int *)ctx;
    assert(opcode == RTL_OP_DOWNLOAD_CMD);
    assert(param_len >= 2 && param_len <= 1 + RTL_FRAG_MAX_DATA);
    (void)params;
    (*count)++;
    return 0;
}

static int check_synthetic_fw(void *ctx, uint16_t opcode, const uint8_t *params, uint8_t len) {
    static const uint8_t expected[] = { 0x80, 0xaa, 0xbb, 0xcc, 0xdd,
                                        0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc };
    int *count = (int *)ctx;
    assert(opcode == RTL_OP_DOWNLOAD_CMD);
    assert(len == sizeof(expected));
    assert(memcmp(params, expected, sizeof(expected)) == 0);
    (*count)++;
    return 0;
}

static void test_rtl_fw_synthetic(void) {
    uint8_t fw[] = { 'R','e','a','l','t','e','c','h', 0x12,0x34,0x56,0x78,
                     1,0, 2,0, 8,0, 22,0,0,0,
                     0xaa,0xbb,0xcc,0xdd, 0,0,0,0 };
    const uint8_t config[] = { 0x9a, 0xbc };
    rtl_fw_image_t img;
    int sent = 0;
    assert(rtl_fw_prepare(&img, fw, sizeof(fw), config, sizeof(config), 1) == 0);
    assert(rtl_fw_chunk_count(&img) == 1);
    assert(rtl_fw_send_chunk(&img, 0, check_synthetic_fw, &sent) == 0);
    assert(sent == 1);
    assert(rtl_fw_send_chunk(&img, 1, check_synthetic_fw, &sent) == -2);
    assert(rtl_fw_prepare(&img, fw, sizeof(fw), config, sizeof(config), 2) == -5);
    fw[18] = 0xff; /* Invalid patch offset */
    assert(rtl_fw_prepare(&img, fw, sizeof(fw), config, sizeof(config), 1) == -4);
    printf("[PASS] Synthetic EPATCH selection, version patch and bounds\n");
}

static void test_rtl_fw_parser(void) {
    const char *path = getenv("RTL_FIRMWARE");
    if (!path) path = "/lib/firmware/rtl_bt/rtl8761bu_fw.bin";
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (getenv("RTL_FIRMWARE")) {
            perror("RTL_FIRMWARE");
            exit(1);
        }
        printf("[SKIP] Realtek firmware not found; set RTL_FIRMWARE to an uncompressed image\n");
        return;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *fw = (uint8_t *)malloc((size_t)sz);
    assert(fw && fread(fw, 1, (size_t)sz, f) == (size_t)sz);
    fclose(f);

    uint8_t cfg[6] = { 0 };
    rtl_fw_image_t img;
    assert(rtl_fw_prepare(&img, fw, (size_t)sz, cfg, sizeof(cfg), 1) == 0);
    assert(img.total_len == (size_t)img.patch_len + sizeof(cfg));
    size_t chunks = rtl_fw_chunk_count(&img);
    assert(chunks > 0);

    int sent = 0;
    for (size_t i = 0; i < chunks; i++) {
        assert(rtl_fw_send_chunk(&img, i, dummy_rtl_cmd, &sent) == 0);
    }
    assert((size_t)sent == chunks);
    free(fw);
    printf("[PASS] Realtek RTL8761BU EPATCH parser (%zu bytes, %zu chunks)\n", img.total_len, chunks);
}

int main(void) {
    test_l2cap_and_ds4();
    test_ds4_rumble_report();
    test_rtl_fw_synthetic();
    test_rtl_fw_parser();
    return 0;
}
