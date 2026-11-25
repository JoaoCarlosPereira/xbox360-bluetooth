#ifndef XBOX360_BT_HCI_CORE_H
#define XBOX360_BT_HCI_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Standard HCI Opcodes (OGF << 10 | OCF) */
#define HCI_OP_INQUIRY                  0x0401u
#define HCI_OP_INQUIRY_CANCEL           0x0402u
#define HCI_OP_CREATE_CONNECTION        0x0405u
#define HCI_OP_LINK_KEY_REQ_REPLY       0x040Bu
#define HCI_OP_LINK_KEY_REQ_NEG_REPLY   0x040Cu
#define HCI_OP_IO_CAP_REQ_REPLY         0x042Bu
#define HCI_OP_USER_CONFIRM_REQ_REPLY   0x042Cu
#define HCI_OP_SET_EVENT_MASK           0x0C01u
#define HCI_OP_RESET                    0x0C03u
#define HCI_OP_WRITE_SCAN_ENABLE        0x0C1Au
#define HCI_OP_WRITE_INQUIRY_MODE       0x0C45u
#define HCI_OP_WRITE_SIMPLE_PAIRING     0x0C56u
#define HCI_OP_READ_LOCAL_VERSION_INFO  0x1001u
#define HCI_OP_READ_BD_ADDR             0x1009u

/* Standard HCI Event Codes */
#define HCI_EVT_INQUIRY_COMPLETE        0x01u
#define HCI_EVT_INQUIRY_RESULT          0x02u
#define HCI_EVT_CONNECTION_COMPLETE     0x03u
#define HCI_EVT_DISCONNECTION_COMPLETE  0x05u
#define HCI_EVT_COMMAND_COMPLETE        0x0Eu
#define HCI_EVT_COMMAND_STATUS          0x0Fu
#define HCI_EVT_HARDWARE_ERROR          0x10u
#define HCI_EVT_LINK_KEY_REQUEST        0x17u
#define HCI_EVT_LINK_KEY_NOTIFICATION   0x18u
#define HCI_EVT_INQUIRY_RESULT_RSSI     0x22u
#define HCI_EVT_EXTENDED_INQUIRY_RESULT 0x2Fu
#define HCI_EVT_IO_CAPABILITY_REQUEST   0x31u
#define HCI_EVT_USER_CONFIRM_REQUEST    0x33u

#define HCI_CMD_HDR_SIZE                3u
#define HCI_EVT_HDR_SIZE                2u
#define HCI_MAX_CMD_SIZE                258u
#define HCI_MAX_EVT_SIZE                257u

typedef struct {
    uint8_t  status;
    uint8_t  hci_version;
    uint16_t hci_revision;
    uint8_t  lmp_version;
    uint16_t manufacturer;
    uint16_t lmp_subversion;
} hci_local_version_info_t;

typedef struct {
    uint8_t  status;
    uint8_t  bd_addr[6]; /* Little-endian wire order (LSB first) */
} hci_bd_addr_info_t;

typedef struct {
    uint8_t  num_hci_cmd_packets;
    uint16_t opcode;
    const uint8_t *params;
    uint8_t  params_len;
} hci_cmd_complete_t;

typedef struct {
    uint8_t  status;
    uint8_t  num_hci_cmd_packets;
    uint16_t opcode;
} hci_cmd_status_t;

typedef struct {
    uint8_t  bd_addr[6];
    uint8_t  page_scan_rep_mode;
    uint32_t class_of_device;
    uint16_t clock_offset;
    int8_t   rssi;
    bool     has_rssi;
    char     name[32];
} hci_inquiry_item_t;

static inline void hci_put_le16(uint8_t *dst, uint16_t val) {
    dst[0] = (uint8_t)(val & 0xFFu);
    dst[1] = (uint8_t)((val >> 8) & 0xFFu);
}

static inline uint16_t hci_get_le16(const uint8_t *src) {
    return (uint16_t)src[0] | ((uint16_t)src[1] << 8);
}

/**
 * Build a raw HCI Command packet (without H4 packet type prefix, suitable for USB Control EP0).
 * Returns total byte length written to out_buf, or 0 on error.
 */
size_t hci_build_command(uint8_t *out_buf, size_t out_cap,
                         uint16_t opcode, const uint8_t *params, uint8_t param_len);

/**
 * Build HCI_Link_Key_Request_Reply (0x040B): BD_ADDR (wire order) + 16-byte Link Key.
 * Returns total byte length written to out_buf, or 0 on error.
 */
size_t hci_build_link_key_reply(uint8_t *out_buf, size_t out_cap,
                                const uint8_t bd_addr[6], const uint8_t link_key[16]);

/**
 * Parse a raw HCI Command Complete event (starting at event_code 0x0E).
 * Returns 0 on success, negative on malformed/non-matching event.
 */
int hci_parse_command_complete(const uint8_t *evt_buf, size_t evt_len,
                               hci_cmd_complete_t *out_cc);

/**
 * Decode payload of HCI_Read_Local_Version_Information (0x1001) Command Complete.
 * Returns 0 on success, negative on invalid length.
 */
int hci_decode_local_version(const hci_cmd_complete_t *cc,
                             hci_local_version_info_t *out_info);

/**
 * Decode payload of HCI_Read_BD_ADDR (0x1009) Command Complete.
 * Returns 0 on success, negative on invalid length.
 */
int hci_decode_bd_addr(const hci_cmd_complete_t *cc,
                       hci_bd_addr_info_t *out_addr);

/**
 * Parse a raw HCI Command Status event (0x0F).
 */
int hci_parse_command_status(const uint8_t *evt_buf, size_t evt_len,
                             hci_cmd_status_t *out_cs);

/**
 * Decode Inquiry Result (0x02), Inquiry Result with RSSI (0x22), or
 * Extended Inquiry Result (0x2F) into up to max_items entries.
 * Returns number of decoded items (>= 0), or negative on error.
 */
int hci_decode_inquiry_event(const uint8_t *evt_buf, size_t evt_len,
                             hci_inquiry_item_t *out_items, size_t max_items);

/**
 * Return human-readable Bluetooth Core version string for HCI_Version byte.
 */
const char *hci_version_str(uint8_t hci_version);

#ifdef __cplusplus
}
#endif

#endif /* XBOX360_BT_HCI_CORE_H */
