/*
 * BTManager.xex — Xbox 360 Bluetooth Connection & Pairing UI
 * Inspired by the original Xbox 360 NXE / Guide interface + modern 10-foot UI.
 * Built with xenosworks/xsdk (no proprietary Microsoft SDK).
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/present.h"
#include "xsdk/thread.h"
#include "dashboard.h"
#include "dashboard_draw.h"
#ifdef BTMANAGER_XENIA
#include "gpu_private.h"
#endif

#include "../manager/bt_manager.h"
#include "../manager/bt_phase1.h"
#include "../manager/bt_trace.h"

static void note(const char *format, ...) {
    char line[320];
    va_list args;
    va_start(args, format);
    const int written = _vsnprintf(line, sizeof line, format, args);
    va_end(args);
    if (written < 0) return;
    line[sizeof line - 1] = 0;
    xsdk_log_write("%s", line);
    bt_trace("%s", line);
}

#ifndef BTMANAGER_XENIA
#include "../manager/console_mailbox.h"
#include "../hal/xbox360_usbd.h"

typedef struct {
    uint64_t token;
    uint64_t started;
    uint64_t last_poll;
    uint64_t toast_until;   /* mantem frase de evento visivel por 4.5s sem sobrescrever */
    uint32_t sent_sequence;
    uint32_t adapter_stage;
    bool sent_command;
    bool was_scanning;
    uint32_t watch_scan;    /* leituras do arquivo de busca depois do Y, ate a lista aparecer */
    bool stored_loaded;     /* bt_devices.ini lido (ou ausente) ao menos uma vez */
    uint32_t reload_tries;  /* releituras pendentes depois de um pareamento concluido */
    uint32_t reload_floor;  /* so para quando a lista tiver pelo menos estes controles */
    uint32_t last_phrase;   /* ultimo phrase_id visto no reply */
    char toast_msg[64];
    enum { IPC_INITIAL, IPC_REPLIED, IPC_TIMEOUT, IPC_WRITE_FAILED } log_state;
} plugin_client_t;

typedef struct {
    pthread_mutex_t lock;
    volatile bool   stop;
    bool            has_cmd;
    uint32_t        cmd_seq;
    uint32_t        cmd;
    uint32_t        cmd_slot;
    uint8_t         cmd_bd_addr[6];
    bool            local_override_paired;
    bt_paired_device_t local_paired[BT_MAX_PAIRED_DEVICES];
    bool            local_toast_pending;
    char            local_toast[64];
    bool            snapshot_ready;
    bool            stored_loaded;
    bt_ipc_state_t  snapshot;
} ipc_worker_t;

/* O DS4 gravado vive em Mass0:\bt_devices.ini (escrito pelo plugin). A UI so le. */
#define BT_DEVICES_INI "\\Device\\Mass0\\bt_devices.ini"
enum { STORED_RELOAD_TRIES = 50 /* 50 polls de 100 ms = 5 s */ };

static bool read_devices_ini(char *buffer, size_t capacity, size_t *size) {
    const ANSI_STRING name = { sizeof BT_DEVICES_INI - 1, sizeof BT_DEVICES_INI, BT_DEVICES_INI };
    const OBJECT_ATTRIBUTES attributes = { .name = &name };
    HANDLE file;
    IO_STATUS_BLOCK io = { 0 };
    NTSTATUS status = NtCreateFile(&file, SYNCHRONIZE | GENERIC_READ, &attributes, &io, NULL,
                                   FILE_ATTRIBUTE_NORMAL, 0, FILE_OPEN,
                                   FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE);
    *size = 0;
    if (!NT_SUCCESS(status)) {
        /* Volume respondeu e nao ha arquivo: primeira execucao, sem DS4 gravado. */
        return status == STATUS_OBJECT_NAME_NOT_FOUND;
    }
    status = NtReadFile(file, 0, NULL, NULL, &io, buffer, (uint32_t)capacity, NULL);
    if (NT_SUCCESS(status)) *size = (size_t)io.information;
    const bool answered = NT_SUCCESS(status) || status == STATUS_END_OF_FILE;
    NtClose(file);
    return answered;
}

