#include <xsdk/plugin.h>
#include "../hal/xbox360_usbd.h"
#include "../hci/hci_core.h"
#include "../transport/bt_usb_transport.h"
#include "../manager/console_mailbox.h"
#include "../manager/bt_phase1.h"
#include "../manager/bt_pair_session.h"
#include "../manager/bt_trace.h"
#include "rtl_boot.h"
#include "../hid/x360_pad_inject.h"
#include "bt_hid_link.h"
#include "ppc_detour.h"

/* As linhas do plugin vão para o HD e para o Mass0, não só para o log do xsdk. */
#define xsdk_plugin_log bt_trace

/* Minimal DPC-safe memory primitives (plugin links libxsdk_plugin.a without newlib) */
void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

void *memset(void *dst, int c, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (size_t i = 0; i < n; i++) d[i] = (uint8_t)c;
    return dst;
}

int memcmp(const void *lhs, const void *rhs, size_t n) {
    const uint8_t *a = (const uint8_t *)lhs;
    const uint8_t *b = (const uint8_t *)rhs;
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (int)a[i] - (int)b[i];
    }
    return 0;
}

void *memmove(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d < s) {
        for (size_t i = 0; i < n; i++) d[i] = s[i];
    } else if (d > s) {
        for (size_t i = n; i > 0; i--) d[i - 1] = s[i - 1];
    }
    return dst;
}

static void sleep_ms(uint32_t ms) {
    int64_t interval = -(int64_t)ms * 10000LL;
    KeDelayExecutionThread(0, 0, &interval);
}

/* xam.xex export 656; call shape copied from hiddriver360 main.cpp:42.
 * Tipo 0 é o orbe do Xbox com o texto livre. Tipo 3 é o aviso de carta.
 * Tipo 0x2F é a faixa da foto (tempo de jogo), mas o sistema troca o texto
 * pela frase de horas, então o aviso do controle não usa esse tipo.
 * A área 1 aparece no painel e a 2 dentro do jogo. */
#define XAM_ORD_XNOTIFY_QUEUE_UI 656u
#define XNOTIFY_UI_TYPE          3u
#define XNOTIFY_UI_XBOX_LOGO     0u
#define XNOTIFY_AREA_BOTH        3ull
#define XNOTIFY_USER_ANY         0x000000FFu
#define ADAPTER_NOT_FOUND_TICKS  150u /* 15 s of 100 ms polls without the UGREEN */
#define AURORA_SETTLE_TICKS      150u /* 15 s with Aurora as the running title */

typedef void (*xnotify_queue_ui_fn)(uint32_t type, uint32_t user_index, uint64_t areas,
                                    const uint16_t *text, void *context);

static xnotify_queue_ui_fn xnotify_queue_ui;
static bool xnotify_resolved;

/* Phrase tables are UTF-8; XNotifyQueueUI wants big-endian UTF-16 (native here). */
static size_t utf8_to_utf16(const char *src, uint16_t *dst, size_t capacity) {
    size_t n = 0;
    const uint8_t *p = (const uint8_t *)src;
    while (*p && n + 1 < capacity) {
        uint32_t c = *p++;
        if (c >= 0xC0u && c < 0xE0u && (*p & 0xC0u) == 0x80u) {
            c = ((c & 0x1Fu) << 6) | (*p++ & 0x3Fu);
        } else if (c >= 0x80u) {
            c = '?';
        }
        dst[n++] = (uint16_t)c;
    }
    dst[n] = 0;
    return n;
}

/* O DashLaunch chama o plugin numa thread de sistema, e o XAM descarta
 * XNotifyQueueUI vindo de lá. O aviso sai numa thread comum. */
static uint16_t g_note_wide[96];
static uint32_t g_note_type;
static volatile uint32_t g_note_state;
static bool g_note_thread;

static uint32_t notify_worker(void *unused) {
    (void)unused;
    for (;;) {
        if (g_note_state == 1u && xnotify_queue_ui) {
            uint16_t local[96];
            const uint32_t type = g_note_type;
            memcpy(local, g_note_wide, sizeof local);
            g_note_state = 0u;
            xnotify_queue_ui(type, XNOTIFY_USER_ANY, XNOTIFY_AREA_BOTH, local, NULL);
        }
        sleep_ms(20);
    }
    return 0;
}

static void notify_thread_start(void) {
    HANDLE thread = 0;
    uint32_t id = 0;
    if (g_note_thread) return;
    const NTSTATUS status = ExCreateThread(&thread, 0, &id, NULL, (void *)notify_worker, NULL, 0);
    g_note_thread = NT_SUCCESS(status);
    xsdk_plugin_log("notify: thread status=0x%08X", (unsigned)status);
}

static void notify_text(const char *text, uint32_t type) {
    unsigned wait;
    if (!text) return;
    if (!xnotify_resolved) {
        HANDLE xam = 0;
        xnotify_resolved = true;
        if (XexGetModuleHandle("xam.xex", &xam) == 0 && xam) {
            void *fn = NULL;
            if (NT_SUCCESS(XexGetProcedureAddress(xam, XAM_ORD_XNOTIFY_QUEUE_UI, &fn)))
                xnotify_queue_ui = (xnotify_queue_ui_fn)fn;
        }
    }
    if (!xnotify_queue_ui) {
        xsdk_plugin_log("notify=absent");
        return;
    }
    notify_thread_start();
    for (wait = 0; g_note_state == 1u && wait < 20u; wait++) sleep_ms(10);
    utf8_to_utf16(text, g_note_wide, sizeof g_note_wide / sizeof g_note_wide[0]);
    g_note_type = type;
    if (g_note_thread) {
        g_note_state = 1u;
    } else {
        xnotify_queue_ui(type, XNOTIFY_USER_ANY, XNOTIFY_AREA_BOTH, g_note_wide, NULL);
    }
    xsdk_plugin_log("notify: queued type=%u", type);
}

static void notify_phrase(uint32_t phrase_id) {
    notify_text(bt_phase1_phrase_text(phrase_id), XNOTIFY_UI_TYPE);
}

/* ---- Pairing session wiring (tarefa 06) ------------------------------------ */

#define BT_DEVICES_INI        "\\Device\\Mass0\\bt_devices.ini"
#define INI_CAPACITY          1024u
#define INI_WAIT_MS           10000u /* espera pelo volume antes de assumir "sem DS4 gravado" */
#define LOOP_MS               10u
#define MAILBOX_EVERY_TICKS   10u    /* 100 ms, como antes */
/* xam.xex export 401 (XamInputGetState), from research/repos/xsdk/imports/imports.txt. */
#define XAM_ORD_INPUT_GET_STATE 401u

typedef uint32_t (*xam_input_get_state_fn)(uint32_t user_index, uint32_t flags, INPUT_STATE *state);

static bt_ipc_state_t   g_ipc;
static bt_pair_session_t g_session;

