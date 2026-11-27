/*
 * Tarefa 06: infraestrutura do transporte para a sessao de pareamento:
 * fila de comandos com um comando em voo, fila de eventos para a thread do plugin,
 * reabertura do Interrupt IN apos o init e fluxo de inquiry sobre a fila.
 */
#include "../transport/bt_usb_transport.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint16_t opcode[64];
    uint8_t  params0[64];
    uint8_t  params5[64];
    int      n;
    int      fail_next;
    int      intr_submits;
    int      release_calls;
    uint8_t *rx_buf;
    uint16_t rx_cap;
} mock_hal_t;

static int hal_control(void *ctx, uint8_t type, uint8_t req, uint16_t value, uint16_t index,
                       const uint8_t *data, uint16_t length) {
    mock_hal_t *m = ctx;
    (void)type; (void)req; (void)value; (void)index;
    if (m->fail_next) { m->fail_next = 0; return -7; }
    assert(m->n < 64 && length >= HCI_CMD_HDR_SIZE);
    m->opcode[m->n] = hci_get_le16(data);
    m->params0[m->n] = length > HCI_CMD_HDR_SIZE ? data[HCI_CMD_HDR_SIZE] : 0;
    m->params5[m->n] = length > HCI_CMD_HDR_SIZE + 5u ? data[HCI_CMD_HDR_SIZE + 5u] : 0;
    m->n++;
    return 0;
}
static int hal_intr(void *ctx, uint8_t *buf, uint16_t max_len) {
    mock_hal_t *m = ctx;
    m->rx_buf = buf; m->rx_cap = max_len; m->intr_submits++;
    return 0;
}
static void hal_release_control(void *ctx) {
    mock_hal_t *m = ctx;
    m->release_calls++;
}

static void setup(bt_usb_transport_t *tr, mock_hal_t *m) {
    memset(m, 0, sizeof(*m));
    bt_usb_device_info_t dev;
    memset(&dev, 0, sizeof(dev));
    dev.vid = 0x2B89; dev.pid = 0x8761; dev.has_intr_in = true;
    dev.intr_in.bEndpointAddress = 0x81;
    dev.intr_in.bmAttributes = BT_USB_EP_TYPE_INTERRUPT;
    dev.intr_in.wMaxPacketSize = 16;
    bt_usb_hal_ops_t hal = {
        .ctx = m, .submit_control_out = hal_control,
        .submit_intr_in = hal_intr, .release_control = hal_release_control
    };
    assert(bt_usb_transport_init(tr, &dev, &hal) == 0);
}

static size_t cc_evt(uint8_t *b, uint16_t opcode, const uint8_t *ret, size_t ret_len) {
    b[0] = HCI_EVT_COMMAND_COMPLETE; b[1] = (uint8_t)(3 + ret_len); b[2] = 1;
    hci_put_le16(b + 3, opcode);
    memcpy(b + 5, ret, ret_len);
    return 5 + ret_len;
}
static size_t cs_evt(uint8_t *b, uint16_t opcode, uint8_t status) {
    b[0] = HCI_EVT_COMMAND_STATUS; b[1] = 4; b[2] = status; b[3] = 1;
    hci_put_le16(b + 4, opcode);
    return 6;
}
static void deliver(bt_usb_transport_t *tr, const uint8_t *evt, size_t len) {
    bt_usb_transport_on_intr_in(tr, evt, len);
}

/* Leva o transporte ate READY com respostas sinteticas. */
static void init_ready(bt_usb_transport_t *tr, mock_hal_t *m) {
    static const uint8_t ok[] = { 0 };
    static const uint8_t ver[] = { 0, 9, 0x34, 0x12, 9, 0x5d, 0, 0x78, 0x56 };
    static const uint8_t bd[] = { 0, 1, 2, 3, 4, 5, 6 };
    uint8_t e[64];
    assert(bt_usb_transport_start_init(tr) == 0);
    assert(m->n == 1 && m->opcode[0] == HCI_OP_RESET);
    deliver(tr, e, cc_evt(e, HCI_OP_RESET, ok, 1));
    assert(m->n == 2 && m->opcode[1] == HCI_OP_READ_LOCAL_VERSION_INFO);
    deliver(tr, e, cc_evt(e, HCI_OP_READ_LOCAL_VERSION_INFO, ver, sizeof ver));
    assert(m->n == 3 && m->opcode[2] == HCI_OP_READ_BD_ADDR);
    deliver(tr, e, cc_evt(e, HCI_OP_READ_BD_ADDR, bd, sizeof bd));
    assert(tr->state == BT_INIT_READY);
    m->n = 0;
}

