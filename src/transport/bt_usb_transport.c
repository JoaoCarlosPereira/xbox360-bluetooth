#include "bt_usb_transport.h"
#include <string.h>

bool bt_usb_is_hci_interface(uint8_t bInterfaceClass,
                             uint8_t bInterfaceSubClass,
                             uint8_t bInterfaceProtocol) {
    return (bInterfaceClass == USB_CLASS_WIRELESS_CONTROLLER) &&
           (bInterfaceSubClass == USB_SUBCLASS_RF_CONTROLLER) &&
           (bInterfaceProtocol == USB_PROTOCOL_BLUETOOTH_HCI);
}

static bt_usb_transport_t *g_active_transport;

bt_usb_transport_t *bt_usb_transport_active(void) {
    return g_active_transport;
}

int bt_usb_transport_init(bt_usb_transport_t *tr,
                          const bt_usb_device_info_t *dev,
                          const bt_usb_hal_ops_t *hal) {
    if (!tr || !dev || !hal || !hal->submit_control_out || !hal->submit_intr_in) {
        return -1;
    }
    if (!dev->has_intr_in) {
        return -2;
    }

    memset(tr, 0, sizeof(*tr));
    tr->dev = *dev;
    tr->hal = *hal;
    tr->state = BT_INIT_IDLE;
    g_active_transport = tr;
    return 0;
}

/*
 * Canal de comandos HCI.
 *
 * Produtores (DPC do USB e thread do plugin) reservam um slot por CAS em cmd_tail e o
 * publicam com ready=1. Quem vence a troca em tx_busy e o unico que retira de cmd_head
 * e chama o HAL; o proximo comando so sai depois do Command Complete/Status do anterior.
 * cmd_inflight e marcado antes de chamar o HAL: a resposta pode chegar no DPC antes de
 * o HAL retornar.
 */