/* Faixa com o orbe do Xbox quando o DualShock entra ou sai. */
static void notify_link_edge(void) {
    const bool up = g_session.state == BT_PAIR_CONNECTED && g_session.established;
    static bool was_up;
    char text[64];
    if (up == was_up) return;
    was_up = up;
    if (up && g_session.player >= 1u && g_session.player <= 4u) {
        static const char prefix[] = "DualShock 4\nConectado no Jogador ";
        memcpy(text, prefix, sizeof prefix - 1u);
        text[sizeof prefix - 1u] = (char)('0' + g_session.player);
        text[sizeof prefix] = '.';
        text[sizeof prefix + 1u] = '\0';
    } else if (up) {
        memcpy(text, "DualShock 4\nConectado.", sizeof "DualShock 4\nConectado.");
    } else {
        memcpy(text, "DualShock 4\nDesconectado.", sizeof "DualShock 4\nDesconectado.");
    }
    notify_text(text, XNOTIFY_UI_XBOX_LOGO);
}
static char             g_ini[INI_CAPACITY];
static uint8_t          g_event[HCI_MAX_EVT_SIZE];
static bool             g_ini_loaded;
static NTSTATUS         g_ini_status; /* last NtCreateFile/NtReadFile status of ini_transfer */
static uint32_t         g_events_dropped_logged;
static xam_input_get_state_fn xam_input_get_state;
static bool             xam_input_resolved;

static bool ini_transfer(const char *path, uint16_t path_size, void *input_out, size_t *size,
                         bool write) {
    const ANSI_STRING name = { path_size - 1, path_size, path };
    const OBJECT_ATTRIBUTES attributes = { .name = &name };
    HANDLE file;
    IO_STATUS_BLOCK io = { 0 };
    NTSTATUS status = NtCreateFile(&file, SYNCHRONIZE | (write ? GENERIC_WRITE : GENERIC_READ),
                                   &attributes, &io, NULL, FILE_ATTRIBUTE_NORMAL, 0,
                                   write ? FILE_OVERWRITE_IF : FILE_OPEN,
                                   FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE);
    g_ini_status = status;
    if (!NT_SUCCESS(status)) return false;
    const uint64_t offset = 0;
    if (write) {
        status = NtWriteFile(file, 0, NULL, NULL, &io, input_out, (uint32_t)*size, &offset);
        if (NT_SUCCESS(status) && io.information != *size) status = STATUS_UNSUCCESSFUL;
    } else {
        status = NtReadFile(file, 0, NULL, NULL, &io, input_out, (uint32_t)*size, NULL);
        *size = NT_SUCCESS(status) ? (size_t)io.information : 0u;
    }
    g_ini_status = status;
    const bool ok = NT_SUCCESS(status);
    NtClose(file);
    return ok;
}

/* Loads Mass0:\bt_devices.ini once. Returns true once the volume answered. */
static bool ini_try_load(void) {
    if (g_ini_loaded) return true;
    size_t size = sizeof g_ini - 1u;
    if (!ini_transfer(BT_DEVICES_INI, sizeof BT_DEVICES_INI, g_ini, &size, false)) {
        /* Volume answered but there is no file (or it is empty): first run, no DS4 stored. */
        if (g_ini_status != STATUS_OBJECT_NAME_NOT_FOUND && g_ini_status != STATUS_END_OF_FILE)
            return false;
        g_ini_loaded = true;
        xsdk_plugin_log("ini: no bt_devices.ini yet (status=0x%08X)", (unsigned)g_ini_status);
        return true;
    }
    g_ini[size] = '\0';
    const int loaded = bt_manager_import_ini(&g_ipc, g_ini, size);
    g_ini_loaded = true;
    xsdk_plugin_log("ini: loaded %d device(s), %u bytes", loaded, (unsigned)size);
    return true;
}

/* Tries the load every 100 ms until the volume answers. */
static void ini_poll(void) {
    static uint32_t ticks;
    if (g_ini_loaded) return;
    if (ticks++ % (100u / LOOP_MS) == 0) ini_try_load();
}

static void pair_persist(void *ctx, const bt_ipc_state_t *ipc) {
    (void)ctx;
    size_t size = bt_manager_export_ini(ipc, g_ini, sizeof g_ini);
    if (ini_transfer(BT_DEVICES_INI, sizeof BT_DEVICES_INI, g_ini, &size, true)) {
        xsdk_plugin_log("ini: saved %u bytes", (unsigned)size);
        g_ini_loaded = true;
    } else {
        xsdk_plugin_log("ini: failed to write bt_devices.ini on Mass0");
    }
}

static bt_usb_transport_t *pair_transport(void) {
    return bt_usb_transport_active();
}

static int pair_send_cmd(void *ctx, uint16_t opcode, const uint8_t *params, uint8_t len) {
    (void)ctx;
    bt_usb_transport_t *tr = pair_transport();
    return tr ? bt_usb_transport_send_command(tr, opcode, params, len) : -1;
}

static int pair_start_inquiry(void *ctx, uint8_t units) {
    (void)ctx;
    bt_usb_transport_t *tr = pair_transport();
    const int rc = tr ? bt_usb_transport_start_inquiry(tr, units) : -1;
    xsdk_plugin_log("scan: start rc=%d", rc);
    return rc;
}

static int pair_stop_inquiry(void *ctx) {
    (void)ctx;
    bt_usb_transport_t *tr = pair_transport();
    return tr ? bt_usb_transport_stop_inquiry(tr) : -1;
}

/* Reads XamInputGetState only to learn whether user 0 already has a controller. */
static bool pair_official_on_player1(void *ctx) {
    (void)ctx;
    if (!xam_input_resolved) {
        HANDLE xam = 0;
        xam_input_resolved = true;
        if (XexGetModuleHandle("xam.xex", &xam) == 0 && xam) {
            void *fn = NULL;
            if (NT_SUCCESS(XexGetProcedureAddress(xam, XAM_ORD_INPUT_GET_STATE, &fn)))
                xam_input_get_state = (xam_input_get_state_fn)fn;
        }
        if (!xam_input_get_state) xsdk_plugin_log("pair: XamInputGetState unresolved; assuming no official");
    }
    if (!xam_input_get_state) return false;
    INPUT_STATE state;
    memset(&state, 0, sizeof state);
    return xam_input_get_state(0, 0, &state) == ERROR_SUCCESS;
}

static void pair_notify(void *ctx, uint32_t phrase_id) {
    (void)ctx;
    xsdk_plugin_log("pair: phrase=%u player=%u", phrase_id, g_session.player);
    /* A queda do link usa o banner de conquista em notify_link_edge. */
    if (phrase_id == BT_PHRASE_DS4_DISCONNECTED) return;
    notify_phrase(phrase_id);
}

static void pair_log(void *ctx, const char *what, uint32_t value) {
    (void)ctx;
    xsdk_plugin_log("pair: %s %u", what, value);
}

static uint64_t now_ms(void) {
    uint64_t t = 0;
    KeQuerySystemTime(&t);
    return t / 10000u;
}

/* ---- XamInput hooks (tarefa 08) ---------------------------------------------------------
 * Same three points as research/repos/hiddriver360/hiddriver/main.cpp: the pointers come from
 * XexGetProcedureAddress (main.cpp:1494, 1496, 1497) and the detour goes on those pointers
 * (main.cpp:1585-1587). No fixed address is used anywhere; if an ordinal does not resolve the hook
 * is logged and not installed.
 */