static void test_command_queue_serializes(void) {
    bt_usb_transport_t tr; mock_hal_t m;
    setup(&tr, &m);
    static const uint8_t p1 = 1, p2 = 2, p3 = 3;
    assert(bt_usb_transport_send_command(&tr, HCI_OP_WRITE_SIMPLE_PAIRING, &p1, 1) == 0);
    assert(bt_usb_transport_send_command(&tr, HCI_OP_WRITE_SCAN_ENABLE, &p2, 1) == 0);
    assert(bt_usb_transport_send_command(&tr, HCI_OP_WRITE_PAGE_TIMEOUT, &p3, 1) == 0);
    assert(m.n == 1 && m.opcode[0] == HCI_OP_WRITE_SIMPLE_PAIRING && m.params0[0] == 1);

    uint8_t e[16]; static const uint8_t ok[] = { 0 };

    /* Resposta de outro opcode e NOP (0x0000) nao liberam o canal. */
    deliver(&tr, e, cc_evt(e, HCI_OP_RESET, ok, 1));
    deliver(&tr, e, cc_evt(e, 0x0000u, ok, 1));
    assert(m.n == 1);

    deliver(&tr, e, cc_evt(e, HCI_OP_WRITE_SIMPLE_PAIRING, ok, 1));
    assert(m.n == 2 && m.opcode[1] == HCI_OP_WRITE_SCAN_ENABLE && m.params0[1] == 2);

    /* Command Status tambem libera o canal (ex.: Create_Connection). */
    deliver(&tr, e, cs_evt(e, HCI_OP_WRITE_SCAN_ENABLE, 0));
    assert(m.n == 3 && m.opcode[2] == HCI_OP_WRITE_PAGE_TIMEOUT && m.params0[2] == 3);
    deliver(&tr, e, cc_evt(e, HCI_OP_WRITE_PAGE_TIMEOUT, ok, 1));
    assert(m.n == 3);

    /* Com o canal livre o proximo comando sai na hora. */
    assert(bt_usb_transport_send_command(&tr, HCI_OP_INQUIRY_CANCEL, NULL, 0) == 0);
    assert(m.n == 4 && m.opcode[3] == HCI_OP_INQUIRY_CANCEL);
    printf("[PASS] fila de comandos HCI: um em voo, ordem preservada\n");
}

static void test_queue_limits_and_hal_failure(void) {
    bt_usb_transport_t tr; mock_hal_t m;
    setup(&tr, &m);
    assert(bt_usb_transport_send_command(NULL, HCI_OP_RESET, NULL, 0) < 0);
    assert(bt_usb_transport_send_command(&tr, HCI_OP_RESET, NULL, 1) < 0); /* params ausentes */

    /* 1 em voo + BT_USB_CMD_QUEUE_DEPTH na fila; o seguinte e recusado. */
    int accepted = 0;
    for (int i = 0; i < 40; i++) {
        if (bt_usb_transport_send_command(&tr, HCI_OP_INQUIRY_CANCEL, NULL, 0) == 0) accepted++;
    }
    assert(accepted == (int)BT_USB_CMD_QUEUE_DEPTH + 1);
    assert(m.n == 1);

    /* Falha do HAL descarta o comando e nao trava o canal. */
    setup(&tr, &m);
    m.fail_next = 1;
    assert(bt_usb_transport_send_command(&tr, HCI_OP_RESET, NULL, 0) == -7);
    assert(bt_usb_transport_send_command(&tr, HCI_OP_INQUIRY_CANCEL, NULL, 0) == 0);
    assert(m.n == 1 && m.opcode[0] == HCI_OP_INQUIRY_CANCEL);
    printf("[PASS] fila de comandos: limite e falha do HAL sem travar o canal\n");
}