static bool cmd_queue_push(bt_usb_transport_t *tr, uint16_t opcode,
                           const uint8_t *params, uint8_t len) {
    uint32_t t;
    for (;;) {
        t = __atomic_load_n(&tr->cmd_tail, __ATOMIC_ACQUIRE);
        const uint32_t h = __atomic_load_n(&tr->cmd_head, __ATOMIC_ACQUIRE);
        if (t - h >= BT_USB_CMD_QUEUE_DEPTH) return false;
        if (__atomic_compare_exchange_n(&tr->cmd_tail, &t, t + 1u, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            break;
        }
    }
    bt_usb_cmd_slot_t *slot = &tr->cmd_slots[t % BT_USB_CMD_QUEUE_DEPTH];
    slot->opcode = opcode;
    slot->len = len;
    if (len) memcpy(slot->params, params, len);
    __atomic_store_n(&slot->ready, 1u, __ATOMIC_RELEASE);
    return true;
}

/* Somente o dono de tx_busy. */
static bool cmd_queue_pop(bt_usb_transport_t *tr, bt_usb_cmd_slot_t *out) {
    const uint32_t h = __atomic_load_n(&tr->cmd_head, __ATOMIC_ACQUIRE);
    if (h == __atomic_load_n(&tr->cmd_tail, __ATOMIC_ACQUIRE)) return false;
    bt_usb_cmd_slot_t *slot = &tr->cmd_slots[h % BT_USB_CMD_QUEUE_DEPTH];
    if (!__atomic_load_n(&slot->ready, __ATOMIC_ACQUIRE)) return false; /* produtor ainda copiando */
    out->opcode = slot->opcode;
    out->len = slot->len;
    if (slot->len) memcpy(out->params, slot->params, slot->len);
    __atomic_store_n(&slot->ready, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&tr->cmd_head, h + 1u, __ATOMIC_RELEASE);
    return true;
}

static bool cmd_queue_pending(bt_usb_transport_t *tr) {
    return __atomic_load_n(&tr->cmd_head, __ATOMIC_ACQUIRE) !=
           __atomic_load_n(&tr->cmd_tail, __ATOMIC_ACQUIRE);
}

static int submit_hci_cmd(bt_usb_transport_t *tr, uint16_t opcode,
                          const uint8_t *params, uint8_t param_len) {
    uint8_t cmd_buf[HCI_MAX_CMD_SIZE];
    size_t cmd_len = hci_build_command(cmd_buf, sizeof(cmd_buf), opcode, params, param_len);
    if (cmd_len == 0) {
        return -1;
    }
    __atomic_store_n(&tr->trace_opcode, opcode, __ATOMIC_RELAXED);
    return tr->hal.submit_control_out(tr->hal.ctx,
                                      BT_USB_REQ_TYPE_HCI_CMD,
                                      BT_USB_REQ_HCI_CMD,
                                      0x0000u,
                                      (uint16_t)tr->dev.interface_num,
                                      cmd_buf,
                                      (uint16_t)cmd_len);
}

/* Envia o que der. Retorna o erro do HAL de um comando enviado agora que falhou, senao 0. */
static int tx_flush(bt_usb_transport_t *tr) {
    int result = 0;
    if (__atomic_exchange_n(&tr->tx_busy, 1u, __ATOMIC_ACQUIRE)) return 0;
    for (;;) {
        bt_usb_cmd_slot_t cmd;
        if (!__atomic_load_n(&tr->cmd_inflight, __ATOMIC_ACQUIRE) && cmd_queue_pop(tr, &cmd)) {
            tr->inflight_opcode = cmd.opcode;
            tr->inflight_age_ms = 0;
            __atomic_store_n(&tr->cmd_inflight, 1u, __ATOMIC_RELEASE);
            const int rc = submit_hci_cmd(tr, cmd.opcode, cmd.params, cmd.len);
            if (rc != 0) {
                __atomic_store_n(&tr->cmd_inflight, 0u, __ATOMIC_RELEASE);
                result = rc;
            }
        }
        __atomic_store_n(&tr->tx_busy, 0u, __ATOMIC_RELEASE);
        /* Um produtor pode ter publicado, ou a resposta chegado, durante o envio. */
        if (__atomic_load_n(&tr->cmd_inflight, __ATOMIC_ACQUIRE) || !cmd_queue_pending(tr)) break;
        if (__atomic_exchange_n(&tr->tx_busy, 1u, __ATOMIC_ACQUIRE)) break;
    }
    return result;
}

int bt_usb_transport_send_command(bt_usb_transport_t *tr, uint16_t opcode,
                                  const uint8_t *params, uint8_t param_len) {
    if (!tr || (param_len > 0 && !params)) {
        return -1;
    }
    if (!cmd_queue_push(tr, opcode, params, param_len)) {
        return -1;
    }
    return tx_flush(tr);
}

static int send_hci_cmd(bt_usb_transport_t *tr, uint16_t opcode,
                        const uint8_t *params, uint8_t param_len) {
    return bt_usb_transport_send_command(tr, opcode, params, param_len);
}

static void note_command_result(bt_usb_transport_t *tr, uint16_t opcode,
                                const uint8_t *params, uint8_t plen) {
    uint8_t n = plen;
    if (n > sizeof(tr->cc_data)) n = (uint8_t)sizeof(tr->cc_data);
    if (n && params) memcpy(tr->cc_data, params, n);
    tr->cc_len = n;
    __atomic_store_n(&tr->cc_opcode, opcode, __ATOMIC_RELAXED);
    __atomic_store_n(&tr->cc_status, (n && params) ? params[0] : 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&tr->cc_param, (n >= 2u && params) ? params[1] : 0u, __ATOMIC_RELAXED);
    __atomic_add_fetch(&tr->cc_seq, 1u, __ATOMIC_RELEASE);
}

static void command_response(bt_usb_transport_t *tr, const uint8_t *evt, size_t evt_len) {
    uint16_t opcode = 0;
    if (evt[0] == HCI_EVT_COMMAND_COMPLETE && evt_len >= 5u) {
        opcode = hci_get_le16(evt + 3);
    } else if (evt[0] == HCI_EVT_COMMAND_STATUS && evt_len >= 6u) {
        opcode = hci_get_le16(evt + 4);
    }
    /* Opcode 0 (NOP) so devolve credito; nao responde a nenhum comando nosso. */
    if (opcode != 0 && __atomic_load_n(&tr->cmd_inflight, __ATOMIC_ACQUIRE) &&
        opcode == tr->inflight_opcode) {
        __atomic_store_n(&tr->cmd_inflight, 0u, __ATOMIC_RELEASE);
    }
}

static void request_next_name(bt_usb_transport_t *tr);

void bt_usb_transport_pump(bt_usb_transport_t *tr, uint32_t elapsed_ms) {
    if (!tr) return;
    if (__atomic_load_n(&tr->cmd_inflight, __ATOMIC_ACQUIRE)) {
        tr->inflight_age_ms += elapsed_ms;
        if (tr->inflight_age_ms >= BT_USB_CMD_TIMEOUT_MS) {
            const uint16_t expired_op = tr->inflight_opcode;
            tr->cmd_timeouts++;
            __atomic_store_n(&tr->cmd_inflight, 0u, __ATOMIC_RELEASE);
            if (tr->hal.release_control) {
                tr->hal.release_control(tr->hal.ctx);
            }
            if (tr->state == BT_INIT_WAIT_RESET ||
                tr->state == BT_INIT_WAIT_LOCAL_VER ||
                tr->state == BT_INIT_WAIT_BD_ADDR) {
                if (tr->init_retries + 1u < BT_USB_INIT_MAX_RETRIES) {
                    tr->init_retries++;
                    send_hci_cmd(tr, expired_op, NULL, 0);
                } else {
                    tr->state = BT_INIT_ERROR;
                    tr->last_error = -12;
                }
            }
        }
    }
    tx_flush(tr);
    if (tr->name_hold && !tr->scan_active &&
        !__atomic_load_n(&tr->cmd_inflight, __ATOMIC_ACQUIRE)) {
        if (tr->name_wait_ms > elapsed_ms) {
            tr->name_wait_ms = (uint16_t)(tr->name_wait_ms - elapsed_ms);
        } else {
            tr->name_wait_ms = 0;
            tr->name_hold = 0;
            request_next_name(tr);
        }
    }
}

void bt_usb_transport_enable_events(bt_usb_transport_t *tr, bool enable) {
    if (!tr) return;
    __atomic_store_n(&tr->events_enabled, enable ? 1u : 0u, __ATOMIC_RELEASE);
}

static void event_fifo_push(bt_usb_transport_t *tr, const uint8_t *evt, size_t len) {
    const uint32_t t = __atomic_load_n(&tr->evt_tail, __ATOMIC_RELAXED);
    const uint32_t h = __atomic_load_n(&tr->evt_head, __ATOMIC_ACQUIRE);
    if (t - h >= BT_USB_EVT_FIFO_DEPTH || len > sizeof(tr->evt_fifo[0].data)) {
        tr->evt_dropped++;
        return;
    }
    bt_usb_evt_slot_t *slot = &tr->evt_fifo[t % BT_USB_EVT_FIFO_DEPTH];
    slot->len = (uint16_t)len;
    memcpy(slot->data, evt, len);
    __atomic_store_n(&tr->evt_tail, t + 1u, __ATOMIC_RELEASE);
}

int bt_usb_transport_pop_event(bt_usb_transport_t *tr, uint8_t *out, size_t out_cap) {
    if (!tr || !out) return -1;
    const uint32_t h = __atomic_load_n(&tr->evt_head, __ATOMIC_RELAXED);
    if (h == __atomic_load_n(&tr->evt_tail, __ATOMIC_ACQUIRE)) return 0;
    const bt_usb_evt_slot_t *slot = &tr->evt_fifo[h % BT_USB_EVT_FIFO_DEPTH];
    if (slot->len > out_cap) return -1;
    memcpy(out, slot->data, slot->len);
    const int n = (int)slot->len;
    __atomic_store_n(&tr->evt_head, h + 1u, __ATOMIC_RELEASE);
    return n;
}

uint32_t bt_usb_transport_events_dropped(const bt_usb_transport_t *tr) {
    return tr ? __atomic_load_n(&tr->evt_dropped, __ATOMIC_RELAXED) : 0u;
}

static int arm_intr_in(bt_usb_transport_t *tr) {
    if (tr->intr_armed) {
        return 0; /* transferencia ja pendente no HAL */
    }
    uint16_t max_pkt = tr->dev.intr_in.wMaxPacketSize;
    if (max_pkt == 0 || max_pkt > sizeof(tr->evt_rx_chunk)) {
        max_pkt = sizeof(tr->evt_rx_chunk);
    }
    const int rc = tr->hal.submit_intr_in(tr->hal.ctx, tr->evt_rx_chunk, max_pkt);
    if (rc == 0) tr->intr_armed = true;
    return rc;
}

int bt_usb_transport_start_init(bt_usb_transport_t *tr) {
    if (!tr) {
        return -1;
    }
    tr->evt_accum_len = 0;
    tr->has_local_ver = false;
    tr->has_bd_addr = false;
    tr->last_error = 0;
    tr->init_retries = 0;

    if (arm_intr_in(tr) != 0) {
        tr->state = BT_INIT_ERROR;
        tr->last_error = -10;
        return -10;
    }

    tr->state = BT_INIT_WAIT_RESET;
    if (send_hci_cmd(tr, HCI_OP_RESET, NULL, 0) != 0) {
        tr->state = BT_INIT_ERROR;
        tr->last_error = -11;
        return -11;
    }
    return 0;
}

static void record_inquiry_items(bt_usb_transport_t *tr,
                                 const hci_inquiry_item_t *items, int count) {
    for (int i = 0; i < count; i++) {
        const hci_inquiry_item_t *src = &items[i];
        int match = -1;
        for (size_t j = 0; j < tr->scan_count; j++) {
            if (memcmp(tr->scan_results[j].bd_addr, src->bd_addr, 6u) == 0) {
                match = (int)j;
                break;
            }
        }
        if (match >= 0) {
            hci_inquiry_item_t *dst = &tr->scan_results[match];
            dst->class_of_device = src->class_of_device;
            if (src->has_rssi) {
                dst->rssi = src->rssi;
                dst->has_rssi = true;
            }
            if (src->name[0] != '\0') {
                memcpy(dst->name, src->name, sizeof(dst->name));
            }
        } else if (tr->scan_count < BT_MAX_DISCOVERED_DEVICES) {
            tr->scan_results[tr->scan_count++] = *src;
        }
    }
}

/* O resultado curto da busca nao traz nome. O aparelho responde o nome real aqui. */
static void request_next_name(bt_usb_transport_t *tr) {
    if (tr->name_inflight || tr->name_hold) return;
    for (size_t i = 0; i < tr->scan_count; i++) {
        if (tr->name_asked[i] || tr->scan_results[i].name[0] != '\0') continue;
        uint8_t p[10];
        const uint16_t clk = (uint16_t)(tr->scan_results[i].clock_offset | 0x8000u);
        memcpy(p, tr->scan_results[i].bd_addr, 6u);
        p[6] = tr->scan_results[i].page_scan_rep_mode;
        p[7] = 0;
        p[8] = (uint8_t)clk;
        p[9] = (uint8_t)(clk >> 8);
        if (send_hci_cmd(tr, HCI_OP_REMOTE_NAME_REQUEST, p, sizeof p) != 0) return;
        tr->name_asked[i] = 1;
        tr->name_inflight = 1;
        return;
    }
}

static void apply_remote_name(bt_usb_transport_t *tr, const uint8_t *evt, size_t evt_len) {
    tr->name_inflight = 0;
    if (evt_len >= 9u) {
        const uint8_t *bd = evt + 3;
        const uint8_t *raw = evt + 9;
        const size_t raw_len = evt_len - 9u;
        for (size_t i = 0; i < tr->scan_count; i++) {
            if (memcmp(tr->scan_results[i].bd_addr, bd, 6u) != 0) continue;
            tr->name_asked[i] = 2;
            if (evt[2] == 0) {
                size_t n = 0;
                while (n < 31u && n < raw_len && raw[n] != 0) n++;
                memcpy(tr->scan_results[i].name, raw, n);
                tr->scan_results[i].name[n] = '\0';
            }
            break;
        }
    }
    request_next_name(tr);
}

static void handle_hci_event(bt_usb_transport_t *tr,
                             const uint8_t *evt, size_t evt_len) {
    if (evt_len < HCI_EVT_HDR_SIZE) {
        return;
    }

    uint8_t evt_code = evt[0];
    __atomic_store_n(&tr->trace_evt, evt_code, __ATOMIC_RELAXED);
    __atomic_add_fetch(&tr->trace_events, 1u, __ATOMIC_RELAXED);

    if (evt_code == HCI_EVT_COMMAND_COMPLETE || evt_code == HCI_EVT_COMMAND_STATUS) {
        command_response(tr, evt, evt_len);
        if (evt_code == HCI_EVT_COMMAND_COMPLETE) {
            hci_cmd_complete_t done;
            if (hci_parse_command_complete(evt, evt_len, &done) == 0)
                note_command_result(tr, done.opcode, done.params, done.params_len);
        } else {
            hci_cmd_status_t done;
            if (hci_parse_command_status(evt, evt_len, &done) == 0)
                note_command_result(tr, done.opcode, &done.status, 1u);
        }
    }

    if (evt_code == HCI_EVT_INQUIRY_RESULT ||
        evt_code == HCI_EVT_INQUIRY_RESULT_RSSI ||
        evt_code == HCI_EVT_EXTENDED_INQUIRY_RESULT) {
        hci_inquiry_item_t items[8];
        int n = hci_decode_inquiry_event(evt, evt_len, items, 8);
        if (n > 0) {
            record_inquiry_items(tr, items, n);
            if (tr->scan_active) tr->name_hold = 1;
            else request_next_name(tr);
        }
        return;
    }

    if (evt_code == HCI_EVT_REMOTE_NAME_REQ_COMPLETE) {
        apply_remote_name(tr, evt, evt_len);
        return;
    }

    if (evt_code == HCI_EVT_INQUIRY_COMPLETE) {
        tr->scan_active = false;
        tr->scan_done = true;
        tr->name_hold = 0;
        request_next_name(tr);
        return;
    }

    if (evt_code == HCI_EVT_COMMAND_STATUS) {
        hci_cmd_status_t cs;
        if (hci_parse_command_status(evt, evt_len, &cs) == 0) {
            if (cs.opcode == HCI_OP_INQUIRY && cs.status != 0x00) {
                tr->scan_active = false;
                tr->scan_done = true;
                tr->last_error = -(int)cs.status;
            }
            if (cs.opcode == HCI_OP_REMOTE_NAME_REQUEST && cs.status != 0x00) {
                tr->name_inflight = 0;
                for (size_t i = 0; i < tr->scan_count; i++) {
                    if (tr->name_asked[i] == 1) tr->name_asked[i] = 0;
                }
                if (tr->name_tries < 4u) {
                    tr->name_tries++;
                    tr->name_hold = 1;
                    tr->name_wait_ms = 400u;
                }
            }
        }
        return;
    }

    if (evt_code != HCI_EVT_COMMAND_COMPLETE) {
        return;
    }

    hci_cmd_complete_t cc;
    if (hci_parse_command_complete(evt, evt_len, &cc) != 0) {
        return;
    }

    if (cc.opcode == HCI_OP_SET_EVENT_MASK && tr->scan_active) {
        if (cc.params_len >= 1u && cc.params[0] != 0x00u && !tr->mask_fallback_tried) {
            static const uint8_t legacy_mask[8] = HCI_EVENT_MASK_LEGACY_INIT;
            tr->mask_fallback_tried = 1u;
            send_hci_cmd(tr, HCI_OP_SET_EVENT_MASK, legacy_mask, sizeof(legacy_mask));
            return;
        }
        uint8_t mode = 0x02u; /* Extended Inquiry Result */
        send_hci_cmd(tr, HCI_OP_WRITE_INQUIRY_MODE, &mode, 1);
        return;
    }

    if (cc.opcode == HCI_OP_WRITE_INQUIRY_MODE && tr->scan_active) {
        if (cc.params_len >= 1u && cc.params[0] != 0x00u && !tr->inq_mode_fallback_tried) {
            uint8_t mode = 0x01u; /* Inquiry Result with RSSI fallback */
            tr->inq_mode_fallback_tried = 1u;
            send_hci_cmd(tr, HCI_OP_WRITE_INQUIRY_MODE, &mode, 1);
            return;
        }
        /* GIAC LAP = 0x9E8B33 (little-endian: 33 8B 9E), length, unlimited responses */
        uint8_t inq_params[5] = { 0x33u, 0x8Bu, 0x9Eu, tr->scan_duration, 0x00u };
        if (send_hci_cmd(tr, HCI_OP_INQUIRY, inq_params, sizeof(inq_params)) != 0) {
            tr->scan_active = false;
            tr->scan_done = true;
            tr->last_error = -30;
        }
        return;
    }

    switch (tr->state) {
        case BT_INIT_WAIT_RESET:
            if (cc.opcode == HCI_OP_RESET) {
                if (cc.params_len >= 1 && cc.params[0] == 0x00) {
                    tr->init_retries = 0;
                    tr->state = BT_INIT_WAIT_LOCAL_VER;
                    if (send_hci_cmd(tr, HCI_OP_READ_LOCAL_VERSION_INFO, NULL, 0) != 0) {
                        tr->state = BT_INIT_ERROR;
                        tr->last_error = -20;
                    }
                } else {
                    tr->state = BT_INIT_ERROR;
                    tr->last_error = -21;
                }
            }
            break;

        case BT_INIT_WAIT_LOCAL_VER:
            if (cc.opcode == HCI_OP_READ_LOCAL_VERSION_INFO) {
                if (hci_decode_local_version(&cc, &tr->local_ver) == 0 &&
                    tr->local_ver.status == 0x00) {
                    tr->init_retries = 0;
                    tr->has_local_ver = true;
                    tr->state = BT_INIT_WAIT_BD_ADDR;
                    if (send_hci_cmd(tr, HCI_OP_READ_BD_ADDR, NULL, 0) != 0) {
                        tr->state = BT_INIT_ERROR;
                        tr->last_error = -22;
                    }
                } else {
                    tr->state = BT_INIT_ERROR;
                    tr->last_error = -23;
                }
            }
            break;

        case BT_INIT_WAIT_BD_ADDR:
            if (cc.opcode == HCI_OP_READ_BD_ADDR) {
                if (hci_decode_bd_addr(&cc, &tr->bd_addr) == 0 &&
                    tr->bd_addr.status == 0x00) {
                    tr->init_retries = 0;
                    tr->has_bd_addr = true;
                    tr->state = BT_INIT_READY;
                } else {
                    tr->state = BT_INIT_ERROR;
                    tr->last_error = -24;
                }
            }
            break;

        default:
            break;
    }
}

int bt_usb_transport_start_inquiry(bt_usb_transport_t *tr, uint8_t duration_1280ms) {
    if (!tr || tr->state != BT_INIT_READY) {
        return -1;
    }
    if (duration_1280ms < 1u) duration_1280ms = 1u;
    if (duration_1280ms > 48u) duration_1280ms = 48u;

    tr->scan_active = true;
    tr->scan_done = false;
    tr->scan_count = 0;
    tr->scan_duration = duration_1280ms;
    memset(tr->name_asked, 0, sizeof tr->name_asked);
    tr->name_inflight = 0;
    tr->name_hold = 0;
    tr->name_tries = 0;
    tr->name_wait_ms = 0;
    tr->mask_fallback_tried = 0;
    tr->inq_mode_fallback_tried = 0;

    /* Interrupt IN ja esta armado desde o init (on_intr_in rearma); armar aqui,
     * de outra thread, poderia reenfileirar um TRB ainda pendente no HAL. */

    /* BT 2.1+ Classic + SSP + EIR mask without reserved/LE bits (safe for cheap clones). */
    static const uint8_t event_mask[8] = HCI_EVENT_MASK_CLASSIC_SSP_INIT;
    const int rc = send_hci_cmd(tr, HCI_OP_SET_EVENT_MASK, event_mask, sizeof(event_mask));
    if (rc != 0) {
        tr->scan_active = false;
        tr->last_error = rc;
    }
    return rc;
}

int bt_usb_transport_stop_inquiry(bt_usb_transport_t *tr) {
    if (!tr || !tr->scan_active) {
        return 0;
    }
    tr->scan_active = false;
    return send_hci_cmd(tr, HCI_OP_INQUIRY_CANCEL, NULL, 0);
}

void bt_usb_transport_on_intr_in(bt_usb_transport_t *tr,
                                 const uint8_t *data, size_t len) {
    if (!tr) return;
    tr->intr_armed = false; /* o HAL completou a transferencia pendente */
    if (!data || len == 0) goto rearm;

    if (tr->evt_accum_len + len > sizeof(tr->evt_accum)) {
        /* Um evento mal formado nao pode calar o endpoint. */
        tr->evt_accum_len = 0;
        goto rearm;
    }

    memcpy(tr->evt_accum + tr->evt_accum_len, data, len);
    tr->evt_accum_len += len;

    while (tr->evt_accum_len >= HCI_EVT_HDR_SIZE) {
        /* Pacote USB curto vem preenchido com zero. Evento 0 nao existe no HCI. */
        if (tr->evt_accum[0] == 0) {
            size_t z = 0;
            while (z < tr->evt_accum_len && tr->evt_accum[z] == 0) z++;
            if (z >= tr->evt_accum_len) {
                tr->evt_accum_len = 0;
                break;
            }
            memmove(tr->evt_accum, tr->evt_accum + z, tr->evt_accum_len - z);
            tr->evt_accum_len -= z;
            continue;
        }
        size_t expected = HCI_EVT_HDR_SIZE + (size_t)tr->evt_accum[1];
        if (tr->evt_accum_len < expected) {
            break; /* Wait for remaining USB frames */
        }

        if (__atomic_load_n(&tr->events_enabled, __ATOMIC_ACQUIRE)) {
            event_fifo_push(tr, tr->evt_accum, expected);
        }
        handle_hci_event(tr, tr->evt_accum, expected);

        size_t rem = tr->evt_accum_len - expected;
        if (rem > 0) {
            memmove(tr->evt_accum, tr->evt_accum + expected, rem);
        }
        tr->evt_accum_len = rem;
    }

    /* Comandos que esperavam a resposta que acabou de chegar. */
    tx_flush(tr);

rearm:
    /* Segue ouvindo depois do init: a sessao de pareamento depende dos eventos. */
    if (tr->state != BT_INIT_ERROR) {
        arm_intr_in(tr);
    }
}