#define KRNL_ORD_XINPUTD_READ_STATE      486u /* xboxkrnl.exe, main.cpp:1494 */
#define XAM_ORD_INPUT_GET_CAPABILITIES_EX 685u /* xam.xex,      main.cpp:1496 */
#define XAM_ORD_INPUT_SET_STATE           402u /* xam.xex,      main.cpp:1497 */
#define XAM_ORD_INPUT_GET_KEYSTROKE       403u /* xam 0x193: tecla do jogo e do painel */
#define XAM_ORD_INPUT_GET_KEYSTROKE_EX    408u /* xam 0x198 */
#define XAM_ORD_INPUT_GET_KEYSTROKE_HUD   409u /* xam 0x199: tecla do painel */
#define XAM_ORD_INPUT_GET_KEYSTROKE_HUD_EX 413u /* xam 0x19D */
#define XAM_ORD_SYSUI_BY_XENON            483u /* xam 0x1E3: painel aberto pelo botão central */
#define XAM_ORD_SEND_XENON_BUTTON        1286u /* xkelib _xamext.h: abre o painel do Xbox */

/* XInputdReadState(deviceContext, packetNumber, gamepad, unk), main.cpp:1365. */
typedef uint32_t (*xinputd_read_state_fn)(uint32_t context, uint32_t *packet, INPUT_GAMEPAD *gamepad,
                                          uint32_t *unk);
/* XamInputGetCapabilitiesEx(unk, user, flags, caps), main.cpp:1330. */
typedef uint32_t (*xam_get_capabilities_ex_fn)(uint32_t unk, uint32_t user, uint32_t flags,
                                               x360_pad_caps_t *caps);
/* XamInputSetState(user, flags, vibration): kernel.h; main.cpp:1307 declares it with extra arguments. */
typedef uint32_t (*xam_set_state_fn)(uint32_t user, uint32_t flags, const INPUT_VIBRATION *vibration);

_Static_assert(sizeof(INPUT_GAMEPAD) == sizeof(x360_gamepad_state_t), "gamepad layout");

enum { HOOK_READ_STATE, HOOK_CAPABILITIES, HOOK_SET_STATE,
       HOOK_KEYSTROKE, HOOK_KEYSTROKE_EX, HOOK_KEYSTROKE_HUD, HOOK_KEYSTROKE_HUD_EX,
       HOOK_COUNT };

static x360_pad_inject_t g_pad;
static x360_gamepad_state_t g_pad_report;   /* last HID report; empty until a transport delivers one */
static bool             g_pad_ps;           /* PS button of that report */
static bool             g_hid_up;
static uint8_t          g_pad_player;       /* last player (1/2) the pairing session decided */
static bool             g_pad_logged_no_player;
/* Device context of the DS4 for XInputdReadState, which has no user index. Set only after
 * XamUserBindDeviceCallback accepted the DS4 on the player the session decided; 0 otherwise. */
static volatile uint32_t g_pad_device_context;

/*
 * XamUserBindDeviceCallback, retail 17559 only: hiddriver360 main.cpp:1530 (0x816D9060), signature
 * main.cpp:239; call "up" main.cpp:498-501, "down" main.cpp:1204. bool is a uint32_t (0/1, as in the
 * register); the unsigned char userIndex out-pointer is a uint8_t. Never called on another kernel.
 */
#define XAM_USER_BIND_DEVICE_CALLBACK_17559 0x816D9060u
#define KERNEL_BUILD_17559                  17559u
#define DS4_BIND_INDEX                      0u /* one DS4 only */

typedef int (*xam_user_bind_device_callback_fn)(uint32_t controller_id, uint32_t context,
                                                uint8_t category, uint32_t disconnect,
                                                uint8_t *user_index);

static xam_user_bind_device_callback_fn g_bind_device;
static bool g_layout_17559;           /* kernel 17559 and the retail layout guard of the USB HAL passed */
static bool g_pad_bound;              /* the DS4 context is registered in XAM */
static uint8_t g_pad_bound_player;
static uint8_t g_pad_user = 0xFFu;
static bool g_guide_was_down;
static uint32_t g_guide_last_ms;
typedef void (*xenon_press_fn)(uint32_t user_index);
static xenon_press_fn g_xenon_press;
typedef uint32_t (*sysui_xenon_fn)(void);
static sysui_xenon_fn g_sysui_by_xenon;
static bool g_pad_bind_refused;       /* this authentication already tried and was refused */
static bool g_pad_logged_layout;
static bool g_pad_logged_empty;

static ppc_detour_t g_detours[HOOK_COUNT];
static xinputd_read_state_fn      g_orig_read_state;
static xam_get_capabilities_ex_fn g_orig_capabilities;
static xam_set_state_fn           g_orig_set_state;
typedef uint32_t (*xam_keystroke_fn)(uint32_t user, uint32_t flags, x360_keystroke_t *stroke);
typedef uint32_t (*xam_keystroke_ex_fn)(uint32_t *user, uint32_t flags, x360_keystroke_t *stroke);
static xam_keystroke_fn    g_orig_keystroke;
static xam_keystroke_ex_fn g_orig_keystroke_ex;
static xam_keystroke_fn    g_orig_keystroke_hud;
static xam_keystroke_ex_fn g_orig_keystroke_hud_ex;
static x360_key_tracker_t  g_key_tracker;
/* Trampolines are code the plugin writes: they live in .text, like hiddriver's ".text" Detour buffer. */
__asm__(".pushsection .text.bt360_trampolines,\"ax\",@progbits\n"
        ".balign 128\n"
        ".globl bt360_trampolines\n"
        "bt360_trampolines:\n"
        ".space 840\n"            /* HOOK_COUNT * PPC_DETOUR_TRAMPOLINE_WORDS * 4 */
        ".popsection\n");
extern uint32_t bt360_trampolines[];
#define g_trampolines bt360_trampolines
_Static_assert(HOOK_COUNT * PPC_DETOUR_TRAMPOLINE_WORDS * 4u == 840u, "trampoline buffer size");

static bool guide_is_open(void) {
    return g_sysui_by_xenon != NULL && g_sysui_by_xenon() != 0u;
}

/* O painel do usuário 0 lê o controle oficial. Enquanto ele está aberto e esse
 * controle está parado, a leitura dele leva o estado do DS4. */
static void overlay_guide_pad(INPUT_GAMEPAD *gamepad) {
    const x360_pad_snapshot_t snap = x360_pad_inject_snapshot(&g_pad);
    const bool idle = gamepad->buttons == 0u && gamepad->left_trigger == 0u &&
                      gamepad->right_trigger == 0u &&
                      gamepad->thumb_lx > -8000 && gamepad->thumb_lx < 8000 &&
                      gamepad->thumb_ly > -8000 && gamepad->thumb_ly < 8000 &&
                      gamepad->thumb_rx > -8000 && gamepad->thumb_rx < 8000 &&
                      gamepad->thumb_ry > -8000 && gamepad->thumb_ry < 8000;
    if (!snap.active || !idle) return;
    *gamepad = *(const INPUT_GAMEPAD *)&snap.state;
    gamepad->buttons = (uint16_t)(gamepad->buttons & (uint16_t)~INPUT_GAMEPAD_GUIDE);
}

