/*
 * BTManager.xex — Xbox 360 Bluetooth Connection & Pairing UI
 * Inspired by the original Xbox 360 NXE / Guide interface.
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
    uint32_t sent_sequence;
    uint32_t adapter_stage;
    bool sent_command;
    bool was_scanning;
    bool stored_loaded;   /* bt_devices.ini lido (ou ausente) ao menos uma vez */
    uint32_t reload_tries; /* releituras pendentes depois de um pareamento concluido */
    uint32_t last_phrase;  /* ultimo phrase_id visto no reply */
    enum { IPC_INITIAL, IPC_REPLIED, IPC_TIMEOUT, IPC_WRITE_FAILED } log_state;
} plugin_client_t;

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

/* Atualiza ipc->paired a partir do arquivo. Retorna true quando o volume respondeu. */
static bool load_stored_ds4(bt_ipc_state_t *ipc) {
    char text[1024];
    size_t size = 0;
    if (!read_devices_ini(text, sizeof text - 1u, &size)) return false;
    text[size] = 0;
    for (size_t i = 0; i < BT_MAX_PAIRED_DEVICES; i++) ipc->paired[i].valid = false;
    bt_manager_import_ini(ipc, text, size);
    return true;
}

/* Le o DS4 gravado no inicio (ate o volume responder) e apos um pareamento concluido. */
static void refresh_stored_ds4(bt_ipc_state_t *ipc, plugin_client_t *client) {
    if (client->stored_loaded && client->reload_tries == 0) return;
    const bool had_ds4 = bt_manager_has_stored_ds4(ipc);
    if (client->stored_loaded) client->reload_tries--;
    if (!load_stored_ds4(ipc)) return;
    const bool has_ds4 = bt_manager_has_stored_ds4(ipc);
    if (!client->stored_loaded || has_ds4 != had_ds4)
        note("ui: DualShock 4 gravado=%u", (unsigned)has_ds4);
    client->stored_loaded = true;
    if (has_ds4) client->reload_tries = 0;
}

static void refresh_scan_list(bt_ipc_state_t *ipc) {
    uint8_t buf[BT_SCAN_FILE_BYTES];
    if (!bt_scan_read_file(buf)) return;
    const uint32_t before = ipc->scan_count;
    if (!bt_scan_apply(ipc, buf, sizeof buf)) return;
    if (ipc->scan_count != before)
        note("ui: scan devices=%u", ipc->scan_count);
}

