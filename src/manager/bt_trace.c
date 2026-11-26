#include "bt_trace.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <xsdk/kernel.h>

/* 2 MB por arquivo: cabe uma sessão inteira e não enche o disco. */
#define TRACE_LIMIT (2u * 1024u * 1024u)
#define TRACE_LINE 384

static const char *const k_paths[] = {
    "\\Device\\Harddisk0\\Partition1\\bt360_trace.log",
    "\\Device\\Mass0\\bt360_trace.log",
    "\\Device\\Mass0\\bt360_diag.log",
};

static bool k_ready;
static uint8_t k_use;
static uint8_t k_full;

static uint16_t path_length(const char *path) {
    uint16_t n = 0;
    while (path[n]) n++;
    return n;
}

static NTSTATUS append_line(const char *path, const char *line, uint32_t length) {
    const uint16_t bytes = path_length(path);
    const ANSI_STRING name = { .length = bytes, .maximum_length = (uint16_t)(bytes + 1), .pointer = path };
    const OBJECT_ATTRIBUTES attributes = { .name = &name };
    IO_STATUS_BLOCK io = { 0 };
    HANDLE file;
    NTSTATUS status = NtCreateFile(&file, GENERIC_WRITE | SYNCHRONIZE, &attributes, &io, NULL,
                                   FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, FILE_OPEN_IF,
                                   FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE);
    if (!NT_SUCCESS(status)) return status;
    FILE_NETWORK_OPEN_INFORMATION info;
    status = NtQueryInformationFile(file, &io, &info, sizeof info, FILE_NETWORK_OPEN_INFORMATION_CLASS);
    if (NT_SUCCESS(status) && info.end_of_file < TRACE_LIMIT)
        status = NtWriteFile(file, 0, NULL, NULL, &io, line, length, &info.end_of_file);
    else if (NT_SUCCESS(status))
        status = STATUS_DISK_FULL;
    const NTSTATUS closed = NtClose(file);
    return NT_SUCCESS(status) ? closed : status;
}

static void open_paths(void) {
    if (k_ready) return;
    k_ready = true;
    for (uint8_t i = 0; i < sizeof k_paths / sizeof k_paths[0]; i++) {
        const uint16_t bytes = path_length(k_paths[i]);
        const ANSI_STRING name = { .length = bytes, .maximum_length = (uint16_t)(bytes + 1), .pointer = k_paths[i] };
        const OBJECT_ATTRIBUTES attributes = { .name = &name };
        IO_STATUS_BLOCK io = { 0 };
        HANDLE file;
        if (NT_SUCCESS(NtCreateFile(&file, GENERIC_WRITE | SYNCHRONIZE, &attributes, &io, NULL,
                                    FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, FILE_OPEN_IF,
                                    FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE))) {
            NtClose(file);
            k_use |= (uint8_t)(1u << i);
        }
    }
}

void bt_trace(const char *format, ...) {
    open_paths();
    if (!k_use) return;
    uint64_t now = 0;
    KeQuerySystemTime(&now);
    char line[TRACE_LINE];
    const unsigned stamp = (unsigned)(now / 10000u);
    for (int nibble = 7; nibble >= 0; nibble--)
        line[7 - nibble] = "0123456789ABCDEF"[(stamp >> (nibble * 4)) & 0xF];
    line[8] = ' ';
    const int head = 9;
    const uint32_t room = (uint32_t)sizeof line - 1u;
    va_list args;
    va_start(args, format);
    int body = _vsnprintf(line + head, room - (uint32_t)head, format, args);
    va_end(args);
    if (body < 0) body = 0;
    if ((uint32_t)body >= room - (uint32_t)head) body = (int)(room - (uint32_t)head - 1u);
    uint32_t length = (uint32_t)head + (uint32_t)body;
    while (length > 0 && line[length - 1] == '\n') length--;
    line[length++] = '\n';
    line[length] = 0;
    for (uint8_t i = 0; i < sizeof k_paths / sizeof k_paths[0]; i++) {
        if ((k_use & (uint8_t)(1u << i)) == 0 || (k_full & (uint8_t)(1u << i)) != 0) continue;
        const NTSTATUS status = append_line(k_paths[i], line, length);
        if (status == STATUS_DISK_FULL) k_full |= (uint8_t)(1u << i);
    }
}