/* Atualiza ipc->paired a partir do arquivo. Um arquivo mais curto que a lista
 * atual e a gravacao anterior ainda no disco: nao apaga quem ja esta na tela. */
static bool load_stored_ds4(bt_ipc_state_t *ipc, int keep_at_least) {
    char text[1024];
    size_t size = 0;
    bt_paired_device_t previous[BT_MAX_PAIRED_DEVICES];
    if (!read_devices_ini(text, sizeof text - 1u, &size)) return false;
    text[size] = 0;
    memcpy(previous, ipc->paired, sizeof previous);
    for (size_t i = 0; i < BT_MAX_PAIRED_DEVICES; i++) ipc->paired[i].valid = false;
    bt_manager_import_ini(ipc, text, size);
    if (bt_manager_paired_count(ipc) < keep_at_least)
        memcpy(ipc->paired, previous, sizeof previous);
    return true;
}

/* Le o DS4 gravado no inicio (ate o volume responder) e apos um pareamento concluido. */
static void refresh_stored_ds4(bt_ipc_state_t *ipc, plugin_client_t *client) {
    if (client->stored_loaded && client->reload_tries == 0) return;
    const int before = bt_manager_paired_count(ipc);
    if (client->stored_loaded) client->reload_tries--;
    const int keep = client->reload_floor ? before : 0;
    if (!load_stored_ds4(ipc, keep)) return;
    const int after = bt_manager_paired_count(ipc);
    if (!client->stored_loaded || after != before)
        note("ui: DualShock 4 gravado=%u", (unsigned)after);
    client->stored_loaded = true;
    if (client->reload_floor == 0) {
        if (after > 0) client->reload_tries = 0;
    } else if ((uint32_t)after >= client->reload_floor) {
        client->reload_tries = 0;
        client->reload_floor = 0;
    }
}

static void refresh_scan_list(bt_ipc_state_t *ipc) {
    uint8_t buf[BT_SCAN_FILE_BYTES];
    if (!bt_scan_read_file(buf)) return;
    const uint32_t before = ipc->scan_count;
    if (!bt_scan_apply(ipc, buf, sizeof buf)) return;
    bt_manager_keep_stored_in_scan(ipc);
    if (ipc->scan_count != before)
        note("ui: scan devices=%u", ipc->scan_count);
}

