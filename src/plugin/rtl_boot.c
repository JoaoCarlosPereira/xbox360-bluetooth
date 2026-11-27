#include "rtl_boot.h"
#include "../chipset/bt_chipset.h"
#include "../chipset/rtl8761b.h"
#include "../hci/hci_core.h"
#include "../manager/bt_trace.h"
#include "../hal/xbox360_usbd.h"
#include <xsdk/kernel.h>

#define FW_CAP  65536u
#define CFG_CAP 1024u
#define USB_FW_DIR_PREFIX  "\\Device\\Mass0\\bt_fw\\"
#define USB_ROOT_PREFIX    "\\Device\\Mass0\\"
#define POST_RESET_TICKS   10u /* ~100 ms settle para clones CSR/Barrot/Actions */

enum {
    PHASE_IDLE = 0,
    PHASE_DELAY,
    PHASE_ROM,
    PHASE_UPLOAD,
    PHASE_SETTLE,
    PHASE_COMMIT,
    PHASE_VER,
    PHASE_DONE,
    PHASE_FW_MISSING,
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
    return phase == PHASE_DELAY || phase == PHASE_ROM || phase == PHASE_UPLOAD ||
           phase == PHASE_SETTLE || phase == PHASE_COMMIT || phase == PHASE_VER;
}

bool rtl_boot_fw_missing(void) {
    return phase == PHASE_FW_MISSING;
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

static uint16_t build_path(char *dst, size_t cap, const char *prefix, const char *name) {
    size_t n = 0;
    if (!dst || !prefix || !name || cap < 2u) return 0;
    while (prefix[n] != '\0') {
        if (n + 1u >= cap) return 0;
        dst[n] = prefix[n];
        n++;
    }
    for (size_t i = 0; name[i] != '\0'; i++) {
        if (n + 1u >= cap) return 0;
        dst[n++] = name[i];
    }
    dst[n] = '\0';
    return (uint16_t)(n + 1u);
}

static bool read_file(const char *path, uint16_t path_size, uint8_t *dst,
                      uint32_t cap, uint32_t *out_n) {
    if (!path || path_size < 2u) return false;
    const ANSI_STRING name = { (uint16_t)(path_size - 1u), path_size, path };
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

/* Procura primeiro em Usb:\bt_fw\<nome> e depois na raiz Usb:\<nome> (retrocompativel). */
static bool read_fw_file(const char *basename, uint8_t *dst, uint32_t cap, uint32_t *out_n) {
    char path[96];
    uint16_t sz;
    if (!basename) return false;
    sz = build_path(path, sizeof path, USB_FW_DIR_PREFIX, basename);
    if (sz > 1u && read_file(path, sz, dst, cap, out_n)) return true;
    sz = build_path(path, sizeof path, USB_ROOT_PREFIX, basename);
    if (sz > 1u && read_file(path, sz, dst, cap, out_n)) return true;
    return false;
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
    if (phase == PHASE_FW_MISSING) return false;
    if (phase == PHASE_FAIL || phase == PHASE_DONE) return true;

    if (phase == PHASE_IDLE) {
        const hci_local_version_info_t *v = &tr->local_ver;
        bt_chipset_profile_t profile;
        bt_chipset_identify(tr->dev.vid, tr->dev.pid, v, &profile);
        bt_trace("radio: vid=%04X pid=%04X mfr=%04X hci=%02X rev=%04X lmp=%04X chip=%s fw=%u",
                 tr->dev.vid, tr->dev.pid, v->manufacturer, v->hci_version,
                 v->hci_revision, v->lmp_subversion, profile.name, (unsigned)profile.needs_fw);

        if (!profile.needs_fw) {
            if ((profile.quirks & BT_QUIRK_POST_RESET_DELAY) != 0u) {
                wait_ticks = 0;
                phase = PHASE_DELAY;
                return false;
            }
            phase = PHASE_DONE;
            return true;
        }

        if (skip_upload) {
            bt_trace("fw: skip reload lmp=%04X", v->lmp_subversion);
            phase = PHASE_FAIL;
            return true;
        }
        if (!read_fw_file(profile.fw_name, fw_mem, FW_CAP, &fw_size)) {
            bt_trace("fw: missing %s", profile.fw_name ? profile.fw_name : "(null)");
            phase = PHASE_FW_MISSING;
            wait_op = 0;
            return false;
        }
        if (!profile.cfg_name || !read_fw_file(profile.cfg_name, cfg_mem, CFG_CAP, &cfg_size)) {
            cfg_size = 0;
        }
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

    if (phase == PHASE_DELAY) {
        if (++wait_ticks < POST_RESET_TICKS) return false;
        phase = PHASE_DONE;
        return true;
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
            .opcode = HCI_OP_READ_LOCAL_VERSION_INFO,
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
        bt_chipset_profile_t post;
        bt_chipset_identify(tr->dev.vid, tr->dev.pid, &ver, &post);
        phase = post.needs_fw ? PHASE_FAIL : PHASE_DONE;
        return true;
    }
    return true;
}
