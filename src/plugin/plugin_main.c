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

/* A faixa é do XNotify, numa thread curta de título. O arquivo fica só como cópia.
 * A thread que pagina o controle não chama o XAM. */
#define ADAPTER_NOT_FOUND_TICKS  150u /* 15 s of 100 ms polls without the UGREEN */
#define AURORA_SETTLE_TICKS      150u /* 15 s with Aurora as the running title */
#define NOTICE_FILE "\\Device\\Mass0\\bt360_notice.txt"

static void notice_write(const char *text);
static bool notify_system(const char *text);

static void notify_phrase(uint32_t phrase_id) {
    const char *text = bt_phase1_phrase_text(phrase_id);
    xsdk_plugin_log("notify: phrase %u", (unsigned)phrase_id);
    if (text) notify_system(text);
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

static void notify_pad_edge(bool up, uint8_t player, bool *was_up) {
    char text[64];
    if (!was_up || up == *was_up) return;
    if (up && player >= 1u && player <= 4u) {
        static const char prefix[] = "DualShock 4 / Conectado no Jogador ";
        memcpy(text, prefix, sizeof prefix - 1u);
        text[sizeof prefix - 1u] = (char)('0' + player);
        text[sizeof prefix] = '.';
        text[sizeof prefix + 1u] = '\0';
    } else if (up) {
        memcpy(text, "DualShock 4 / Conectado.", sizeof "DualShock 4 / Conectado.");
    } else {
        memcpy(text, "DualShock 4 desconectado.", sizeof "DualShock 4 desconectado.");
    }
    if (notify_system(text)) *was_up = up;
}

/* Faixa quando cada DualShock entra ou sai. */
static void notify_link_edge(void) {
    static bool was_up;
    static bool was_second;
    notify_pad_edge(g_session.state == BT_PAIR_CONNECTED && g_session.established,
                    g_session.player, &was_up);
    notify_pad_edge(g_session.second.established, g_session.second.player, &was_second);
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

/* Uma linha: 8 dígitos, espaço, texto. A Aurora mostra e o plugin segue. */
static uint32_t g_notice_seq;

static void notice_write(const char *text) {
    char buf[160];
    size_t i = 9u;
    size_t n;
    uint32_t seq;
    if (!text || !text[0]) return;
    seq = ++g_notice_seq;
    for (int d = 7; d >= 0; d--) {
        buf[d] = (char)('0' + (seq % 10u));
        seq /= 10u;
    }
    buf[8] = ' ';
    while (text[0] && i + 2u < sizeof buf) {
        buf[i++] = text[0] == '\n' ? ' ' : text[0];
        text++;
    }
    buf[i++] = '\n';
    n = i;
    if (ini_transfer(NOTICE_FILE, sizeof NOTICE_FILE, buf, &n, true))
        xsdk_plugin_log("notify: aurora notice %u", (unsigned)g_notice_seq);
    else
        xsdk_plugin_log("notify: aurora notice failed status=0x%08X", (unsigned)g_ini_status);
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

/* Faixa do sistema, como o XNotify Tweaks: thread de título, ícone 34.
 * XapiThreadStartup quando o xam entrega o endereço. Sem ele, a mesma flag
 * TITLE_EXEC do código publicado. Sem flag de sistema, sem tipo 3, sem 0x2F. */
#define NOTICE_TYPE_EXCLAIM        34u
#define NOTICE_USER_ANY            0xFFu
#define NOTICE_AREA_SYSTEM         1ull
#define NOTICE_SYSTEM              0x2u
#define NOTICE_TITLE_EXEC          0x100u
#define NOTICE_RETURN_KTHREAD      0x80u
#define NOTICE_STACK               (64u * 1024u)
#define NOTICE_WAIT_MS             5000u
#define XAM_ORD_NOTIFY_QUEUE_UI    656u
#define XAM_ORD_LOADER_GET_STATE   1191u
#define XAM_ORD_CREATE_THREAD      0x43Cu
#define XAM_ORD_XAPIP_CREATE_THREAD 0x45Cu
#define KRNL_ORD_PROCESS_TYPE      102u
#define LOADER_TITLE_RUNNING       5u
#define PROC_USER                  1u

typedef void (*xnotify_queue_ui_fn)(uint32_t type, uint32_t user, uint64_t areas,
                                    const uint16_t *text, void *context);
typedef uint32_t (*xam_dword_fn)(void);

/* Símbolo do XDK. O xsdk não o liga; se o linker não tiver, fica nulo. */
extern void XapiThreadStartup(void *start, void *context) __attribute__((weak));

static xnotify_queue_ui_fn g_queue_ui;
static xam_dword_fn        g_loader_state;
static xam_dword_fn        g_process_type;
static void               *g_xapi_startup;
static volatile uint32_t   g_notify_result;
static volatile bool       g_notify_busy;
static bool                g_notify_worker_up;
static bool                g_notify_stuck;
static bool                g_notify_logged_unavailable;
static bool                g_notify_ready;
static uint16_t            g_notice_wide[96];

static bool read_u32(uint32_t addr, uint32_t *out) {
    if ((addr & 3u) != 0u) return false;
    if (!MmIsAddressValid((const void *)(uintptr_t)addr)) return false;
    if (!MmIsAddressValid((const uint8_t *)(uintptr_t)addr + 3u)) return false;
    memcpy(out, (const void *)(uintptr_t)addr, sizeof *out);
    return true;
}

static void *resolve_ordinal(const char *module_name, uint32_t ordinal) {
    HANDLE module = 0;
    void *fn = NULL;
    if (XexGetModuleHandle(module_name, &module) != 0 || !module) return NULL;
    if (!NT_SUCCESS(XexGetProcedureAddress(module, ordinal, &fn)) || !fn) return NULL;
    if (!MmIsAddressValid(fn)) return NULL;
    return fn;
}

/* lis r6,hi + addi/ori r6,r6,lo vira o endereço passado no 4º argumento. */
static uint32_t ppc_r6_address(uint32_t lis, uint32_t next) {
    const uint32_t hi = lis & 0xFFFFu;
    const uint32_t op = next >> 26;
    const uint32_t rs = (next >> 21) & 31u;
    const uint32_t ra = (next >> 16) & 31u;
    const uint32_t lo = next & 0xFFFFu;
    if ((lis >> 26) != 15u || ((lis >> 21) & 31u) != 6u) return 0;
    if (op == 14u && rs == 6u && ra == 6u)
        return (hi << 16) + (uint32_t)(int32_t)(int16_t)lo;
    if (op == 24u && rs == 6u && ra == 6u)
        return (hi << 16) | lo;
    return 0;
}

static bool prologue_ok(uint32_t first) {
    /* stwu r1, d(r1) ou mflr r12: prólogo de função, não um dado. */
    return (first & 0xFFFF0000u) == 0x94210000u || first == 0x7D8802A6u;
}

/* O CreateThread do xam passa XapiThreadStartup no r6 antes de chamar o kernel.
 * Um candidato só, dentro da imagem, com prólogo. Vários ou nenhum: não usa. */
static void *startup_from_creator(void *creator, uint32_t image_base, uint32_t image_size) {
    const uint32_t start = (uint32_t)(uintptr_t)creator;
    uint32_t found = 0;
    unsigned hits = 0;
    if (!creator || image_size < 8u || start < image_base || start >= image_base + image_size)
        return NULL;
    for (unsigned i = 0; i < 192u; i++) {
        uint32_t word = 0;
        uint32_t next = 0;
        const uint32_t addr = start + i * 4u;
        if (!read_u32(addr, &word)) break;
        if (word == 0x4E800020u) break;
        if ((word >> 26) != 15u || !read_u32(addr + 4u, &next)) continue;
        const uint32_t target = ppc_r6_address(word, next);
        uint32_t first = 0;
        bool calls = false;
        if (target < image_base || target + 4u > image_base + image_size) continue;
        if ((target & 3u) != 0u || target == start) continue;
        if (!read_u32(target, &first) || !prologue_ok(first)) continue;
        for (unsigned j = i + 2u; j < i + 12u && j < 192u; j++) {
            uint32_t branch = 0;
            if (!read_u32(start + j * 4u, &branch)) break;
            if (branch == 0x4E800020u) break;
            if ((branch >> 26) == 18u && (branch & 1u) != 0u) {
                calls = true;
                break;
            }
        }
        if (!calls) continue;
        hits++;
        found = target;
    }
    if (hits != 1u) {
        xsdk_plugin_log("notify: startup scan hits=%u", hits);
        return NULL;
    }
    return (void *)(uintptr_t)found;
}

static void *find_xapi_startup(void) {
    void *linked = (void *)XapiThreadStartup;
    HANDLE xam = 0;
    const LDR_DATA_TABLE_ENTRY *ldr;
    void *from_xapip;
    void *from_create;
    if (linked) return linked;
    if (XexGetModuleHandle("xam.xex", &xam) != 0 || !xam) return NULL;
    if (!MmIsAddressValid((const void *)(uintptr_t)xam)) return NULL;
    ldr = (const LDR_DATA_TABLE_ENTRY *)(uintptr_t)xam;
    if (ldr->image_base == 0u || ldr->image_size < 8u) return NULL;
    from_xapip = startup_from_creator(resolve_ordinal("xam.xex", XAM_ORD_XAPIP_CREATE_THREAD),
                                      ldr->image_base, ldr->image_size);
    if (from_xapip) return from_xapip;
    from_create = startup_from_creator(resolve_ordinal("xam.xex", XAM_ORD_CREATE_THREAD),
                                       ldr->image_base, ldr->image_size);
    return from_create;
}

static void utf8_to_wide(const char *in, uint16_t *out, size_t cap) {
    size_t o = 0;
    if (cap == 0u) return;
    while (in[0] != '\0' && o + 1u < cap) {
        const uint8_t c = (uint8_t)in[0];
        uint32_t cp;
        if (c < 0x80u) {
            cp = c;
            in += 1;
        } else if ((c & 0xE0u) == 0xC0u && ((uint8_t)in[1] & 0xC0u) == 0x80u) {
            cp = ((uint32_t)(c & 0x1Fu) << 6) | ((uint8_t)in[1] & 0x3Fu);
            in += 2;
        } else if ((c & 0xF0u) == 0xE0u && ((uint8_t)in[1] & 0xC0u) == 0x80u &&
                   ((uint8_t)in[2] & 0xC0u) == 0x80u) {
            cp = ((uint32_t)(c & 0x0Fu) << 12) | ((uint32_t)((uint8_t)in[1] & 0x3Fu) << 6) |
                 ((uint8_t)in[2] & 0x3Fu);
            in += 3;
        } else {
            cp = '?';
            in += 1;
        }
        if (cp > 0xFFFFu) cp = '?';
        out[o++] = (uint16_t)cp;
    }
    out[o] = 0;
}

/* Só o XNotify. Sem página do controle e sem arquivo. */
static uint32_t notify_on_title(void *context) {
    (void)context;
    if (!g_process_type || (g_process_type() & 0xFFu) != PROC_USER ||
        !g_loader_state || g_loader_state() != LOADER_TITLE_RUNNING) {
        g_notify_result = 2u;
        return 0;
    }
    g_queue_ui(NOTICE_TYPE_EXCLAIM, NOTICE_USER_ANY, NOTICE_AREA_SYSTEM, g_notice_wide, NULL);
    g_notify_result = 1u;
    return 0;
}

static bool notify_prepare(void) {
    if (g_notify_ready) return g_queue_ui && g_loader_state && g_process_type;
    g_notify_ready = true;
    g_queue_ui = (xnotify_queue_ui_fn)resolve_ordinal("xam.xex", XAM_ORD_NOTIFY_QUEUE_UI);
    g_loader_state = (xam_dword_fn)resolve_ordinal("xam.xex", XAM_ORD_LOADER_GET_STATE);
    g_process_type = (xam_dword_fn)resolve_ordinal("xboxkrnl.exe", KRNL_ORD_PROCESS_TYPE);
    g_xapi_startup = find_xapi_startup();
    if (!g_queue_ui || !g_loader_state || !g_process_type) return false;
    if (g_xapi_startup)
        xsdk_plugin_log("notify: XapiThreadStartup resolved");
    else
        xsdk_plugin_log("notify: XapiThreadStartup absent; title flag only");
    return true;
}

/* Esta thread pode parar no XAM. A thread que pagina o controle só deixa o texto. */
static uint32_t notify_worker(void *context) {
    (void)context;
    for (;;) {
        HANDLE thread = 0;
        NTSTATUS status;
        int64_t timeout;
        if (!g_notify_busy) {
            sleep_ms(20);
            continue;
        }
        if (!notify_prepare()) {
            if (!g_notify_logged_unavailable) {
                g_notify_logged_unavailable = true;
                xsdk_plugin_log("notify: title thread unavailable");
            }
            g_notify_busy = false;
            continue;
        }
        g_notify_result = 0;
        xsdk_plugin_log("notify: queue type=34 startup=%u", g_xapi_startup ? 1u : 0u);
        status = ExCreateThread(&thread, NOTICE_STACK, NULL, g_xapi_startup, (void *)notify_on_title, NULL,
                                NOTICE_TITLE_EXEC | NOTICE_RETURN_KTHREAD);
        if (!NT_SUCCESS(status) || !thread) {
            xsdk_plugin_log("notify: title thread create failed status=0x%08X", (unsigned)status);
            g_notify_busy = false;
            continue;
        }
        timeout = -((int64_t)NOTICE_WAIT_MS * 10000LL);
        status = KeWaitForSingleObject((void *)(uintptr_t)thread, 0, 0, 0, &timeout);
        if (status == 0)
            xsdk_plugin_log("notify: title thread returned result=%u", (unsigned)g_notify_result);
        else {
            g_notify_stuck = true;
            xsdk_plugin_log("notify: title thread timed out");
        }
        ObDereferenceObject((uint32_t)(uintptr_t)thread);
        g_notify_busy = false;
    }
    return 0;
}

static void notify_worker_start(void) {
    HANDLE thread = 0;
    NTSTATUS status;
    if (g_notify_worker_up) return;
    status = ExCreateThread(&thread, NOTICE_STACK, NULL, NULL, (void *)notify_worker, NULL,
                            NOTICE_SYSTEM | NOTICE_RETURN_KTHREAD);
    if (!NT_SUCCESS(status) || !thread) {
        xsdk_plugin_log("notify: worker failed status=0x%08X", (unsigned)status);
        return;
    }
    ObDereferenceObject((uint32_t)(uintptr_t)thread);
    g_notify_worker_up = true;
}

static bool notify_system(const char *text) {
    if (!text || text[0] == '\0') return true;
    if (g_notify_stuck) return true;
    if (g_notify_busy) return false;
    notice_write(text);
    utf8_to_wide(text, g_notice_wide, sizeof g_notice_wide / sizeof g_notice_wide[0]);
    notify_worker_start();
    if (!g_notify_worker_up) return false;
    g_notify_busy = true;
    return true;
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
#define XAM_ORD_INPUT_GET_KEYSTROKE       403u /* tela nativa; corpo curto, copiado inteiro */
#define XAM_ORD_INPUT_GET_KEYSTROKE_HUD   409u /* painel do guia, só se o prólogo for seguro */
#define XAM_ORD_SEND_XENON_BUTTON        1286u /* xkelib _xamext.h: abre o painel do Xbox */

/* XInputdReadState(deviceContext, packetNumber, gamepad, unk), main.cpp:1365. */
typedef uint32_t (*xinputd_read_state_fn)(uint32_t context, uint32_t *packet, INPUT_GAMEPAD *gamepad,
                                          uint32_t *unk);
/* XamInputGetCapabilitiesEx(unk, user, flags, caps), main.cpp:1330. */
typedef uint32_t (*xam_get_capabilities_ex_fn)(uint32_t unk, uint32_t user, uint32_t flags,
                                               x360_pad_caps_t *caps);
/* XamInputSetState(user, flags, vibration): kernel.h; main.cpp:1307 declares it with extra arguments. */
typedef uint32_t (*xam_set_state_fn)(uint32_t user, uint32_t flags, const INPUT_VIBRATION *vibration);
typedef uint32_t (*xam_keystroke_fn)(uint32_t user, uint32_t flags, x360_keystroke_t *key);

_Static_assert(sizeof(INPUT_GAMEPAD) == sizeof(x360_gamepad_state_t), "gamepad layout");

enum { HOOK_READ_STATE, HOOK_CAPABILITIES, HOOK_SET_STATE, HOOK_KEYSTROKE, HOOK_KEYSTROKE_HUD,
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
#define DS4_BIND_INDEX                      0u
#define DS4_BIND_INDEX_2                    1u

typedef int (*xam_user_bind_device_callback_fn)(uint32_t controller_id, uint32_t context,
                                                uint8_t category, uint32_t disconnect,
                                                uint8_t *user_index);

static xam_user_bind_device_callback_fn g_bind_device;
static bool g_layout_17559;           /* kernel 17559 and the retail layout guard of the USB HAL passed */
static bool g_pad_bound;              /* the DS4 context is registered in XAM */
static uint8_t g_pad_bound_player;
static uint8_t g_pad_user = 0xFFu;
static bool g_guide_was_down;
static bool g_guide_latched; /* PS abriu o painel; o segundo toque fecha. Sem consultar o XAM. */
static uint32_t g_guide_last_ms;
typedef void (*xenon_press_fn)(uint32_t user_index);
static xenon_press_fn g_xenon_press;
static bool g_pad_bind_refused;       /* this authentication already tried and was refused */
static bool g_pad_logged_layout;
static bool g_pad_logged_empty;

static x360_pad_inject_t g_pad2;
static x360_gamepad_state_t g_pad2_report;
static bool             g_pad2_ps;
static uint8_t          g_pad2_player;
static volatile uint32_t g_pad2_context;
static bool             g_pad2_bound;
static uint8_t          g_pad2_bound_player;
static uint8_t          g_pad2_user = 0xFFu;
static bool             g_pad2_bind_refused;
static bool             g_pad2_hid;
static bool             g_pad2_logged_no_player;

static ppc_detour_t g_detours[HOOK_COUNT];
static xinputd_read_state_fn      g_orig_read_state;
static xam_get_capabilities_ex_fn g_orig_capabilities;
static xam_set_state_fn           g_orig_set_state;
static xam_keystroke_fn           g_orig_keystroke;
static xam_keystroke_fn           g_orig_keystroke_hud;
static x360_key_tracker_t         g_key_tracker;
/* Trampolines are code the plugin writes: they live in .text, like hiddriver's ".text" Detour buffer. */
__asm__(".pushsection .text.bt360_trampolines,\"ax\",@progbits\n"
        ".balign 128\n"
        ".globl bt360_trampolines\n"
        "bt360_trampolines:\n"
        ".space 680\n"            /* HOOK_COUNT * PPC_DETOUR_TRAMPOLINE_WORDS * 4 */
        ".popsection\n");
extern uint32_t bt360_trampolines[];
#define g_trampolines bt360_trampolines
_Static_assert(HOOK_COUNT * PPC_DETOUR_TRAMPOLINE_WORDS * 4u == 680u, "trampoline buffer size");

/* A tela de perfil só lê o jogador 1. O oficial fica nessa leitura e cada DS4 soma. */
static uint32_t g_player1_context;
static x360_gamepad_state_t g_swap_last;

static void bump_packet(uint32_t *packet, x360_gamepad_state_t *last, const x360_gamepad_state_t *now) {
    if (!packet || memcmp(last, now, sizeof *now) == 0) return;
    *last = *now;
    *packet += 1u;
}

static uint32_t hook_read_state(uint32_t context, uint32_t *packet, INPUT_GAMEPAD *gamepad,
                                uint32_t *unk) {
    const bool is0 = g_pad_device_context != 0u && context == g_pad_device_context;
    const bool is1 = g_pad2_context != 0u && context == g_pad2_context;
    const uint32_t status = g_orig_read_state(context, packet, gamepad, unk);
    const uint32_t result = is1
        ? x360_pad_inject_filter_read_state(&g_pad2, true, status, packet,
                                            (x360_gamepad_state_t *)gamepad)
        : x360_pad_inject_filter_read_state(&g_pad, is0, status, packet,
                                            (x360_gamepad_state_t *)gamepad);
    if (result == X360_PAD_ERROR_SUCCESS && status != X360_PAD_ERROR_SUCCESS && unk) *unk = 0u;
    /* A tela de perfil só lê o jogador 1. O controle original permanece nessa
     * leitura; cada DualShock ligado soma os botões dele, sem tomar o lugar. */
    if (!gamepad || is0 || is1 || context == 0u) return result;
    if (g_player1_context == 0u || context < g_player1_context) g_player1_context = context;
    if (context != g_player1_context) return result;
    {
        const bool official = status == X360_PAD_ERROR_SUCCESS;
        x360_gamepad_state_t shown = *(x360_gamepad_state_t *)gamepad;
        bool keep = official;
        const x360_pad_snapshot_t first = x360_pad_inject_snapshot(&g_pad);
        const x360_pad_snapshot_t second = x360_pad_inject_snapshot(&g_pad2);
        if (g_pad_bound && first.active) {
            x360_pad_overlay_player1(&shown, &first.state, keep);
            keep = true;
        }
        if (g_pad2_bound && second.active) {
            x360_pad_overlay_player1(&shown, &second.state, keep);
            keep = true;
        }
        if (!keep) return result;
        *(x360_gamepad_state_t *)gamepad = shown;
        bump_packet(packet, &g_swap_last, &shown);
        if (!official && unk) *unk = 0u;
        return X360_PAD_ERROR_SUCCESS;
    }
}

static uint32_t hook_capabilities(uint32_t unk, uint32_t user, uint32_t flags, x360_pad_caps_t *caps) {
    const uint32_t status = g_orig_capabilities(unk, user, flags, caps);
    const uint32_t first = x360_pad_inject_filter_capabilities(&g_pad, user, status, caps);
    if (first != status) return first;
    return x360_pad_inject_filter_capabilities(&g_pad2, user, status, caps);
}

static uint32_t hook_set_state(uint32_t user, uint32_t flags, const INPUT_VIBRATION *vibration) {
    const uint32_t status = g_orig_set_state(user, flags, vibration);
    if (!vibration) return status;
    const uint32_t first = x360_pad_inject_filter_set_state(&g_pad, user, status,
                                                            vibration->left_motor_speed,
                                                            vibration->right_motor_speed);
    if (first != status) return first;
    return x360_pad_inject_filter_set_state(&g_pad2, user, status, vibration->left_motor_speed,
                                            vibration->right_motor_speed);
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

static uint32_t keystroke_from_ds4(uint32_t status, uint32_t user, x360_keystroke_t *key) {
    x360_gamepad_state_t merged;
    bool any = false;
    x360_pad_snapshot_t snap;
    if (!key) return status;
    if (user != 0u && user != 0xFFu) return status;
    memset(&merged, 0, sizeof merged);
    if (g_pad_bound) {
        snap = x360_pad_inject_snapshot(&g_pad);
        if (snap.active) {
            x360_pad_overlay_player1(&merged, &snap.state, false);
            any = true;
        }
    }
    if (g_pad2_bound) {
        snap = x360_pad_inject_snapshot(&g_pad2);
        if (snap.active) {
            x360_pad_overlay_player1(&merged, &snap.state, any);
            any = true;
        }
    }
    if (!any) return status;
    return x360_pad_keystroke_filter(&g_key_tracker, &merged, status, key, 0u, (uint32_t)now_ms());
}

/* O teclado do sistema trata o Y e, no meio disso, pede outra tecla.
 * Entrar de novo no atalho sobrescreve o CTR da chamada que ainda não voltou. */
static volatile uint32_t g_key_busy;

static uint32_t hook_key_guard(xam_keystroke_fn orig, uint32_t user, uint32_t flags,
                               x360_keystroke_t *key) {
    if (__atomic_fetch_add(&g_key_busy, 1u, __ATOMIC_ACQ_REL) != 0u) {
        __atomic_fetch_sub(&g_key_busy, 1u, __ATOMIC_ACQ_REL);
        return X360_PAD_ERROR_EMPTY;
    }
    const uint32_t status = orig ? orig(user, flags, key) : X360_PAD_ERROR_EMPTY;
    const uint32_t out = keystroke_from_ds4(status, user, key);
    __atomic_fetch_sub(&g_key_busy, 1u, __ATOMIC_ACQ_REL);
    return out;
}

static uint32_t hook_keystroke(uint32_t user, uint32_t flags, x360_keystroke_t *key) {
    return hook_key_guard(g_orig_keystroke, user, flags, key);
}

static uint32_t hook_keystroke_hud(uint32_t user, uint32_t flags, x360_keystroke_t *key) {
    return hook_key_guard(g_orig_keystroke_hud, user, flags, key);
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

/* O atalho de 403 tem dez instruções e usa o CTR. Copiar o corpo inteiro
 * deixa o CTR e o LR do teclado intactos. */
static bool hook_install_body(const char *name, HANDLE module, uint32_t ordinal, unsigned index,
                              const void *hook, void **original) {
    void *target = NULL;
    const NTSTATUS resolved = module ? XexGetProcedureAddress(module, ordinal, &target) : STATUS_UNSUCCESSFUL;
    size_t used = 0;
    uint32_t *tramp;
    if (!NT_SUCCESS(resolved) || !target || !MmIsAddressValid(target)) {
        xsdk_plugin_log("pad: %s ordinal %u not resolved (status=0x%08X); hook not installed", name,
                        (unsigned)ordinal, (unsigned)resolved);
        return false;
    }
    tramp = &g_trampolines[index * PPC_DETOUR_TRAMPOLINE_WORDS];
    *original = tramp;
    if (!ppc_detour_install_body(&g_detours[index], (uint32_t *)target, (uint32_t)(uintptr_t)target,
                                 (uint32_t)(uintptr_t)hook, tramp, (uint32_t)(uintptr_t)tramp,
                                 PPC_DETOUR_TRAMPOLINE_WORDS, &used, code_flush)) {
        *original = NULL;
        xsdk_plugin_log("pad: %s ordinal %u at 0x%08X: body detour refused", name, (unsigned)ordinal,
                        (unsigned)(uintptr_t)target);
        return false;
    }
    xsdk_plugin_log("pad: %s ordinal %u at 0x%08X body hooked, trampoline %u words", name,
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
    hook_install_body("XamInputGetKeystroke", xam, XAM_ORD_INPUT_GET_KEYSTROKE, HOOK_KEYSTROKE,
                      (const void *)hook_keystroke, (void **)&g_orig_keystroke);
    hook_install("XamInputGetKeystrokeHud", xam, XAM_ORD_INPUT_GET_KEYSTROKE_HUD, HOOK_KEYSTROKE_HUD,
                 (const void *)hook_keystroke_hud, (void **)&g_orig_keystroke_hud);
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
    const int slot = ctx == (void *)1 ? 1 : 0;
    if (bt_hid_link_output(slot, report, len) != 0) {
        static bool logged;
        if (!logged) {
            logged = true;
            xsdk_plugin_log("pad: rumble frame has no open HID channel");
        }
    }
}

/* Tira o DS4 da lista do XAM. O estado publicado continua, para a navegação do painel. */
static void pad_release_xam(void) {
    g_pad_device_context = 0u;
    if (g_pad_bound && g_bind_device) {
        g_bind_device(x360_pad_controller_id(DS4_BIND_INDEX), x360_pad_device_context(DS4_BIND_INDEX),
                      0u, 1u, NULL);
        xsdk_plugin_log("pad: DS4 context unbound from XAM (player %u)", (unsigned)g_pad_bound_player);
    }
    g_pad_bound = false;
    g_pad_bound_player = 0u;
    g_pad_user = 0xFFu;
}

/* "Down": removes the DS4 context from XAM and clears only the DS4's own state. */
static void pad_unbind(void) {
    pad_release_xam();
    x360_pad_inject_drop(&g_pad);
    g_guide_was_down = false;
    g_guide_latched = false;
    g_guide_last_ms = 0u;
    g_player1_context = 0u;
    memset(&g_swap_last, 0, sizeof g_swap_last);
}

/* O jogador é o usuário que o XAM entregou. O usuário 0 fica com o controle oficial. */
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
    const uint8_t other = g_pad2_bound ? g_pad2_user : 0xFFu;
    const uint8_t adopted = bt_player_for_bound_user(pair_official_on_player1(NULL), user, other);
    x360_pad_bind_verdict_t verdict;
    if (adopted != 0u) {
        player = adopted;
        verdict = X360_PAD_BIND_OK;
    } else if (user == 0u) {
        verdict = X360_PAD_BIND_OFFICIAL_SLOT;
    } else {
        verdict = x360_pad_inject_check_bind(player, user);
    }
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
    g_pad_player = player;
    g_pad_user = user;
    g_pad_device_context = context;
    if (g_session.player != player) {
        g_session.player = player;
        if (g_session.has_peer && bt_manager_set_player(&g_ipc, g_session.peer, player) >= 0)
            pair_persist(NULL, &g_ipc);
        xsdk_plugin_log("pad: auto player %u", (unsigned)player);
    }
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
            bt_hid_link_stop(0);
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
        if (bt_hid_link_start(0, g_session.acl_handle) == 0)
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
    if (x360_pad_in_use(&g_pad_report)) bt_pair_session_note_use(&g_session, 0, now_ms());
    /* O painel visível é o do perfil do controle oficial (usuário 0), não o do jogador 2. */
    const bool guide_down = g_pad_ps || g_pad2_ps;
    const uint32_t now = (uint32_t)now_ms();
    if (guide_down && g_xenon_press &&
        (!g_guide_was_down || now - g_guide_last_ms >= 1000u)) {
        if (!g_guide_was_down) {
            g_guide_latched = !g_guide_latched;
            xsdk_plugin_log("pad: guide press user=0 latched=%u", g_guide_latched ? 1u : 0u);
        }
        g_guide_last_ms = now;
        g_xenon_press(0u);
    }
    g_guide_was_down = guide_down;
}

static void pad2_unbind(void) {
    g_pad2_context = 0u;
    if (g_pad2_bound && g_bind_device) {
        g_bind_device(x360_pad_controller_id(DS4_BIND_INDEX_2),
                      x360_pad_device_context(DS4_BIND_INDEX_2), 0u, 1u, NULL);
        xsdk_plugin_log("pad: second DS4 unbound (player %u)", (unsigned)g_pad2_bound_player);
    }
    g_pad2_bound = false;
    g_pad2_bound_player = 0u;
    g_pad2_user = 0xFFu;
    x360_pad_inject_drop(&g_pad2);
}

static bool pad2_bind(uint8_t player) {
    uint8_t user = X360_PAD_USER_INDEX_NONE;
    const uint32_t context = x360_pad_device_context(DS4_BIND_INDEX_2);
    const int result = g_bind_device(x360_pad_controller_id(DS4_BIND_INDEX_2), context, 0u, 0u, &user);
    const uint8_t other = g_pad_bound ? g_pad_user : 0xFFu;
    const uint8_t adopted = bt_player_for_bound_user(pair_official_on_player1(NULL), user, other);
    x360_pad_bind_verdict_t verdict;
    if (adopted != 0u) {
        player = adopted;
        verdict = X360_PAD_BIND_OK;
    } else if (user == 0u) {
        verdict = X360_PAD_BIND_OFFICIAL_SLOT;
    } else {
        verdict = x360_pad_inject_check_bind(player, user);
    }
    xsdk_plugin_log("pad: second bind result=%d userIndex=%u player=%u verdict=%u", result,
                    (unsigned)user, (unsigned)player, (unsigned)verdict);
    if (verdict != X360_PAD_BIND_OK) {
        g_bind_device(x360_pad_controller_id(DS4_BIND_INDEX_2), context, 0u, 1u, NULL);
        return false;
    }
    g_pad2_bound = true;
    g_pad2_bound_player = player;
    g_pad2_player = player;
    g_pad2_user = user;
    g_pad2_context = context;
    if (g_session.second.player != player) {
        g_session.second.player = player;
        if (bt_manager_set_player(&g_ipc, g_session.second.peer, player) >= 0)
            pair_persist(NULL, &g_ipc);
        xsdk_plugin_log("pad: auto player %u", (unsigned)player);
    }
    xsdk_plugin_log("pad: second DS4 context 0x%08X bound on user %u", (unsigned)context,
                    (unsigned)user);
    return true;
}

static void pad2_service(void) {
    const bool up = g_session.second.established;
    if (g_session.second.player >= X360_PAD_PLAYER_MIN &&
        g_session.second.player <= X360_PAD_PLAYER_MAX)
        g_pad2_player = g_session.second.player;
    if (!up) {
        if (g_pad2_hid) {
            bt_hid_link_stop(1);
            g_pad2_hid = false;
        }
        pad2_unbind();
        g_pad2_logged_no_player = false;
        g_pad2_bind_refused = false;
        return;
    }
    if (!g_pad2_hid && g_session.second.encrypted &&
        g_session.second.acl_handle != 0 && g_session.second.acl_handle != 0xFFFFu) {
        if (bt_hid_link_start(1, g_session.second.acl_handle) == 0) g_pad2_hid = true;
    }
    if (!g_layout_17559 || !g_bind_device) return;
    if (g_pad2_player == 0u) {
        if (!g_pad2_logged_no_player) {
            g_pad2_logged_no_player = true;
            xsdk_plugin_log("pad: second DS4 authenticated but no player; not publishing");
        }
        return;
    }
    if (g_pad2_bound && g_pad2_bound_player != g_pad2_player) {
        pad2_unbind();
        g_pad2_bind_refused = false;
    }
    if (!g_pad2_bound) {
        if (g_pad2_bind_refused) return;
        if (!pad2_bind(g_pad2_player)) {
            g_pad2_bind_refused = true;
            return;
        }
    }
    x360_pad_inject_publish(&g_pad2, true, g_pad2_player, &g_pad2_report, g_pad2_ps);
    if (x360_pad_in_use(&g_pad2_report)) bt_pair_session_note_use(&g_session, 1, now_ms());
}

static void pad_setup(void) {
    const x360_pad_sink_t sink0 = { .ctx = (void *)0, .write_slot = pad_write_slot,
                                    .send_report = pad_send_report };
    const x360_pad_sink_t sink1 = { .ctx = (void *)1, .write_slot = pad_write_slot,
                                    .send_report = pad_send_report };
    x360_pad_inject_init(&g_pad, &sink0);
    x360_pad_inject_init(&g_pad2, &sink1);
    memset(&g_pad_report, 0, sizeof g_pad_report);
    memset(&g_pad2_report, 0, sizeof g_pad2_report);
    bt_hid_link_bind(0, &g_pad_report, &g_pad_ps);
    bt_hid_link_bind(1, &g_pad2_report, &g_pad2_ps);
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
    pad2_service();
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
                            const bt_paired_device_t *primary;
                            const bt_paired_device_t *second;
                            pair_persist(NULL, &g_ipc);
                            primary = g_session.has_peer
                                          ? bt_manager_find_paired(&g_ipc, g_session.peer) : NULL;
                            second = g_session.second.active
                                         ? bt_manager_find_paired(&g_ipc, g_session.second.peer)
                                         : NULL;
                            if (g_session.has_peer &&
                                memcmp(request.address, g_session.peer, 6u) == 0)
                                g_session.player = (uint8_t)request.slot;
                            else if (primary && primary->player >= 1u && primary->player <= 4u)
                                g_session.player = primary->player;
                            if (g_session.second.active &&
                                memcmp(request.address, g_session.second.peer, 6u) == 0)
                                g_session.second.player = (uint8_t)request.slot;
                            else if (second && second->player >= 1u && second->player <= 4u)
                                g_session.second.player = second->player;
                            xsdk_plugin_log("pad: players now %u and %u",
                                            (unsigned)g_session.player,
                                            (unsigned)g_session.second.player);
                        }
                    } else if (seen)
                        bt_pair_session_connect_known(&g_session, seen, now_ms());
                    else {
                        const uint8_t *cmd_addr = has_address ? request.address : NULL;
                        uint8_t slot_addr[6];
                        if (request.command == BT_IPC_CMD_DISCONNECT_SLOT && !has_address &&
                            request.slot < 4u) {
                            for (size_t i = 0; i < BT_MAX_PAIRED_DEVICES; i++) {
                                if (!g_ipc.paired[i].valid) continue;
                                if (g_ipc.paired[i].player != (uint8_t)(request.slot + 1u)) continue;
                                memcpy(slot_addr, g_ipc.paired[i].bd_addr, 6u);
                                cmd_addr = slot_addr;
                                break;
                            }
                        }
                        bt_pair_session_on_command(&g_session, request.command, cmd_addr, now_ms());
                    }
                }
            }
            /* Console sees the DS4 event while it lasts; the adapter phrase otherwise. */
            const bool ds4_event = g_session.phrase_id != BT_PHRASE_NONE;
            uint8_t link_a = BT_LINK_DOWN;
            uint8_t link_b = BT_LINK_DOWN;
            uint8_t packed_player = 0;
            uint8_t packed_link = 0;
            if (g_session.state == BT_PAIR_CONNECTED && g_session.established)
                link_a = BT_LINK_UP;
            else if (g_session.state == BT_PAIR_CONNECTING || g_session.state == BT_PAIR_AUTH)
                link_a = BT_LINK_CONNECTING;
            if (g_session.second.established)
                link_b = BT_LINK_UP;
            else if (g_session.second.state == BT_PAIR_CONNECTING ||
                     g_session.second.state == BT_PAIR_AUTH)
                link_b = BT_LINK_CONNECTING;
            bt_manager_pack_links(&packed_player, &packed_link, g_session.player, link_a,
                                  g_session.second.player, link_b);
            uint8_t share_player = 0u;
            if ((g_pad_report.buttons & X360_BTN_BACK) != 0 &&
                g_session.state == BT_PAIR_CONNECTED && g_session.established &&
                g_session.player >= 1u && g_session.player <= 4u) {
                share_player = g_session.player;
            } else if ((g_pad2_report.buttons & X360_BTN_BACK) != 0 &&
                       g_session.second.established &&
                       g_session.second.player >= 1u && g_session.second.player <= 4u) {
                share_player = g_session.second.player;
            }
            const bt_console_reply_t reply = {
                .magic = BT_IPC_MAGIC,
                .version = BT_IPC_VERSION,
                .token = request.token,
                .usb_status = rc,
                .adapter_stage = shown,
                .phrase_id = ds4_event ? g_session.phrase_id : phrase_id,
                .player = packed_player != 0u ? packed_player : player,
                .pair_active = g_session.pair_active ? 1u : 0u,
                .link = packed_link,
                .reserved = share_player
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