static void test_pump_timeout_releases_channel(void) {
    bt_usb_transport_t tr; mock_hal_t m;
    setup(&tr, &m);
    assert(bt_usb_transport_send_command(&tr, HCI_OP_INQUIRY_CANCEL, NULL, 0) == 0);
    assert(bt_usb_transport_send_command(&tr, HCI_OP_RESET, NULL, 0) == 0);
    assert(m.n == 1);
    bt_usb_transport_pump(&tr, BT_USB_CMD_TIMEOUT_MS - 1u);
    assert(m.n == 1);
    bt_usb_transport_pump(&tr, 1u);
    assert(m.n == 2 && m.opcode[1] == HCI_OP_RESET);
    assert(tr.cmd_timeouts == 1u);
    printf("[PASS] comando sem resposta expira e libera a fila\n");
}

static void test_event_fifo(void) {
    bt_usb_transport_t tr; mock_hal_t m;
    setup(&tr, &m);
    uint8_t out[HCI_MAX_EVT_SIZE];
    uint8_t e[64];
    static const uint8_t ok[] = { 0 };

    deliver(&tr, e, cc_evt(e, HCI_OP_RESET, ok, 1)); /* desligada: nada na fila */
    assert(bt_usb_transport_pop_event(&tr, out, sizeof out) == 0);

    bt_usb_transport_enable_events(&tr, true);
    uint8_t conn[] = { HCI_EVT_CONNECTION_COMPLETE, 11, 0, 0x21, 0x00, 1, 2, 3, 4, 5, 6, 1, 0 };
    deliver(&tr, conn, sizeof conn);
    /* Evento fragmentado em dois quadros USB. */
    uint8_t lk[] = { HCI_EVT_LINK_KEY_NOTIFICATION, 23, 1, 2, 3, 4, 5, 6,
                     1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 5 };
    deliver(&tr, lk, 10);
    assert(bt_usb_transport_pop_event(&tr, out, sizeof out) == 13); /* so o primeiro completo */
    assert(memcmp(out, conn, sizeof conn) == 0);
    assert(bt_usb_transport_pop_event(&tr, out, sizeof out) == 0);
    deliver(&tr, lk + 10, sizeof lk - 10);
    assert(bt_usb_transport_pop_event(&tr, out, 8) == -1); /* buffer pequeno: evento fica */
    assert(bt_usb_transport_pop_event(&tr, out, sizeof out) == (int)sizeof lk);
    assert(memcmp(out, lk, sizeof lk) == 0);
    assert(bt_usb_transport_pop_event(&tr, out, sizeof out) == 0);

    /* Fila cheia descarta e conta; os mais antigos sao preservados. */
    for (unsigned i = 0; i < BT_USB_EVT_FIFO_DEPTH + 3u; i++) {
        uint8_t ev[] = { HCI_EVT_DISCONNECTION_COMPLETE, 4, 0, (uint8_t)i, 0, 0x13 };
        deliver(&tr, ev, sizeof ev);
    }
    assert(bt_usb_transport_events_dropped(&tr) == 3u);
    for (unsigned i = 0; i < BT_USB_EVT_FIFO_DEPTH; i++) {
        assert(bt_usb_transport_pop_event(&tr, out, sizeof out) == 6);
        assert(out[0] == HCI_EVT_DISCONNECTION_COMPLETE && out[3] == (uint8_t)i);
    }
    assert(bt_usb_transport_pop_event(&tr, out, sizeof out) == 0);

    bt_usb_transport_enable_events(&tr, false);
    deliver(&tr, conn, sizeof conn);
    assert(bt_usb_transport_pop_event(&tr, out, sizeof out) == 0);
    printf("[PASS] fila de eventos HCI para a thread do plugin\n");
}

static void test_intr_in_stays_armed_after_ready(void) {
    bt_usb_transport_t tr; mock_hal_t m;
    setup(&tr, &m);
    init_ready(&tr, &m);
    int before = m.intr_submits;
    uint8_t e[16];
    uint8_t conn_evt[] = { HCI_EVT_DISCONNECTION_COMPLETE, 4, 0, 1, 0, 0x13 };
    deliver(&tr, conn_evt, sizeof conn_evt);
    assert(m.intr_submits == before + 1); /* continua ouvindo apos o init */
    deliver(&tr, e, cs_evt(e, HCI_OP_INQUIRY, 0));
    assert(m.intr_submits == before + 2);
    printf("[PASS] Interrupt IN continua armado depois do init\n");
}