static uint32_t hook_read_state(uint32_t context, uint32_t *packet, INPUT_GAMEPAD *gamepad,
                                uint32_t *unk) {
    uint32_t status = g_orig_read_state(context, packet, gamepad, unk);
    const uint32_t ds4_context = g_pad_device_context;
    const bool is_ds4 = ds4_context != 0u && context == ds4_context;
    const uint32_t result = x360_pad_inject_filter_read_state(&g_pad, is_ds4, status, packet,
                                                              (x360_gamepad_state_t *)gamepad);
    if (result == X360_PAD_ERROR_SUCCESS && status != X360_PAD_ERROR_SUCCESS && unk) *unk = 0u;
    if (!is_ds4 && result == X360_PAD_ERROR_SUCCESS && gamepad && guide_is_open())
        overlay_guide_pad(gamepad);
    return result;
}

static uint32_t hook_capabilities(uint32_t unk, uint32_t user, uint32_t flags, x360_pad_caps_t *caps) {
    const uint32_t status = g_orig_capabilities(unk, user, flags, caps);
    return x360_pad_inject_filter_capabilities(&g_pad, user, status, caps);
}

static uint32_t hook_set_state(uint32_t user, uint32_t flags, const INPUT_VIBRATION *vibration) {
    const uint32_t status = g_orig_set_state(user, flags, vibration);
    if (!vibration) return status;
    return x360_pad_inject_filter_set_state(&g_pad, user, status, vibration->left_motor_speed,
                                            vibration->right_motor_speed);
}

/* O painel aberto pelo PS é o do usuário 0. A tecla do DS4 só entra nessa consulta. */
static uint32_t supply_guide_key(uint32_t status, uint32_t user, x360_keystroke_t *stroke) {
    if (!stroke || status == X360_PAD_ERROR_SUCCESS) return status;
    if (!guide_is_open() || x360_pad_inject_normalize_user(user) != 0u) return status;
    const x360_pad_snapshot_t snap = x360_pad_inject_snapshot(&g_pad);
    if (!snap.active) return status;
    x360_keystroke_t gen;
    if (x360_pad_keystroke_next(&g_key_tracker, &snap.state, 0u,
                                (uint32_t)now_ms(), &gen) != X360_PAD_ERROR_SUCCESS)
        return status;
    *stroke = gen;
    return X360_PAD_ERROR_SUCCESS;
}

static uint32_t hook_keystroke(uint32_t user, uint32_t flags, x360_keystroke_t *stroke) {
    const uint32_t status = g_orig_keystroke(user, flags, stroke);
    return supply_guide_key(status, user, stroke);
}

static uint32_t hook_keystroke_hud(uint32_t user, uint32_t flags, x360_keystroke_t *stroke) {
    const uint32_t status = g_orig_keystroke_hud(user, flags, stroke);
    return supply_guide_key(status, user, stroke);
}

static uint32_t hook_keystroke_ex(uint32_t *user, uint32_t flags, x360_keystroke_t *stroke) {
    const uint32_t status = g_orig_keystroke_ex(user, flags, stroke);
    return supply_guide_key(status, user ? *user : 0u, stroke);
}

static uint32_t hook_keystroke_hud_ex(uint32_t *user, uint32_t flags, x360_keystroke_t *stroke) {
    const uint32_t status = g_orig_keystroke_hud_ex(user, flags, stroke);
    return supply_guide_key(status, user ? *user : 0u, stroke);
}

/* Data cache to memory, then the instruction cache (the sequence of xsdk_code_flush, which the
 * plugin runtime does not carry): 128-byte lines on Xenon. */
static void code_flush(const void *start, size_t bytes) {
    if (!bytes) return;
    const uintptr_t first = (uintptr_t)start & ~(uintptr_t)127u;
    const uintptr_t last = ((uintptr_t)start + bytes - 1u) & ~(uintptr_t)127u;
    for (uintptr_t line = first;; line += 128u) {
        __asm__ volatile("dcbst 0, %0" : : "r"(line) : "memory");
        if (line == last) break;
    }
    __asm__ volatile("sync" : : : "memory");
    for (uintptr_t line = first;; line += 128u) {
        __asm__ volatile("icbi 0, %0" : : "r"(line) : "memory");
        if (line == last) break;
    }
    __asm__ volatile("sync" : : : "memory");
    __asm__ volatile("isync" : : : "memory");
}

static bool hook_install(const char *name, HANDLE module, uint32_t ordinal, unsigned index,
                         const void *hook, void **original) {
    void *target = NULL;
    const NTSTATUS resolved = module ? XexGetProcedureAddress(module, ordinal, &target) : STATUS_UNSUCCESSFUL;
    if (!NT_SUCCESS(resolved) || !target || !MmIsAddressValid(target)) {
        xsdk_plugin_log("pad: %s ordinal %u not resolved (status=0x%08X); hook not installed", name,
                        (unsigned)ordinal, (unsigned)resolved);
        return false;
    }
    uint32_t *tramp = &g_trampolines[index * PPC_DETOUR_TRAMPOLINE_WORDS];
    size_t used = 0;
    *original = tramp; /* before the detour: a call may arrive the moment the branch is written */
    if (!ppc_detour_install(&g_detours[index], (uint32_t *)target, (uint32_t)(uintptr_t)target,
                            (uint32_t)(uintptr_t)hook, tramp, (uint32_t)(uintptr_t)tramp,
                            PPC_DETOUR_TRAMPOLINE_WORDS, &used, code_flush)) {
        *original = NULL;
        xsdk_plugin_log("pad: %s ordinal %u at 0x%08X: detour refused", name, (unsigned)ordinal,
                        (unsigned)(uintptr_t)target);
        return false;
    }
    xsdk_plugin_log("pad: %s ordinal %u at 0x%08X hooked, trampoline %u words", name,
                    (unsigned)ordinal, (unsigned)(uintptr_t)target, (unsigned)used);
    return true;
}

/* Installs only the hooks whose ordinal resolved; the original is always set before the detour. */
static void pad_hooks_install(void) {
    HANDLE kernel = 0, xam = 0;
    if (XexGetModuleHandle("xboxkrnl.exe", &kernel) != 0) kernel = 0;
    if (XexGetModuleHandle("xam.xex", &xam) != 0) xam = 0;
    hook_install("XInputdReadState", kernel, KRNL_ORD_XINPUTD_READ_STATE, HOOK_READ_STATE,
                 (const void *)hook_read_state, (void **)&g_orig_read_state);
    hook_install("XamInputGetCapabilitiesEx", xam, XAM_ORD_INPUT_GET_CAPABILITIES_EX, HOOK_CAPABILITIES,
                 (const void *)hook_capabilities, (void **)&g_orig_capabilities);
    hook_install("XamInputSetState", xam, XAM_ORD_INPUT_SET_STATE, HOOK_SET_STATE,
                 (const void *)hook_set_state, (void **)&g_orig_set_state);
    hook_install("XamInputGetKeystroke", xam, XAM_ORD_INPUT_GET_KEYSTROKE, HOOK_KEYSTROKE,
                 (const void *)hook_keystroke, (void **)&g_orig_keystroke);
    hook_install("XamInputGetKeystrokeEx", xam, XAM_ORD_INPUT_GET_KEYSTROKE_EX, HOOK_KEYSTROKE_EX,
                 (const void *)hook_keystroke_ex, (void **)&g_orig_keystroke_ex);
    hook_install("XamInputGetKeystrokeHud", xam, XAM_ORD_INPUT_GET_KEYSTROKE_HUD, HOOK_KEYSTROKE_HUD,
                 (const void *)hook_keystroke_hud, (void **)&g_orig_keystroke_hud);
    hook_install("XamInputGetKeystrokeHudEx", xam, XAM_ORD_INPUT_GET_KEYSTROKE_HUD_EX,
                 HOOK_KEYSTROKE_HUD_EX, (const void *)hook_keystroke_hud_ex,
                 (void **)&g_orig_keystroke_hud_ex);
    void *sysui = NULL;
    if (xam && NT_SUCCESS(XexGetProcedureAddress(xam, XAM_ORD_SYSUI_BY_XENON, &sysui)) &&
        sysui && MmIsAddressValid(sysui))
        g_sysui_by_xenon = (sysui_xenon_fn)sysui;
    xsdk_plugin_log("pad: XamIsSysUiInvokedByXenonButton ordinal %u %s",
                    (unsigned)XAM_ORD_SYSUI_BY_XENON, g_sysui_by_xenon ? "resolved" : "missing");
    void *xenon = NULL;
    if (xam && NT_SUCCESS(XexGetProcedureAddress(xam, XAM_ORD_SEND_XENON_BUTTON, &xenon)) &&
        xenon && MmIsAddressValid(xenon)) {
        g_xenon_press = (xenon_press_fn)xenon;
        xsdk_plugin_log("pad: XamInputSendXenonButtonPress ordinal %u at 0x%08X",
                        (unsigned)XAM_ORD_SEND_XENON_BUTTON, (unsigned)(uintptr_t)xenon);
    } else {
        xsdk_plugin_log("pad: XamInputSendXenonButtonPress ordinal %u not resolved",
                        (unsigned)XAM_ORD_SEND_XENON_BUTTON);
    }
}