static void update_plugin_status(bt_ipc_state_t *ipc, plugin_client_t *client) {
    const uint64_t poll_interval  = 2500000ULL;   /* 250 ms */
    const uint64_t retry_interval = 10000000ULL;  /* 1.0 s */
    const uint64_t toast_duration = 45000000ULL;  /* 4.5 s */
    uint64_t now;
    KeQuerySystemTime(&now);
    if (now >= client->last_poll && now - client->last_poll < poll_interval) return;
    client->last_poll = now;
    refresh_stored_ds4(ipc, client);
    if ((ipc->cmd == BT_IPC_CMD_START_SCAN || ipc->cmd == BT_IPC_CMD_STOP_SCAN) &&
        ipc->cmd_seq != client->sent_sequence)
        client->watch_scan = 12u;
    if (client->watch_scan || ipc->scan_active || client->was_scanning)
        refresh_scan_list(ipc);
    if (ipc->scan_active) {
        ipc->scan_finished_empty = false;
        client->watch_scan = 0;
    } else if (client->was_scanning && ipc->scan_count == 0) {
        ipc->scan_finished_empty = true;
    }
    if (client->watch_scan && !ipc->scan_active) client->watch_scan--;
    client->was_scanning = ipc->scan_active;

    if (client->token) {
        bt_console_reply_t reply;
        if (bt_console_read_reply(&reply) && reply.token == client->token) {
            if (client->log_state != IPC_REPLIED || client->sent_command ||
                client->adapter_stage != reply.adapter_stage)
                note("ipc: reply sequence=%u usb_status=%d adapter_stage=%u phrase=%u player=%u pair=%u",
                     client->sent_sequence, reply.usb_status, reply.adapter_stage,
                     reply.phrase_id, reply.player, reply.pair_active);
            client->log_state = IPC_REPLIED;
            client->adapter_stage = reply.adapter_stage;
            ipc->dongle_ready = reply.usb_status == 0 &&
                                reply.adapter_stage == BT_USB_STAGE_READY &&
                                reply.phrase_id != BT_PHRASE_ADAPTER_FW_MISSING;
            bt_manager_apply_links(ipc, reply.player, reply.link);
            ipc->identify_player = reply.reserved;

            /* Frase de evento do DS4 fica retida por 4.5s no toast. */
            if (reply.phrase_id != client->last_phrase) {
                client->last_phrase = reply.phrase_id;
                if (bt_ui_apply_reply_phrase(ipc, reply.phrase_id)) {
                    snprintf(client->toast_msg, sizeof client->toast_msg, "%s", ipc->status_msg);
                    client->toast_until = now + toast_duration;
                }
                if (reply.phrase_id == BT_PHRASE_DS4_PAIRED_PLAYER1 ||
                    reply.phrase_id == BT_PHRASE_DS4_PAIRED_PLAYER2 ||
                    reply.phrase_id == BT_PHRASE_DS4_PAIRED_PLAYER3 ||
                    reply.phrase_id == BT_PHRASE_DS4_PAIRED_PLAYER4) {
                    client->reload_floor = (uint32_t)bt_manager_paired_count(ipc) + 1u;
                    client->reload_tries = STORED_RELOAD_TRIES;
                }
            }

            if (now < client->toast_until && client->toast_msg[0]) {
                snprintf(ipc->status_msg, sizeof ipc->status_msg, "%s", client->toast_msg);
            } else {
                const char *status = "Plugin ativo. Aguardando o Bluetooth.";
                if (reply.usb_status != 0) status = "Plugin ativo. Registro USB indisponível.";
                else if (ipc->dongle_ready && reply.pair_active)
                    status = "Procurando o DualShock 4. Segure SHARE + PS.";
                else if (ipc->dongle_ready && ipc->scan_active)
                    status = "Busca em andamento.";
                else if (ipc->dongle_ready && ipc->scan_finished_empty)
                    status = "Nenhum dispositivo encontrado.";
                else if (ipc->dongle_ready && ipc->pad_link == BT_LINK_UP) {
                    unsigned up = 0;
                    for (unsigned i = 0; i < 4u; i++)
                        if (ipc->player_link[i] == BT_LINK_UP) up++;
                    status = up >= 2u ? "Dois DualShock 4 conectados (2/2 BT ativos)."
                                      : "DualShock 4 conectado (1/2 BT ativo).";
                } else if (ipc->dongle_ready && ipc->pad_link == BT_LINK_CONNECTING)
                    status = "DualShock 4 conectando.";
                else if (ipc->dongle_ready && bt_manager_has_stored_ds4(ipc))
                    status = "DualShock 4 desconectado.";
                else if (ipc->dongle_ready) status = "Adaptador pronto.";
                else if (reply.phrase_id == BT_PHRASE_ADAPTER_FW_MISSING) {
                    const char *phrase = bt_phase1_phrase_text(reply.phrase_id);
                    if (phrase) status = phrase;
                } else if (reply.adapter_stage == BT_USB_STAGE_ERROR)
                    status = "Falha ao iniciar o Bluetooth. Consulte o log.";
                else if (reply.adapter_stage == BT_USB_STAGE_REMOVING)
                    status = "Bluetooth desconectado. Encerrando USB.";
                else if (reply.adapter_stage != BT_USB_STAGE_WAITING)
                    status = "Bluetooth detectado. Inicialização USB/HCI.";
                else if (reply.phrase_id == BT_PHRASE_ADAPTER_NOT_FOUND ||
                         reply.phrase_id == BT_PHRASE_ADAPTER_WRONG ||
                         reply.phrase_id == BT_PHRASE_ADAPTER_DISCONNECTED) {
                    const char *phrase = bt_phase1_phrase_text(reply.phrase_id);
                    if (phrase) status = phrase;
                }
                snprintf(ipc->status_msg, sizeof(ipc->status_msg), "%s", status);
            }
            ipc->ack_seq = client->sent_sequence;
        } else if (now < client->started || now - client->started >= retry_interval) {
            ipc->dongle_ready = false;
            if (client->log_state != IPC_TIMEOUT || client->sent_command)
                note("ipc: reply timeout sequence=%u", client->sent_sequence);
            client->log_state = IPC_TIMEOUT;
            snprintf(ipc->status_msg, sizeof(ipc->status_msg), "Aguardando resposta do plugin pelo USB.");
        } else {
            return;
        }
        client->token = 0;
    }

    const bool command_pending = ipc->cmd_seq != client->sent_sequence;
    if (!command_pending && now >= client->started && now - client->started < retry_interval) return;
    client->started = now;
    client->sent_sequence = ipc->cmd_seq;
    bt_console_request_t request = {
        .magic = BT_IPC_MAGIC,
        .version = BT_IPC_VERSION,
        .token = now,
        .command = command_pending ? ipc->cmd : BT_IPC_CMD_NONE,
        .slot = command_pending ? ipc->cmd_slot : 0,
    };
    if (command_pending) {
        for (size_t i = 0; i < sizeof request.address; i++) request.address[i] = ipc->cmd_bd_addr[i];
    }
    if (bt_console_write_request(&request)) {
        if (command_pending)
            note("ipc: sent sequence=%u command=%u slot=%u",
                 ipc->cmd_seq, request.command, request.slot);
        client->token = now;
        client->sent_command = command_pending;
    } else {
        ipc->dongle_ready = false;
        if (client->log_state != IPC_WRITE_FAILED || command_pending)
            note("ipc: request write failed on Mass0 (command=%u)", request.command);
        client->log_state = IPC_WRITE_FAILED;
        snprintf(ipc->status_msg, sizeof(ipc->status_msg), "Não foi possível gravar no pendrive do plugin.");
    }
}