static void test_inquiry_over_queue(void) {
    bt_usb_transport_t tr; mock_hal_t m;
    setup(&tr, &m);
    init_ready(&tr, &m);
    static const uint8_t ok[] = { 0 };
    uint8_t e[16];
    int arm0 = m.intr_submits;

    assert(bt_usb_transport_send_command(&tr, HCI_OP_WRITE_SCAN_ENABLE, ok, 1) == 0);
    assert(bt_usb_transport_start_inquiry(&tr, 48) == 0);
    assert(m.intr_submits == arm0); /* ja armado: nao rearma sobre transferencia pendente */
    assert(m.n == 1); /* scan enable em voo; inquiry espera na fila */
    deliver(&tr, e, cc_evt(e, HCI_OP_WRITE_SCAN_ENABLE, ok, 1));
    assert(m.n == 2 && m.opcode[1] == HCI_OP_SET_EVENT_MASK);
    deliver(&tr, e, cc_evt(e, HCI_OP_SET_EVENT_MASK, ok, 1));
    assert(m.n == 3 && m.opcode[2] == HCI_OP_WRITE_INQUIRY_MODE && m.params0[2] == 2);
    deliver(&tr, e, cc_evt(e, HCI_OP_WRITE_INQUIRY_MODE, ok, 1));
    assert(m.n == 4 && m.opcode[3] == HCI_OP_INQUIRY);
    assert(tr.scan_active);

    deliver(&tr, e, cs_evt(e, HCI_OP_INQUIRY, 0));
    assert(bt_usb_transport_stop_inquiry(&tr) == 0);
    assert(m.n == 5 && m.opcode[4] == HCI_OP_INQUIRY_CANCEL);
    assert(!tr.scan_active);
    printf("[PASS] inquiry reaproveitado sobre a fila de comandos\n");
}

static void test_remote_name_fills_scan_item(void) {
    bt_usb_transport_t tr; mock_hal_t m;
    setup(&tr, &m);
    init_ready(&tr, &m);
    uint8_t found[17] = {
        HCI_EVT_INQUIRY_RESULT_RSSI, 15, 1,
        0x67, 0x2F, 0x82, 0x92, 0x29, 0x20,
        1, 0, 0x08, 0x25, 0x00, 0x00, 0x00, 0xC0
    };
    deliver(&tr, found, sizeof found);
    assert(tr.scan_count == 1);
    assert(tr.scan_results[0].name[0] == '\0');
    assert(m.n == 1 && m.opcode[0] == HCI_OP_REMOTE_NAME_REQUEST);

    uint8_t e[16];
    deliver(&tr, e, cs_evt(e, HCI_OP_REMOTE_NAME_REQUEST, 0));
    const char *name = "Wireless Controller";
    uint8_t rsp[2 + 1 + 6 + 32];
    rsp[0] = HCI_EVT_REMOTE_NAME_REQ_COMPLETE;
    rsp[1] = (uint8_t)(1 + 6 + 20);
    rsp[2] = 0;
    memcpy(rsp + 3, found + 3, 6);
    memcpy(rsp + 9, name, 20);
    deliver(&tr, rsp, 2u + rsp[1]);
    assert(strcmp(tr.scan_results[0].name, "Wireless Controller") == 0);
    assert(m.n == 1);
    printf("[PASS] nome real do aparelho entra na lista da busca\n");
}

static void test_active_transport(void) {
    bt_usb_transport_t tr; mock_hal_t m;
    setup(&tr, &m);
    assert(bt_usb_transport_active() == &tr);
    printf("[PASS] transporte ativo acessivel ao plugin\n");
}

