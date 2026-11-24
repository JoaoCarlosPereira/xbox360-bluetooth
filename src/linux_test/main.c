#include "../hci/hci_core.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/* Linux kernel Bluetooth HCI socket ABI constants (avoids libbluetooth-dev dependency) */
#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH 31
#endif
#define BTPROTO_HCI     1
#define SOL_HCI         0
#define HCI_OPT_FILTER  2
#define HCI_CHANNEL_RAW 0

#define HCI_H4_CMD_PKT  0x01u
#define HCI_H4_EVT_PKT  0x04u

struct sockaddr_hci {
    sa_family_t    hci_family;
    unsigned short hci_dev;
    unsigned short hci_channel;
};

struct hci_filter {
    uint32_t type_mask;
    uint32_t event_mask[2];
    uint16_t opcode;
};

static int send_cmd_wait_cc(int fd, uint16_t opcode, hci_cmd_complete_t *out_cc,
                            uint8_t *evt_storage, size_t evt_cap) {
    uint8_t tx[1 + HCI_MAX_CMD_SIZE];
    tx[0] = HCI_H4_CMD_PKT;

    size_t cmd_len = hci_build_command(tx + 1, sizeof(tx) - 1, opcode, NULL, 0);
    if (cmd_len == 0) {
        return -1;
    }

    struct hci_filter flt;
    memset(&flt, 0, sizeof(flt));
    flt.type_mask = (1u << HCI_H4_EVT_PKT);
    flt.event_mask[0] = (1u << HCI_EVT_COMMAND_COMPLETE) | (1u << HCI_EVT_COMMAND_STATUS);
    flt.opcode = opcode;
    if (setsockopt(fd, SOL_HCI, HCI_OPT_FILTER, &flt, sizeof(flt)) < 0) {
        perror("setsockopt(HCI_FILTER)");
        return -2;
    }

    if (write(fd, tx, 1 + cmd_len) < 0) {
        perror("write(HCI cmd)");
        return -3;
    }

    for (int tries = 0; tries < 10; tries++) {
        ssize_t n = read(fd, evt_storage, evt_cap);
        if (n < 0) {
            perror("read(HCI evt)");
            return -4;
        }
        if (n < 2 || evt_storage[0] != HCI_H4_EVT_PKT) {
            continue;
        }
        if (hci_parse_command_complete(evt_storage + 1, (size_t)(n - 1), out_cc) == 0) {
            if (out_cc->opcode == opcode) {
                return 0;
            }
        }
    }
    return -5;
}

int main(void) {
    int fd = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI);
    if (fd < 0) {
        perror("socket(AF_BLUETOOTH, BTPROTO_HCI)");
        return 1;
    }

    struct timeval tv = { 2, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_hci addr;
    memset(&addr, 0, sizeof(addr));
    addr.hci_family  = AF_BLUETOOTH;
    addr.hci_dev     = 0; /* hci0 */
    addr.hci_channel = HCI_CHANNEL_RAW;

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "bind(hci0) failed: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    uint8_t evt_buf[1 + HCI_MAX_EVT_SIZE];
    hci_cmd_complete_t cc;

    if (send_cmd_wait_cc(fd, HCI_OP_READ_LOCAL_VERSION_INFO, &cc, evt_buf, sizeof(evt_buf)) == 0) {
        hci_local_version_info_t ver;
        if (hci_decode_local_version(&cc, &ver) == 0) {
            printf("HCI_Read_Local_Version_Information (0x1001):\n");
            printf("  Status       : 0x%02X\n", ver.status);
            printf("  HCI Version  : 0x%02X (Bluetooth %s)\n", ver.hci_version, hci_version_str(ver.hci_version));
            printf("  HCI Revision : 0x%04X\n", ver.hci_revision);
            printf("  LMP Version  : 0x%02X\n", ver.lmp_version);
            printf("  Manufacturer : 0x%04X (%u)\n", ver.manufacturer, ver.manufacturer);
            printf("  LMP Subver   : 0x%04X\n", ver.lmp_subversion);
        }
    } else {
        fprintf(stderr, "Failed to query HCI_Read_Local_Version_Information\n");
    }

    if (send_cmd_wait_cc(fd, HCI_OP_READ_BD_ADDR, &cc, evt_buf, sizeof(evt_buf)) == 0) {
        hci_bd_addr_info_t bd;
        if (hci_decode_bd_addr(&cc, &bd) == 0) {
            printf("HCI_Read_BD_ADDR (0x1009):\n");
            printf("  Status       : 0x%02X\n", bd.status);
            printf("  BD_ADDR      : %02X:%02X:%02X:%02X:%02X:%02X\n",
                   bd.bd_addr[5], bd.bd_addr[4], bd.bd_addr[3],
                   bd.bd_addr[2], bd.bd_addr[1], bd.bd_addr[0]);
        }
    } else {
        fprintf(stderr, "Failed to query HCI_Read_BD_ADDR\n");
    }

    close(fd);
    return 0;
}
