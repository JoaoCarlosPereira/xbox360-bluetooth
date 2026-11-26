#include "rtl_boot.h"
#include "../chipset/rtl8761b.h"
#include "../hci/hci_core.h"
#include "../manager/bt_trace.h"
#include "../hal/xbox360_usbd.h"
#include <xsdk/kernel.h>

#define FW_CAP 49152u
#define CFG_CAP 1024u
#define FW_FILE "\\Device\\Mass0\\rtl8761bu_fw.bin"
#define CFG_FILE "\\Device\\Mass0\\rtl8761bu_config.bin"

enum {
    PHASE_IDLE = 0,
    PHASE_ROM,
    PHASE_UPLOAD,
    PHASE_SETTLE,
    PHASE_COMMIT,
    PHASE_VER,
    PHASE_DONE,
    PHASE_FAIL
};

static uint8_t phase;
static uint8_t skip_upload;
static uint16_t wait_op;
static uint32_t seen_seq;
static uint32_t wait_ticks;
static size_t next_chunk;
static size_t chunk_total;
static uint32_t fw_size;
static uint32_t cfg_size;
static uint8_t fw_mem[FW_CAP];
static uint8_t cfg_mem[CFG_CAP];
static rtl_fw_image_t image;

void rtl_boot_reset(void) {
    /* Queda no meio do reset do patch nao dispara outra carga por cima. */
    if (phase == PHASE_SETTLE || phase == PHASE_COMMIT || phase == PHASE_VER)
        skip_upload = 1;
    else
        skip_upload = 0;
    phase = PHASE_IDLE;
    wait_op = 0;
    wait_ticks = 0;
    next_chunk = 0;
    chunk_total = 0;
}

bool rtl_boot_pending(void) {
    return phase == PHASE_ROM || phase == PHASE_UPLOAD || phase == PHASE_SETTLE ||
           phase == PHASE_COMMIT || phase == PHASE_VER;
}

/* true quando o comando esperado respondeu. status e param so valem nesse caso. */
static bool command_answered(bt_usb_transport_t *tr, uint8_t *status, uint8_t *param) {
    const uint32_t seq = __atomic_load_n(&tr->cc_seq, __ATOMIC_ACQUIRE);
    if (seq == seen_seq) return false;
    const uint32_t op = __atomic_load_n(&tr->cc_opcode, __ATOMIC_RELAXED);
    seen_seq = seq;
    if (op != wait_op) return false;
    *status = (uint8_t)__atomic_load_n(&tr->cc_status, __ATOMIC_RELAXED);
    *param = (uint8_t)__atomic_load_n(&tr->cc_param, __ATOMIC_RELAXED);
    return true;
}

static bool read_file(const char *path, uint16_t path_size, uint8_t *dst,
                      uint32_t cap, uint32_t *out_n) {
    const ANSI_STRING name = { path_size - 1, path_size, path };
    const OBJECT_ATTRIBUTES attributes = { .name = &name };
    HANDLE file;
    IO_STATUS_BLOCK io = { 0 };
    NTSTATUS status = NtCreateFile(&file, SYNCHRONIZE | GENERIC_READ, &attributes, &io, NULL,
                                   FILE_ATTRIBUTE_NORMAL, 0, FILE_OPEN,
                                   FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE);
    if (!NT_SUCCESS(status)) return false;
    status = NtReadFile(file, 0, NULL, NULL, &io, dst, cap, NULL);
    const bool ok = NT_SUCCESS(status) && io.information > 0 && io.information <= cap;
    if (ok) *out_n = (uint32_t)io.information;
    return NT_SUCCESS(NtClose(file)) && ok;
}

static int send_chunk(void *ctx, uint16_t opcode, const uint8_t *params, uint8_t param_len) {
    return bt_usb_transport_send_command(ctx, opcode, params, param_len);
}

static void fail(const char *why, uint32_t value) {
    bt_trace("fw: %s %u", why, value);
    phase = PHASE_FAIL;
    wait_op = 0;
}

