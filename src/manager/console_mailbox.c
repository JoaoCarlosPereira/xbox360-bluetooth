#include "console_mailbox.h"
#include <xsdk/kernel.h>

_Static_assert(sizeof(bt_console_request_t) == 32, "Request file layout");
_Static_assert(sizeof(bt_console_reply_t) == 32, "Reply file layout");

#define REQUEST_FILE "\\Device\\Mass0\\bt360_request.bin"
#define REPLY_FILE "\\Device\\Mass0\\bt360_reply.bin"

static bool transfer(const char *path, uint16_t path_size, void *output,
                     const void *input, uint32_t size) {
    const ANSI_STRING name = { path_size - 1, path_size, path };
    const OBJECT_ATTRIBUTES attributes = { .name = &name };
    HANDLE file;
    IO_STATUS_BLOCK io = { 0 };
    NTSTATUS status = NtCreateFile(&file, SYNCHRONIZE | (input ? GENERIC_WRITE : GENERIC_READ),
                                   &attributes, &io, NULL, FILE_ATTRIBUTE_NORMAL, 0,
                                   input ? FILE_OPEN_IF : FILE_OPEN,
                                   FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE);
    if (!NT_SUCCESS(status)) return false;
    if (input) {
        /* Fixed-size records can be replaced without first truncating the file. */
        const uint64_t offset = 0;
        status = NtWriteFile(file, 0, NULL, NULL, &io, input, size, &offset);
    } else {
        status = NtReadFile(file, 0, NULL, NULL, &io, output, size, NULL);
    }
    bool complete = NT_SUCCESS(status) && io.information == size;
    return NT_SUCCESS(NtClose(file)) && complete;
}

bool bt_console_read_request(bt_console_request_t *request) {
    return transfer(REQUEST_FILE, sizeof REQUEST_FILE, request, NULL, sizeof *request) &&
           request->magic == BT_IPC_MAGIC && request->version == BT_IPC_VERSION &&
           request->token != 0 && request->command <= BT_IPC_CMD_FORGET_PAIRED;
}

bool bt_console_write_request(const bt_console_request_t *request) {
    return transfer(REQUEST_FILE, sizeof REQUEST_FILE, NULL, request, sizeof *request);
}

bool bt_console_read_reply(bt_console_reply_t *reply) {
    return transfer(REPLY_FILE, sizeof REPLY_FILE, reply, NULL, sizeof *reply) &&
           reply->magic == BT_IPC_MAGIC && reply->version == BT_IPC_VERSION;
}

bool bt_console_write_reply(const bt_console_reply_t *reply) {
    return transfer(REPLY_FILE, sizeof REPLY_FILE, NULL, reply, sizeof *reply);
}