/* Thread dedicada para I/O do mailbox em Mass0: mantem a renderizacao a 60 fps. */
static void *ipc_worker_entry(void *ctx) {
    ipc_worker_t *w = (ipc_worker_t *)ctx;
    bt_ipc_state_t worker_ipc;
    plugin_client_t client = { 0 };
    bt_ipc_init(&worker_ipc);

    while (!w->stop) {
        pthread_mutex_lock(&w->lock);
        if (w->has_cmd) {
            worker_ipc.cmd_seq = w->cmd_seq;
            worker_ipc.cmd = w->cmd;
            worker_ipc.cmd_slot = w->cmd_slot;
            memcpy(worker_ipc.cmd_bd_addr, w->cmd_bd_addr, 6u);
            w->has_cmd = false;
        }
        if (w->local_override_paired) {
            memcpy(worker_ipc.paired, w->local_paired, sizeof worker_ipc.paired);
            w->local_override_paired = false;
        }
        if (w->local_toast_pending) {
            uint64_t now;
            KeQuerySystemTime(&now);
            snprintf(client.toast_msg, sizeof client.toast_msg, "%s", w->local_toast);
            client.toast_until = now + 45000000ULL;
            snprintf(worker_ipc.status_msg, sizeof worker_ipc.status_msg, "%s", client.toast_msg);
            w->local_toast_pending = false;
        }
        pthread_mutex_unlock(&w->lock);

        update_plugin_status(&worker_ipc, &client);

        pthread_mutex_lock(&w->lock);
        w->snapshot = worker_ipc;
        w->stored_loaded = client.stored_loaded;
        w->snapshot_ready = true;
        pthread_mutex_unlock(&w->lock);

        xsdk_thread_sleep(40);
    }
    return NULL;
}
#endif