/* Sink of x360_pad_inject: the slot is the state the hooks above hand to XamInput. */
static void pad_write_slot(void *ctx, uint8_t user_index, const x360_gamepad_state_t *state) {
    (void)ctx;
    (void)state;
    /* Transitions only, never per frame: bt_trace writes files. */
    static int8_t last_user = -1;
    if ((int8_t)user_index != last_user) {
        last_user = (int8_t)user_index;
        xsdk_plugin_log("pad: DS4 state on XInput user %u (hooks serve it)", (unsigned)user_index);
    }
}

static void pad_send_report(void *ctx, const uint8_t *report, size_t len) {
    (void)ctx;
    if (bt_hid_link_output(report, len) != 0) {
        static bool logged;
        if (!logged) {
            logged = true;
            xsdk_plugin_log("pad: rumble frame has no open HID channel");
        }
    }
}

/* "Down": removes the DS4 context from XAM and clears only the DS4's own state. */
static void pad_unbind(void) {
    g_pad_device_context = 0u; /* hooks stop answering before the context disappears */
    x360_pad_inject_drop(&g_pad);
    if (g_pad_bound && g_bind_device) {
        g_bind_device(x360_pad_controller_id(DS4_BIND_INDEX), x360_pad_device_context(DS4_BIND_INDEX),
                      0u, 1u, NULL);
        xsdk_plugin_log("pad: DS4 context unbound from XAM (player %u)", (unsigned)g_pad_bound_player);
    }
    g_pad_bound = false;
    g_pad_bound_player = 0u;
    g_pad_user = 0xFFu;
    g_guide_was_down = false;
    g_guide_last_ms = 0u;
    memset(&g_key_tracker, 0, sizeof g_key_tracker);
}

/* "Up": registers the DS4 context and checks which user XAM gave it. */
static bool pad_bind(uint8_t player) {
    if (!g_layout_17559 || !g_bind_device) {
        if (!g_pad_logged_layout) {
            g_pad_logged_layout = true;
            xsdk_plugin_log("pad: kernel build %u / layout is not retail 17559; XamUserBindDeviceCallback not called",
                            (unsigned)XboxKrnlVersion->build);
        }
        return false;
    }
    uint8_t user = X360_PAD_USER_INDEX_NONE;
    const uint32_t context = x360_pad_device_context(DS4_BIND_INDEX);
    const int result = g_bind_device(x360_pad_controller_id(DS4_BIND_INDEX), context, 0u, 0u, &user);
    const x360_pad_bind_verdict_t verdict = x360_pad_inject_check_bind(player, user);
    xsdk_plugin_log("pad: bind result=%d userIndex=%u player=%u verdict=%u", result, (unsigned)user,
                    (unsigned)player, (unsigned)verdict);
    if (verdict != X360_PAD_BIND_OK) {
        xsdk_plugin_log(verdict == X360_PAD_BIND_OFFICIAL_SLOT
                            ? "pad: XAM gave the DS4 the official controller's slot; not publishing there"
                            : "pad: XAM gave the DS4 a user other than the decided player; not publishing");
        g_bind_device(x360_pad_controller_id(DS4_BIND_INDEX), context, 0u, 1u, NULL);
        return false;
    }
    g_pad_bound = true;
    g_pad_bound_player = player;
    g_pad_user = user;
    g_pad_device_context = context;
    xsdk_plugin_log("pad: DS4 context 0x%08X bound on user %u", (unsigned)context, (unsigned)user);
    if (!g_pad_logged_empty) {
        g_pad_logged_empty = true;
        xsdk_plugin_log("pad: DS4 context is live; buttons arrive on the HID channel");
    }
    return true;
}

/* Plugin thread, every tick: mirrors the pairing session into XAM through x360_pad_inject. */
static void pad_service(void) {
    const bool authenticated = g_session.state == BT_PAIR_CONNECTED && g_session.established;
    if (g_session.player >= X360_PAD_PLAYER_MIN && g_session.player <= X360_PAD_PLAYER_MAX)
        g_pad_player = g_session.player;
    if (!authenticated) {
        if (g_hid_up) {
            bt_hid_link_stop();
            g_hid_up = false;
        }
        pad_unbind();
        g_pad_logged_no_player = false;
        g_pad_bind_refused = false;
        return;
    }
    /* O DS4 derruba o ACL se o HID não abrir. Isso não espera o jogador do XAM. */
    if (!g_hid_up && g_session.encrypted &&
        g_session.acl_handle != 0 && g_session.acl_handle != 0xFFFFu) {
        if (bt_hid_link_start(g_session.acl_handle) == 0)
            g_hid_up = true;
    }
    if (g_pad_player == 0u) {
        if (!g_pad_logged_no_player) {
            g_pad_logged_no_player = true;
            xsdk_plugin_log("pad: DS4 authenticated but the session decided no player; not publishing");
        }
        return;
    }
    if (g_pad_bound && g_pad_bound_player != g_pad_player) {
        pad_unbind();
        g_pad_bind_refused = false;
    }
    if (!g_pad_bound) {
        if (g_pad_bind_refused) return;
        if (!pad_bind(g_pad_player)) {
            g_pad_bind_refused = true;
            return;
        }
    }
    x360_pad_inject_publish(&g_pad, true, g_pad_player, &g_pad_report, g_pad_ps);
    /* O painel visível é o do perfil do controle oficial (usuário 0), não o do jogador 2. */
    const bool guide_down = g_pad_ps;
    const uint32_t now = (uint32_t)now_ms();
    if (guide_down && g_xenon_press &&
        (!g_guide_was_down || now - g_guide_last_ms >= 1000u)) {
        if (!g_guide_was_down) xsdk_plugin_log("pad: guide press user=0");
        g_guide_last_ms = now;
        g_xenon_press(0u);
    }
    g_guide_was_down = guide_down;
}