static void test_init_reset_retry_and_exhaust(void) {
    bt_usb_transport_t tr; mock_hal_t m;
    uint8_t e[64];
    static const uint8_t ok[] = { 0 };
    static const uint8_t ver[] = { 0, 6, 0xBB, 0x22, 6, 0x0A, 0, 0xBB, 0x22 };
    static const uint8_t bd[] = { 0, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };

    /* 1st reset times out, 2nd succeeds and finishes init. */
    setup(&tr, &m);
    assert(bt_usb_transport_start_init(&tr) == 0);
    assert(m.n == 1 && m.opcode[0] == HCI_OP_RESET);
    bt_usb_transport_pump(&tr, BT_USB_CMD_TIMEOUT_MS);
    assert(m.release_calls == 1);
    assert(m.n == 2 && m.opcode[1] == HCI_OP_RESET);
    assert(tr.state == BT_INIT_WAIT_RESET);
    deliver(&tr, e, cc_evt(e, HCI_OP_RESET, ok, 1));
    assert(tr.state == BT_INIT_WAIT_LOCAL_VER);
    deliver(&tr, e, cc_evt(e, HCI_OP_READ_LOCAL_VERSION_INFO, ver, sizeof ver));
    deliver(&tr, e, cc_evt(e, HCI_OP_READ_BD_ADDR, bd, sizeof bd));
    assert(tr.state == BT_INIT_READY);

    /* Exhausting all retries transitions to BT_INIT_ERROR. */
    setup(&tr, &m);
    assert(bt_usb_transport_start_init(&tr) == 0);
    for (unsigned i = 0; i < BT_USB_INIT_MAX_RETRIES; i++) {
        bt_usb_transport_pump(&tr, BT_USB_CMD_TIMEOUT_MS);
    }
    assert(tr.state == BT_INIT_ERROR && tr.last_error == -12);
    assert(m.n == (int)BT_USB_INIT_MAX_RETRIES);
    printf("[PASS] retry de HCI_Reset no init e erro limpo ao esgotar tentativas\n");
}

static void test_inquiry_fallbacks_for_cheap_dongles(void) {
    bt_usb_transport_t tr; mock_hal_t m;
    setup(&tr, &m);
    init_ready(&tr, &m);
    uint8_t e[16];
    static const uint8_t err[] = { 0x12 }; /* Invalid HCI Command Parameters */
    static const uint8_t ok[] = { 0x00 };

    assert(bt_usb_transport_start_inquiry(&tr, 4) == 0);
    assert(m.n == 1 && m.opcode[0] == HCI_OP_SET_EVENT_MASK && m.params5[0] == 0xF8u);
    /* Clone rejects full Classic+SSP mask -> falls back to legacy mask (byte 5 == 0x00). */
    deliver(&tr, e, cc_evt(e, HCI_OP_SET_EVENT_MASK, err, 1));
    assert(m.n == 2 && m.opcode[1] == HCI_OP_SET_EVENT_MASK && m.params5[1] == 0x00u);
    deliver(&tr, e, cc_evt(e, HCI_OP_SET_EVENT_MASK, ok, 1));
    assert(m.n == 3 && m.opcode[2] == HCI_OP_WRITE_INQUIRY_MODE && m.params0[2] == 2u);
    /* Clone rejects EIR mode (2) -> falls back to RSSI mode (1), then sends Inquiry. */
    deliver(&tr, e, cc_evt(e, HCI_OP_WRITE_INQUIRY_MODE, err, 1));
    assert(m.n == 4 && m.opcode[3] == HCI_OP_WRITE_INQUIRY_MODE && m.params0[3] == 1u);
    deliver(&tr, e, cc_evt(e, HCI_OP_WRITE_INQUIRY_MODE, ok, 1));
    assert(m.n == 5 && m.opcode[4] == HCI_OP_INQUIRY);
    printf("[PASS] fallbacks de Set_Event_Mask e Write_Inquiry_Mode para clones baratos\n");
}

int main(void) {
    test_command_queue_serializes();
    test_queue_limits_and_hal_failure();
    test_pump_timeout_releases_channel();
    test_event_fifo();
    test_intr_in_stays_armed_after_ready();
    test_inquiry_over_queue();
    test_remote_name_fills_scan_item();
    test_active_transport();
    test_init_reset_retry_and_exhaust();
    test_inquiry_fallbacks_for_cheap_dongles();
    return 0;
}