static void update_plugin_status(bt_ipc_state_t *ipc, plugin_client_t *client) {
    const uint64_t poll_interval = 2500000; /* 250 ms. Leitura no HD USB divide o barramento com o dongle. */
    const uint64_t retry_interval = 10000000; /* One second in 100 ns units. */
    uint64_t now;
    KeQuerySystemTime(&now);
    if (now >= client->last_poll && now - client->last_poll < poll_interval) return;
    client->last_poll = now;
    refresh_stored_ds4(ipc, client);
    if (ipc->scan_active || client->was_scanning) refresh_scan_list(ipc);
    if (ipc->scan_active) ipc->scan_finished_empty = false;
    else if (client->was_scanning && ipc->scan_count == 0) ipc->scan_finished_empty = true;
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
            ipc->dongle_ready = reply.usb_status == 0 && reply.adapter_stage == BT_USB_STAGE_READY;
            ipc->pad_link = reply.link;
            ipc->live_player = (reply.link == BT_LINK_UP || reply.link == BT_LINK_CONNECTING) &&
                               reply.player >= 1u && reply.player <= 4u
                                   ? reply.player : 0u;
            ipc->identify_player = reply.reserved;
            const char *status = "Plugin ativo. Aguardando o UGREEN.";
            if (reply.usb_status != 0) status = "Plugin ativo. Registro USB indisponível.";
            else if (ipc->dongle_ready && reply.pair_active)
                status = "Procurando o DualShock 4. Segure SHARE + PS.";
            else if (ipc->dongle_ready && ipc->scan_active)
                status = "Busca em andamento.";
            else if (ipc->dongle_ready && ipc->scan_finished_empty)
                status = "Nenhum dispositivo encontrado.";
            else if (ipc->dongle_ready && reply.link == BT_LINK_UP)
                status = "DualShock 4 conectado.";
            else if (ipc->dongle_ready && reply.link == BT_LINK_CONNECTING)
                status = "DualShock 4 conectando.";
            else if (ipc->dongle_ready && bt_manager_has_stored_ds4(ipc))
                status = "DualShock 4 desconectado.";
            else if (ipc->dongle_ready) status = "Adaptador pronto.";
            else if (reply.adapter_stage == BT_USB_STAGE_ERROR) status = "Falha ao iniciar o UGREEN. Consulte o log.";
            else if (reply.adapter_stage == BT_USB_STAGE_REMOVING) status = "UGREEN desconectado. Encerrando USB.";
            else if (reply.adapter_stage != BT_USB_STAGE_WAITING) status = "UGREEN detectado. Inicialização USB/HCI.";
            else if (reply.phrase_id == BT_PHRASE_ADAPTER_NOT_FOUND ||
                     reply.phrase_id == BT_PHRASE_ADAPTER_WRONG ||
                     reply.phrase_id == BT_PHRASE_ADAPTER_DISCONNECTED) {
                const char *phrase = bt_phase1_phrase_text(reply.phrase_id);
                if (phrase) status = phrase;
            }
            snprintf(ipc->status_msg, sizeof(ipc->status_msg), "%s", status);
            /* Mesma frase da notificacao do plugin: texto vem so de bt_phase1_phrase_text. */
            bt_ui_apply_reply_phrase(ipc, reply.phrase_id);
            if (reply.phrase_id != client->last_phrase) {
                client->last_phrase = reply.phrase_id;
                if ((reply.phrase_id == BT_PHRASE_DS4_PAIRED_PLAYER1 ||
                     reply.phrase_id == BT_PHRASE_DS4_PAIRED_PLAYER2 ||
                     reply.phrase_id == BT_PHRASE_DS4_ALREADY_PAIRED) &&
                    !bt_manager_has_stored_ds4(ipc))
                    client->reload_tries = STORED_RELOAD_TRIES;
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
#endif

enum {
    BUFFERS = 2,
    SWAP_INTERVAL = 1,
    BEGIN_TIMEOUT_MS = 5000
};

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
    snprintf(local_ipc.status_msg, sizeof(local_ipc.status_msg), "Prévia no Xenia. Bluetooth indisponível.");
#else
    plugin_client_t plugin = { 0 };
#endif
    bt_ui_model_t ui;
    bt_ui_model_init(&ui);
    bt_remap_wizard_t wiz = { 0 };

    uint16_t prev_btns = 0;
    bool input_failed = false;
    while (!ui.exit_requested) {
        INPUT_STATE input = { 0 };
        uint16_t btns = 0;
        const NTSTATUS input_status = XamInputGetState(0, 0, &input);
        if (input_status == ERROR_SUCCESS) {
            btns = input.gamepad.buttons;
            if (input_failed) {
                note("ui: XamInputGetState do jogador 1 voltou");
                input_failed = false;
            }
        } else if (!input_failed) {
            note("ui: XamInputGetState do jogador 1 falhou status=0x%08X", (unsigned)input_status);
            input_failed = true;
        }
        uint16_t newly_pressed = (uint16_t)(btns & (uint16_t)(~prev_btns));
        prev_btns = btns;

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
        if (newly_pressed)
            note("ui: btn=%04X pane=%u cursor=%d foco=%u exit=%u", newly_pressed,
                 (unsigned)ui.pane, ui.cursor, (unsigned)ui.content_focused,
                 (unsigned)ui.exit_requested);
        if (ui.exit_requested) break;

#ifndef BTMANAGER_XENIA
        update_plugin_status(&local_ipc, &plugin);
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
        xsdk_gpu_resolve(address, width, height, 0xFF202A21);
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
    /* O DashLaunch lê os botões na hora em que o título devolve o painel.
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