static void pad_setup(void) {
    const x360_pad_sink_t sink = { .ctx = NULL, .write_slot = pad_write_slot,
                                   .send_report = pad_send_report };
    x360_pad_inject_init(&g_pad, &sink);
    memset(&g_pad_report, 0, sizeof g_pad_report);
    bt_hid_link_bind(&g_pad_report, &g_pad_ps);
    x360_usbd_set_acl_rx(bt_hid_link_on_acl);
    pad_hooks_install();
}

static void pair_session_setup(void) {
    bt_ipc_init(&g_ipc);
    const bt_pair_ops_t ops = {
        .ctx = NULL,
        .send_cmd = pair_send_cmd,
        .start_inquiry = pair_start_inquiry,
        .stop_inquiry = pair_stop_inquiry,
        .official_on_player1 = pair_official_on_player1,
        .persist = pair_persist,
        .notify = pair_notify,
        .log = pair_log,
    };
    bt_pair_session_init(&g_session, &g_ipc, &ops);
}

/* Uma linha por fato que decide se um aparelho aparece ou conecta. */
static void log_link_event(const uint8_t *evt, int n) {
    if (n < (int)HCI_EVT_HDR_SIZE) return;
    const uint8_t code = evt[0];
    if (code == HCI_EVT_INQUIRY_RESULT || code == HCI_EVT_INQUIRY_RESULT_RSSI ||
        code == HCI_EVT_EXTENDED_INQUIRY_RESULT) {
        return; /* o endereco sai em scan: dev quando a lista cresce */
    }
    if (code == HCI_EVT_INQUIRY_COMPLETE) {
        xsdk_plugin_log("scan: complete status=%u", n >= 3 ? evt[2] : 0u);
        return;
    }
    if (code == HCI_EVT_COMMAND_STATUS) {
        hci_cmd_status_t cs;
        if (hci_parse_command_status(evt, (size_t)n, &cs) != 0) return;
        if (cs.opcode != HCI_OP_INQUIRY && cs.opcode != HCI_OP_CREATE_CONNECTION &&
            cs.opcode != HCI_OP_WRITE_SCAN_ENABLE && cs.opcode != HCI_OP_WRITE_SIMPLE_PAIRING &&
            cs.opcode != HCI_OP_WRITE_INQUIRY_MODE && cs.opcode != HCI_OP_REMOTE_NAME_REQUEST)
            return;
        xsdk_plugin_log("hci: status op=%04X st=%02X", cs.opcode, cs.status);
        return;
    }
    if (code == HCI_EVT_REMOTE_NAME_REQ_COMPLETE && n >= 9) {
        char name[32];
        size_t k = 0;
        while (k < 31u && 9 + (int)k < n && evt[9 + k] != 0) {
            name[k] = (char)evt[9 + k];
            k++;
        }
        name[k] = '\0';
        xsdk_plugin_log("scan: name %02X:%02X:%02X:%02X:%02X:%02X %s",
                        evt[8], evt[7], evt[6], evt[5], evt[4], evt[3], name);
        return;
    }
    if (code == HCI_EVT_CONNECTION_COMPLETE && n >= 13) {
        xsdk_plugin_log("hci: connect status=%02X handle=%04X %02X:%02X:%02X:%02X:%02X:%02X",
                        evt[2], (unsigned)hci_get_le16(evt + 3),
                        evt[10], evt[9], evt[8], evt[7], evt[6], evt[5]);
        return;
    }
    if (code == HCI_EVT_AUTH_COMPLETE && n >= 5) {
        xsdk_plugin_log("hci: auth status=%02X handle=%04X", evt[2],
                        (unsigned)hci_get_le16(evt + 3));
        return;
    }
    if (code == HCI_EVT_ENCRYPTION_CHANGE && n >= 6) {
        xsdk_plugin_log("hci: encrypt status=%02X handle=%04X on=%u", evt[2],
                        (unsigned)hci_get_le16(evt + 3), evt[5]);
        return;
    }
    if (code == HCI_EVT_DISCONNECTION_COMPLETE && n >= 6) {
        xsdk_plugin_log("hci: disconnect status=%02X handle=%04X reason=%02X",
                        evt[2], (unsigned)hci_get_le16(evt + 3), evt[5]);
    }
}

/* A inquiry dura 48 * 1,28 s. Sem o evento de fim, a tela ficava buscando. */
static void end_scan_if_stuck(bt_usb_transport_t *tr, uint64_t now);

/*
 * Runs on the plugin thread every LOOP_MS. HCI events arrive from the USB DPC through the
 * transport's SPSC queue; commands leave through its command queue.
 */
static void pair_session_service(uint32_t stage, uint32_t *ready_ms, bool *session_up,
                                 uint64_t *last_pump_ms) {
    const uint64_t now = now_ms();
    bt_usb_transport_t *tr = stage == BT_USB_STAGE_READY ? pair_transport() : NULL;
    const bool ini_was_loaded = g_ini_loaded;
    ini_poll(); /* bt_devices.ini is read as soon as the volume answers, adapter or not */

    if (!tr) {
        /* RESPONDED durante o reset do firmware nao e desconexao. */
        if (stage == BT_USB_STAGE_WAITING || stage == BT_USB_STAGE_REMOVING ||
            x360_usbd_adapter_removed())
            rtl_boot_reset();
        pad_unbind(); /* adapter gone: the DS4 is gone; release only its context and slot */
        if (*session_up) {
            bt_pair_session_adapter_lost(&g_session);
            xsdk_plugin_log("pair: adapter lost, session reset");
            *session_up = false;
        }
        *ready_ms = 0;
        return;
    }
    if (!*session_up) {
        *ready_ms += LOOP_MS;
        if (!g_ini_loaded && *ready_ms < INI_WAIT_MS) return;
        bt_usb_transport_enable_events(tr, true);
        if (!rtl_boot_ready(tr)) return;
        *session_up = true;
        *last_pump_ms = now;
        xsdk_plugin_log("pair: adapter ready, ini=%u stored=%u lmp=%04X", (unsigned)g_ini_loaded,
                        (unsigned)(bt_pair_session_stored_addr(&g_session) != NULL),
                        tr->local_ver.lmp_subversion);
        bt_pair_session_adapter_ready(&g_session, now);
        return;
    }

    /* A volume that mounted late may bring a DS4 the session did not know about. */
    if (!ini_was_loaded && g_ini_loaded && bt_pair_session_stored_addr(&g_session))
        bt_pair_session_on_command(&g_session, BT_IPC_CMD_CONNECT_PAIRED, NULL, now);

    const uint64_t delta = now > *last_pump_ms ? now - *last_pump_ms : 0u;
    *last_pump_ms = now;
    bt_usb_transport_pump(tr, delta > 1000u ? 1000u : (uint32_t)delta);
    end_scan_if_stuck(tr, now);

    for (unsigned i = 0; i < BT_USB_EVT_FIFO_DEPTH; i++) {
        const int n = bt_usb_transport_pop_event(tr, g_event, sizeof g_event);
        if (n <= 0) break;
        log_link_event(g_event, n);
        bt_pair_session_on_event(&g_session, g_event, (size_t)n, now);
    }
    const uint32_t dropped = bt_usb_transport_events_dropped(tr);
    if (dropped != g_events_dropped_logged) {
        xsdk_plugin_log("pair: %u HCI events dropped", (unsigned)dropped);
        g_events_dropped_logged = dropped;
    }
    bt_pair_session_tick(&g_session, now);
    pad_service();
}