enum {
    BUFFERS = 2,
    SWAP_INTERVAL = 1,
    BEGIN_TIMEOUT_MS = 5000,
    STICK_DEADZONE = 14000,
    DIR_REPEAT_FIRST_FRAMES = 20, /* ~330 ms a 60 fps */
    DIR_REPEAT_NEXT_FRAMES  = 8   /* ~133 ms a 60 fps */
};

static uint16_t stick_to_dpad(int16_t lx, int16_t ly) {
    uint16_t dir = 0;
    if (lx < -STICK_DEADZONE) dir |= INPUT_GAMEPAD_DPAD_LEFT;
    else if (lx > STICK_DEADZONE) dir |= INPUT_GAMEPAD_DPAD_RIGHT;
    if (ly > STICK_DEADZONE) dir |= INPUT_GAMEPAD_DPAD_UP;
    else if (ly < -STICK_DEADZONE) dir |= INPUT_GAMEPAD_DPAD_DOWN;
    return dir;
}

int main(void) {
    note("BTManager: startup");
    note("trace: bt360_trace.log na raiz do HD interno e do Mass0; copia em bt360_diag.log");
    NTSTATUS graphics_status = xsdk_gpu_open();
    if (!NT_SUCCESS(graphics_status)) {
        note("video: GPU initialization failed status=0x%08X", (unsigned)graphics_status);
        return 1;
    }

    uint32_t width = 0, height = 0;
#ifdef BTMANAGER_XENIA
    width = 1280;
    height = 720;
    volatile uint32_t *frame = xsdk_gpu_alloc(xsdk_gpu_resolve_bytes(width, height));
    if (!frame) {
        note("video: front buffer allocation failed");
        xsdk_gpu_close();
        return 1;
    }
#else
    graphics_status = xsdk_present_open(XSDK_DISPLAY_720P, BUFFERS, SWAP_INTERVAL, &width, &height);
    if (!NT_SUCCESS(graphics_status)) {
        note("video: presentation initialization failed status=0x%08X", (unsigned)graphics_status);
        xsdk_gpu_close();
        return 1;
    }
#endif
    if (!dash_open()) {
        note("video: dashboard renderer initialization failed");
#ifdef BTMANAGER_XENIA
        if (NT_SUCCESS(xsdk_gpu_close())) xsdk_gpu_free(frame);
#else
        xsdk_present_close();
        xsdk_gpu_close();
#endif
        return 1;
    }
    note("video: dashboard ready at %ux%u", width, height);

    bt_ipc_state_t local_ipc;
    bt_ipc_init(&local_ipc);
#ifdef BTMANAGER_XENIA
    snprintf(local_ipc.status_msg, sizeof(local_ipc.status_msg),
             "Prévia no Xenia. Bluetooth indisponível.");
#else
    ipc_worker_t worker;
    memset(&worker, 0, sizeof worker);
    pthread_mutex_init(&worker.lock, NULL);
    pthread_t worker_thread;
    const bool worker_started = pthread_create(&worker_thread, NULL, ipc_worker_entry, &worker) == 0;
    if (!worker_started) {
        note("ipc: failed to create worker thread");
    }
    bool initial_pane_chosen = false;
    bool user_interacted = false;
#endif
    bt_ui_model_t ui;
    bt_ui_model_init(&ui);
    bt_remap_wizard_t wiz = { 0 };

    uint16_t prev_btns = 0;
    uint16_t held_dir = 0;
    unsigned dir_repeat_frames = 0;
    bool input_failed = false;
    while (!ui.exit_requested) {
        uint16_t btns = 0;
        bool any_pad = false;
        NTSTATUS input_status = (NTSTATUS)0xC0000001u;
        /* O DS4 fica no jogador 2 quando o oficial ocupa o 1. Os dois navegam (D-pad + analogico). */
        for (uint32_t user = 0; user < 4u; user++) {
            INPUT_STATE input;
            memset(&input, 0, sizeof input);
            input_status = XamInputGetState(user, 0, &input);
            if (input_status != ERROR_SUCCESS) continue;
            any_pad = true;
            btns = (uint16_t)(btns | input.gamepad.buttons |
                              stick_to_dpad(input.gamepad.thumb_lx, input.gamepad.thumb_ly));
        }
        if (any_pad) {
            if (input_failed) {
                note("ui: XamInputGetState voltou");
                input_failed = false;
            }
        } else if (!input_failed) {
            note("ui: XamInputGetState falhou status=0x%08X", (unsigned)input_status);
            input_failed = true;
        }
        uint16_t newly_pressed = (uint16_t)(btns & (uint16_t)(~prev_btns));
        prev_btns = btns;

        /* Repeticao suave ao segurar direcional ou analogico esquerdo. */
        const uint16_t cur_dir = (uint16_t)(btns & (INPUT_GAMEPAD_DPAD_UP | INPUT_GAMEPAD_DPAD_DOWN |
                                                    INPUT_GAMEPAD_DPAD_LEFT | INPUT_GAMEPAD_DPAD_RIGHT));
        if (cur_dir != 0 && cur_dir == held_dir) {
            if (++dir_repeat_frames >= DIR_REPEAT_FIRST_FRAMES) {
                newly_pressed = (uint16_t)(newly_pressed | cur_dir);
                dir_repeat_frames = DIR_REPEAT_FIRST_FRAMES - DIR_REPEAT_NEXT_FRAMES;
            }
        } else {
            held_dir = cur_dir;
            dir_repeat_frames = 0;
        }

        const uint32_t seq_before = local_ipc.cmd_seq;
        char msg_before[64];
        memcpy(msg_before, local_ipc.status_msg, sizeof msg_before);

        if (wiz.active) {
            if (newly_pressed & INPUT_GAMEPAD_BACK) {
                wiz.active = false;
            } else {
                bt_remap_wizard_feed_raw(&wiz, local_ipc.slot_raw_mask[wiz.slot_idx],
                                         &local_ipc.slots[wiz.slot_idx].map);
            }
        } else {
            if ((newly_pressed & INPUT_GAMEPAD_START) &&
                ui.pane == BT_UI_PANE_SLOTS &&
                ui.cursor >= 0 && (uint32_t)ui.cursor < BT_MAX_ACTIVE_SLOTS &&
                local_ipc.slots[ui.cursor].connected) {
                bt_remap_wizard_start(&wiz, (uint32_t)ui.cursor,
                                      local_ipc.slot_raw_mask[ui.cursor]);
            } else {
                bt_ui_handle_input(&ui, &local_ipc, newly_pressed);
            }
        }
        if (newly_pressed) {
#ifndef BTMANAGER_XENIA
            user_interacted = true;
#endif
            note("ui: btn=%04X pane=%u cursor=%d foco=%u exit=%u", newly_pressed,
                 (unsigned)ui.pane, ui.cursor, (unsigned)ui.content_focused,
                 (unsigned)ui.exit_requested);
        }
        if (ui.exit_requested) break;

#ifndef BTMANAGER_XENIA
        pthread_mutex_lock(&worker.lock);
        if (local_ipc.cmd_seq != seq_before) {
            worker.cmd_seq = local_ipc.cmd_seq;
            worker.cmd = local_ipc.cmd;
            worker.cmd_slot = local_ipc.cmd_slot;
            memcpy(worker.cmd_bd_addr, local_ipc.cmd_bd_addr, 6u);
            worker.has_cmd = true;
            if (local_ipc.cmd == BT_IPC_CMD_FORGET_PAIRED ||
                local_ipc.cmd == BT_IPC_CMD_SET_PLAYER) {
                memcpy(worker.local_paired, local_ipc.paired, sizeof worker.local_paired);
                worker.local_override_paired = true;
            }
        } else if (memcmp(msg_before, local_ipc.status_msg, sizeof msg_before) != 0) {
            /* Aviso local da UI (ex: limite de 2/2 BT ativos) entra no toast por 4.5s. */
            snprintf(worker.local_toast, sizeof worker.local_toast, "%s", local_ipc.status_msg);
            worker.local_toast_pending = true;
        }
        if (worker.snapshot_ready) {
            const uint32_t ui_seq = local_ipc.cmd_seq;
            const bool keep_local_paired = worker.local_override_paired;
            bt_paired_device_t saved_paired[BT_MAX_PAIRED_DEVICES];
            if (keep_local_paired) memcpy(saved_paired, local_ipc.paired, sizeof saved_paired);
            local_ipc = worker.snapshot;
            local_ipc.cmd_seq = ui_seq;
            if (keep_local_paired) memcpy(local_ipc.paired, saved_paired, sizeof saved_paired);
            if (!initial_pane_chosen && worker.stored_loaded) {
                initial_pane_chosen = true;
                if (!user_interacted) bt_ui_model_select_initial_pane(&ui, &local_ipc);
            }
        }
        pthread_mutex_unlock(&worker.lock);
#else
        (void)seq_before;
        (void)msg_before;
#endif
        uint32_t address = 0;
#ifdef BTMANAGER_XENIA
        address = xsdk_gpu_address(frame);
#else
        graphics_status = xsdk_present_begin(&address, BEGIN_TIMEOUT_MS);
        if (!NT_SUCCESS(graphics_status)) {
            note("video: frame acquisition failed status=0x%08X", (unsigned)graphics_status);
            break;
        }
#endif

        if (!dash_begin(width, height)) {
            note("video: dashboard GPU fence timed out");
            break;
        }
        dashboard_draw(&local_ipc, &ui, &wiz);
        dash_end();
        xsdk_gpu_resolve(address, width, height, 0xFF14171B);
#ifdef BTMANAGER_XENIA
        GpuSurface shown;
        xsdk_gpu_swap(frame, width, height, &shown);
        graphics_status = xsdk_gpu_wait(xsdk_gpu_fence(), BEGIN_TIMEOUT_MS);
        if (!NT_SUCCESS(graphics_status)) {
            note("video: swap fence failed status=0x%08X", (unsigned)graphics_status);
            break;
        }
#else
        xsdk_present_end();
#endif
    }

    note("BTManager: stopping (user_exit=%u)", (unsigned)ui.exit_requested);
#ifndef BTMANAGER_XENIA
    worker.stop = true;
    if (worker_started) {
        pthread_join(worker_thread, NULL);
    }
    pthread_mutex_destroy(&worker.lock);
#endif
    /* O DashLaunch le os botoes na hora em que o titulo devolve o painel.
     * Sair com B ainda apertado faz ele abrir a Aurora de novo. */
    if (ui.exit_requested) {
        unsigned spins = 0;
        for (; spins < 150u; spins++) {
            bool held = false;
            for (uint32_t user = 0; user < 4u; user++) {
                INPUT_STATE state;
                memset(&state, 0, sizeof state);
                if (XamInputGetState(user, 0, &state) == ERROR_SUCCESS &&
                    (state.gamepad.buttons & INPUT_GAMEPAD_B))
                    held = true;
            }
            if (!held) break;
            xsdk_thread_sleep(20);
        }
        xsdk_thread_sleep(300);
    }
    dash_close();
#ifndef BTMANAGER_XENIA
    xsdk_present_close();
    xsdk_gpu_close();
#else
    if (!NT_SUCCESS(xsdk_gpu_close())) return 1;
    xsdk_gpu_free(frame);
#endif
    return 0;
}