bool rtl_boot_ready(bt_usb_transport_t *tr) {
    if (!tr || !tr->has_local_ver) return false;
    if (phase == PHASE_FAIL || phase == PHASE_DONE) return true;
    if (!rtl_needs_firmware(&tr->local_ver)) return true;

    if (phase == PHASE_IDLE) {
        const hci_local_version_info_t *v = &tr->local_ver;
        bt_trace("radio: mfr=%04X lmp=%04X hci=%02X", v->manufacturer, v->lmp_subversion,
                 v->hci_version);
        if (skip_upload) {
            bt_trace("fw: skip reload lmp=%04X", v->lmp_subversion);
            phase = PHASE_FAIL;
            return true;
        }
        if (!read_file(FW_FILE, sizeof FW_FILE, fw_mem, FW_CAP, &fw_size)) {
            fail("file missing", 0);
            return true;
        }
        if (!read_file(CFG_FILE, sizeof CFG_FILE, cfg_mem, CFG_CAP, &cfg_size)) cfg_size = 0;
        seen_seq = __atomic_load_n(&tr->cc_seq, __ATOMIC_ACQUIRE);
        if (bt_usb_transport_send_command(tr, RTL_OP_READ_ROM_VERSION, NULL, 0) != 0) {
            fail("rom command", 0);
            return true;
        }
        wait_op = RTL_OP_READ_ROM_VERSION;
        wait_ticks = 0;
        phase = PHASE_ROM;
        return false;
    }

    if (phase == PHASE_ROM || phase == PHASE_UPLOAD) {
        uint8_t status = 0, param = 0;
        if (!command_answered(tr, &status, &param)) {
            if (++wait_ticks < 200u) return false;
            x360_usbd_release_control();
            fail("timeout", wait_op);
            return true;
        }
        if (status != 0x00u) {
            fail("command status", status);
            return true;
        }
        wait_op = 0;
        wait_ticks = 0;
        if (phase == PHASE_ROM) {
            const int prep = rtl_fw_prepare(&image, fw_mem, fw_size,
                                            cfg_size ? cfg_mem : NULL, cfg_size, param);
            if (prep != 0) {
                fail("prepare", (uint32_t)(-prep));
                return true;
            }
            chunk_total = rtl_fw_chunk_count(&image);
            next_chunk = 0;
            bt_trace("fw: rom=%u chunks=%u", param, (unsigned)chunk_total);
            phase = PHASE_UPLOAD;
        }
        if (phase == PHASE_UPLOAD) {
            if (next_chunk >= chunk_total) {
                bt_trace("fw: upload done, committing");
                wait_ticks = 0;
                phase = PHASE_SETTLE;
                return false;
            } else if ((seen_seq = __atomic_load_n(&tr->cc_seq, __ATOMIC_ACQUIRE)),
                       rtl_fw_send_chunk(&image, next_chunk, send_chunk, tr) != 0) {
                fail("chunk", (uint32_t)next_chunk);
                return true;
            } else {
                wait_op = RTL_OP_DOWNLOAD_CMD;
                next_chunk++;
                if (next_chunk == 1u || next_chunk == chunk_total || (next_chunk % 20u) == 0u)
                    bt_trace("fw: chunk %u/%u", (unsigned)next_chunk, (unsigned)chunk_total);
                return false;
            }
        }
    }

    if (phase == PHASE_SETTLE) {
        if (++wait_ticks < 20u) return false;
        seen_seq = __atomic_load_n(&tr->cc_seq, __ATOMIC_ACQUIRE);
        if (bt_usb_transport_send_command(tr, HCI_OP_RESET, NULL, 0) != 0) {
            fail("reset send", 0);
            return true;
        }
        wait_op = HCI_OP_RESET;
        wait_ticks = 0;
        phase = PHASE_COMMIT;
        return false;
    }

    if (phase == PHASE_COMMIT || phase == PHASE_VER) {
        uint8_t status = 0, param = 0;
        if (!command_answered(tr, &status, &param)) {
            if (++wait_ticks < 200u) return false;
            x360_usbd_release_control();
            fail("commit timeout", wait_op);
            return true;
        }
        if (status != 0x00u) {
            fail("commit status", status);
            return true;
        }
        wait_ticks = 0;
        if (phase == PHASE_COMMIT) {
            seen_seq = __atomic_load_n(&tr->cc_seq, __ATOMIC_ACQUIRE);
            if (bt_usb_transport_send_command(tr, HCI_OP_READ_LOCAL_VERSION_INFO, NULL, 0) != 0) {
                fail("version send", 0);
                return true;
            }
            wait_op = HCI_OP_READ_LOCAL_VERSION_INFO;
            phase = PHASE_VER;
            return false;
        }
        hci_cmd_complete_t cc = {
            .params = tr->cc_data,
            .params_len = tr->cc_len
        };
        hci_local_version_info_t ver;
        if (hci_decode_local_version(&cc, &ver) != 0 || ver.status != 0x00u) {
            fail("version", tr->cc_len);
            return true;
        }
        tr->local_ver = ver;
        tr->has_local_ver = true;
        bt_trace("fw: committed lmp=%04X", ver.lmp_subversion);
        phase = rtl_needs_firmware(&ver) ? PHASE_FAIL : PHASE_DONE;
        return true;
    }
    return true;
}