/* A inquiry dura 48 * 1,28 s. Sem o evento de fim, a tela ficava buscando. */
static void end_scan_if_stuck(bt_usb_transport_t *tr, uint64_t now) {
    static uint64_t scan_since;
    static uint32_t seen_timeouts;
    if (!tr->scan_active) {
        scan_since = 0;
        seen_timeouts = tr->cmd_timeouts;
        return;
    }
    if (scan_since == 0) scan_since = now;
    const bool timed_out = tr->cmd_timeouts != seen_timeouts;
    const bool window_over = now > scan_since && now - scan_since > 65000u;
    seen_timeouts = tr->cmd_timeouts;
    if (!timed_out && !window_over) return;
    tr->scan_active = false;
    x360_usbd_release_control();
    xsdk_plugin_log("scan: ended without result devices=%u %s",
                    (unsigned)tr->scan_count, timed_out ? "timeout" : "window");
    scan_since = 0;
}

/* A tela le Mass0\bt360_scan.bin. O reply de 32 bytes nao cabe na lista. */
static void publish_scan_list(uint32_t stage) {
    static uint32_t published_count = 0xFFFFFFFFu;
    static uint8_t published_active = 0xFFu;
    static uint32_t ticks = 0;
    bt_usb_transport_t *tr = stage == BT_USB_STAGE_READY ? pair_transport() : NULL;
    if (!tr) return;
    const uint8_t active = tr->scan_active ? 1u : 0u;
    const bool periodic = active && (++ticks % 10u) == 0u;
    if (!periodic && tr->scan_count == published_count && active == published_active) return;
    uint8_t buf[BT_SCAN_FILE_BYTES];
    if (bt_scan_pack(buf, sizeof buf, tr->scan_results, (uint32_t)tr->scan_count,
                     tr->scan_active) != BT_SCAN_FILE_BYTES)
        return;
    if (!bt_scan_write_file(buf)) return;
    if (tr->scan_count != published_count || active != published_active || periodic)
        xsdk_plugin_log("scan: devices=%u active=%u opcode=%04X evt=%02X events=%u timeouts=%u err=%d",
                        (unsigned)tr->scan_count, active,
                        __atomic_load_n(&tr->trace_opcode, __ATOMIC_RELAXED),
                        __atomic_load_n(&tr->trace_evt, __ATOMIC_RELAXED),
                        __atomic_load_n(&tr->trace_events, __ATOMIC_RELAXED),
                        tr->cmd_timeouts, tr->last_error);
    if (tr->scan_count != published_count) {
        for (size_t i = 0; i < tr->scan_count; i++) {
            const hci_inquiry_item_t *it = &tr->scan_results[i];
            char name[32];
            memcpy(name, it->name, 31u);
            name[31] = '\0';
            xsdk_plugin_log("scan: dev %02X:%02X:%02X:%02X:%02X:%02X class=%06X %s",
                            it->bd_addr[5], it->bd_addr[4], it->bd_addr[3],
                            it->bd_addr[2], it->bd_addr[1], it->bd_addr[0],
                            it->class_of_device, name);
        }
    }
    published_count = (uint32_t)tr->scan_count;
    published_active = active;
}

/* UTF-16 base name of the running title, or an empty string. Disk stays up. */
static void copy_running_title(char *out, size_t cap) {
    out[0] = '\0';
    if (cap < 2u || !XexExecutableModuleHandle || !MmIsAddressValid((const void *)XexExecutableModuleHandle))
        return;
    const HANDLE module = *XexExecutableModuleHandle;
    if (!module || !MmIsAddressValid((const void *)(uintptr_t)module)) return;
    const LDR_DATA_TABLE_ENTRY *ldr = (const LDR_DATA_TABLE_ENTRY *)module;
    const uint16_t nbytes = (uint16_t)(ldr->base_dll_name[0] >> 16);
    const uint16_t *chars = (const uint16_t *)(uintptr_t)ldr->base_dll_name[1];
    if (nbytes < 2u || nbytes > 80u || (nbytes & 1u) || !chars || !MmIsAddressValid(chars)) return;
    if (!MmIsAddressValid((const uint8_t *)chars + (nbytes - 1u))) return;
    size_t n = 0;
    for (size_t i = 0; i < nbytes / 2u && n + 1u < cap; i++) {
        const uint16_t c = chars[i];
        out[n++] = (c >= 32u && c <= 126u) ? (char)c : '?';
    }
    out[n] = '\0';
}

static bool title_is_aurora(const char *name) {
    static const char needle[] = "aurora";
    for (size_t i = 0; name[i] != '\0'; i++) {
        size_t j = 0;
        while (needle[j] != '\0') {
            char c = name[i + j];
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if (c != needle[j]) break;
            j++;
        }
        if (needle[j] == '\0') return true;
    }
    return false;
}

/* One silent bus reset once Aurora has been the running title for AURORA_SETTLE_TICKS.
 * Returns true after that reset has returned. No log while the bus is down. */
static bool aurora_bus_reset(int ordinal_rc) {
    static bool done;
    static uint32_t stable;
    static char seen[40];
    if (done || ordinal_rc != 0) return done;
    char name[40];
    memset(name, 0, sizeof name);
    copy_running_title(name, sizeof name);
    if (name[0] != '\0' && memcmp(name, seen, sizeof name) != 0) {
        memcpy(seen, name, sizeof name);
        xsdk_plugin_log("usb: title %s", name);
    }
    if (!title_is_aurora(name)) {
        stable = 0;
        return false;
    }
    if (++stable < AURORA_SETTLE_TICKS) return false;
    xsdk_plugin_log("usb: Aurora is the running title; resetting the bus");
    const int rc = x360_usbd_enumerate_ugreen();
    done = true;
    xsdk_plugin_log("usb: bus reset after Aurora rc=%d", rc);
    return true;
}

/* Returns false while the adapter is still arriving (no phrase yet). */
static bool adapter_state(int rc, uint32_t stage, uint32_t waiting_ticks,
                          bt_phase1_adapter_state_t *state) {
    if (rc != 0) { *state = BT_PHASE1_ADAPTER_NONE; return true; }
    if (stage == BT_USB_STAGE_READY) { *state = BT_PHASE1_ADAPTER_READY; return true; }
    if (x360_usbd_adapter_removed()) { *state = BT_PHASE1_ADAPTER_REMOVED; return true; }
    if (stage != BT_USB_STAGE_WAITING) return false; /* UGREEN arrived, not ready yet */
    if (x360_usbd_wrong_adapter_hits()) { *state = BT_PHASE1_ADAPTER_WRONG; return true; }
    if (waiting_ticks >= ADAPTER_NOT_FOUND_TICKS) { *state = BT_PHASE1_ADAPTER_NONE; return true; }
    return false;
}

void plugin_main(HANDLE module) {
    (void)module;

    xsdk_plugin_log_open("\\Device\\Mass0\\bt360_diag.log");
    xsdk_plugin_log("bt360_proto: loaded (kernel %u.%u.%u.%u)\n",
                    XboxKrnlVersion->major, XboxKrnlVersion->minor,
                    XboxKrnlVersion->build, XboxKrnlVersion->qfe);

    int rc = x360_usbd_resolve_ordinals();
    /* rc == 0 only past the USB HAL's retail 17559 layout guard (build and kernel pointers). */
    g_layout_17559 = rc == 0 && XboxKrnlVersion->build == KERNEL_BUILD_17559;
    if (g_layout_17559)
        g_bind_device = (xam_user_bind_device_callback_fn)(uintptr_t)XAM_USER_BIND_DEVICE_CALLBACK_17559;
    xsdk_plugin_log("usb: ordinal result=%d; bus reset waits for Aurora", rc);
    xsdk_plugin_log("ipc: polling request file on Mass0 (protocol %u)", BT_IPC_VERSION);
    xsdk_plugin_log("trace: bt360_trace.log na raiz do HD interno e do Mass0; copia em bt360_diag.log");

    pair_session_setup();
    pad_setup();

    uint64_t last_token = 0;
    uint64_t logged_token = 0;
    uint64_t dispatched_token = 0;
    bool write_failed = false;
    bool first_pass = true;
    uint32_t phrase_id = BT_PHRASE_NONE;
    uint8_t player = 0;
    uint32_t last_notified = BT_PHRASE_NONE;
    uint32_t waiting_ticks = 0;
    uint32_t pair_ready_ms = 0;
    uint64_t pair_last_pump_ms = 0;
    bool pair_session_up = false;
    uint32_t loop_tick = 0;
    bool usb_bus_ready = rc != 0;
    while (1) {
        x360_usbd_service();
        bt_hid_link_tick(now_ms());
        const uint32_t stage = x360_usbd_adapter_status();
        /* HCI events and the 60 s pairing clock: every LOOP_MS. */
        pair_session_service(stage, &pair_ready_ms, &pair_session_up, &pair_last_pump_ms);
        notify_link_edge();
        /* So durante a carga dos blocos. O reset do radio nao segura mais esta frase. */
        const uint32_t shown = (stage == BT_USB_STAGE_READY && rtl_boot_pending())
                                   ? BT_USB_STAGE_RESPONDED : stage;

        if (loop_tick++ % MAILBOX_EVERY_TICKS != 0) {
            sleep_ms(LOOP_MS);
            continue;
        }

        if (!usb_bus_ready) usb_bus_ready = aurora_bus_reset(rc);
        publish_scan_list(stage);
        x360_usbd_log_changes();
        if (!usb_bus_ready) waiting_ticks = 0;
        else if (stage == BT_USB_STAGE_WAITING) waiting_ticks++;
        else waiting_ticks = 0;
        bt_phase1_adapter_state_t state;
        if (usb_bus_ready && adapter_state(rc, shown, waiting_ticks, &state)) {
            const bt_phase1_context_t ctx = {
                .event = BT_PHASE1_EV_ADAPTER, .adapter = state
            };
            const bt_phase1_decision_t decision = bt_phase1_decide(&ctx);
            phrase_id = decision.phrase_id;
            player = decision.player;
            if (phrase_id != last_notified) {
                xsdk_plugin_log("adapter: phrase=%u player=%u stage=%u", phrase_id, player, stage);
                notify_phrase(phrase_id);
                last_notified = phrase_id;
            }
        } else {
            phrase_id = BT_PHRASE_NONE;
            player = 0;
        }
        bt_console_request_t request;
        if (bt_console_read_request(&request) && request.token != last_token) {
            const bool trace = last_token == 0 || request.command != BT_IPC_CMD_NONE;
            if (trace && logged_token != request.token) {
                xsdk_plugin_log("ipc: request command=%u slot=%u", request.command, request.slot);
                logged_token = request.token;
            }
            if (request.token != dispatched_token) {
                dispatched_token = request.token;
                if (first_pass) {
                    /* Request left on the volume by an earlier boot: never start a scan from it. */
                    if (request.command != BT_IPC_CMD_NONE)
                        xsdk_plugin_log("ipc: stale request command=%u ignored", request.command);
                } else if (request.command != BT_IPC_CMD_NONE) {
                    static const uint8_t no_address[6] = { 0 };
                    const bool has_address = memcmp(request.address, no_address, 6u) != 0;
                    const hci_inquiry_item_t *seen = NULL;
                    bt_usb_transport_t *tr = pair_transport();
                    if (tr && has_address && request.command == BT_IPC_CMD_PAIR_AND_CONNECT) {
                        for (size_t i = 0; i < tr->scan_count; i++) {
                            if (memcmp(tr->scan_results[i].bd_addr, request.address, 6u) == 0) {
                                seen = &tr->scan_results[i];
                                break;
                            }
                        }
                    }
                    if (request.command == BT_IPC_CMD_SET_PLAYER && has_address &&
                        request.slot >= 1u && request.slot <= 4u) {
                        if (bt_manager_set_player(&g_ipc, request.address,
                                                  (uint8_t)request.slot) >= 0) {
                            pair_persist(NULL, &g_ipc);
                            if (g_session.has_peer &&
                                memcmp(g_session.peer, request.address, 6u) == 0)
                                g_session.player = (uint8_t)request.slot;
                        }
                    } else if (seen)
                        bt_pair_session_connect_known(&g_session, seen, now_ms());
                    else
                        bt_pair_session_on_command(&g_session, request.command,
                                                   has_address ? request.address : NULL, now_ms());
                }
            }
            /* Console sees the DS4 event while it lasts; the adapter phrase otherwise. */
            const bool ds4_event = g_session.phrase_id != BT_PHRASE_NONE;
            uint8_t link = BT_LINK_DOWN;
            if (g_session.state == BT_PAIR_CONNECTING || g_session.state == BT_PAIR_AUTH)
                link = BT_LINK_CONNECTING;
            else if (g_session.state == BT_PAIR_CONNECTED && g_session.established)
                link = BT_LINK_UP;
            const bool share = (g_pad_report.buttons & X360_BTN_BACK) != 0 &&
                               g_session.player >= 1u && g_session.player <= 4u;
            const bt_console_reply_t reply = {
                .magic = BT_IPC_MAGIC,
                .version = BT_IPC_VERSION,
                .token = request.token,
                .usb_status = rc,
                .adapter_stage = shown,
                .phrase_id = ds4_event ? g_session.phrase_id : phrase_id,
                .player = (ds4_event || link != BT_LINK_DOWN) ? g_session.player : player,
                .pair_active = g_session.pair_active ? 1u : 0u,
                .link = link,
                .reserved = share ? g_session.player : 0u
            };
            if (bt_console_write_reply(&reply)) {
                if (trace || write_failed) {
                    xsdk_plugin_log("ipc: reply written usb_status=%d command=%u phrase=%u pair_active=%u",
                                    rc, request.command, reply.phrase_id, reply.pair_active);
                }
                last_token = request.token;
                write_failed = false;
            } else if (!write_failed) {
                xsdk_plugin_log("ipc: failed to write bt360_reply.bin on Mass0");
                write_failed = true;
            }
        }
        first_pass = false;
        sleep_ms(LOOP_MS);
    }
}
