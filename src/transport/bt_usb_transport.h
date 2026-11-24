#ifndef XBOX360_BT_USB_TRANSPORT_H
#define XBOX360_BT_USB_TRANSPORT_H

#include "../hci/hci_core.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Standard USB Bluetooth HCI class/subclass/protocol */
#define USB_CLASS_WIRELESS_CONTROLLER   0xE0u
#define USB_SUBCLASS_RF_CONTROLLER      0x01u
#define USB_PROTOCOL_BLUETOOTH_HCI      0x01u

/* USB Transfer Types */
#define BT_USB_EP_TYPE_CONTROL          0x00u
#define BT_USB_EP_TYPE_ISOCHRONOUS      0x01u
#define BT_USB_EP_TYPE_BULK             0x02u
#define BT_USB_EP_TYPE_INTERRUPT        0x03u

#define BT_USB_DIR_OUT                  0x00u
#define BT_USB_DIR_IN                   0x01u

/* USB Class-Specific Request for HCI Command on EP0 */
#define BT_USB_REQ_TYPE_HCI_CMD         0x20u /* Host-to-Device | Class | Interface */
#define BT_USB_REQ_HCI_CMD              0x00u

typedef struct {
    uint8_t  bEndpointAddress;
    uint8_t  bmAttributes;
    uint16_t wMaxPacketSize; /* Already host-endian & masked (& 0x7FF) */
    uint8_t  bInterval;
} bt_usb_ep_info_t;

typedef struct {
    uint16_t         vid;
    uint16_t         pid;
    uint8_t          interface_num;
    bt_usb_ep_info_t intr_in;
    bt_usb_ep_info_t bulk_in;
    bt_usb_ep_info_t bulk_out;
    bool             has_intr_in;
    bool             has_bulk_in;
    bool             has_bulk_out;
} bt_usb_device_info_t;

/**
 * Platform HAL callbacks (implemented by Xbox 360 Usbd HAL or Linux test harness).
 */
typedef struct {
    void *ctx;
    int (*submit_control_out)(void *ctx, uint8_t bmRequestType, uint8_t bRequest,
                              uint16_t wValue, uint16_t wIndex,
                              const uint8_t *data, uint16_t length);
    int (*submit_intr_in)(void *ctx, uint8_t *buf, uint16_t max_len);
} bt_usb_hal_ops_t;

typedef enum {
    BT_INIT_IDLE = 0,
    BT_INIT_WAIT_RESET,
    BT_INIT_WAIT_LOCAL_VER,
    BT_INIT_WAIT_BD_ADDR,
    BT_INIT_READY,
    BT_INIT_ERROR
} bt_init_state_t;

#define BT_MAX_DISCOVERED_DEVICES 16u

typedef struct {
    bt_usb_device_info_t     dev;
    bt_usb_hal_ops_t         hal;
    bt_init_state_t          state;

    /* Interrupt IN event reassembly buffer */
    uint8_t                  evt_rx_chunk[64];
    uint8_t                  evt_accum[HCI_MAX_EVT_SIZE];
    size_t                   evt_accum_len;

    /* Decoded controller info */
    bool                     has_local_ver;
    hci_local_version_info_t local_ver;
    bool                     has_bd_addr;
    hci_bd_addr_info_t       bd_addr;
    int                      last_error;

    /* Inquiry / device discovery state */
    bool                     scan_active;
    bool                     scan_done;
    uint8_t                  scan_duration;
    hci_inquiry_item_t       scan_results[BT_MAX_DISCOVERED_DEVICES];
    size_t                   scan_count;
} bt_usb_transport_t;

/**
 * Check if a USB interface descriptor matches standard Bluetooth HCI (0xE0/0x01/0x01).
 */
bool bt_usb_is_hci_interface(uint8_t bInterfaceClass,
                             uint8_t bInterfaceSubClass,
                             uint8_t bInterfaceProtocol);

/**
 * Initialize transport context with discovered device/endpoint info and HAL ops.
 */
int bt_usb_transport_init(bt_usb_transport_t *tr,
                          const bt_usb_device_info_t *dev,
                          const bt_usb_hal_ops_t *hal);

/**
 * Start the HCI initialization sequence (arms Interrupt IN and sends HCI_Reset).
 */
int bt_usb_transport_start_init(bt_usb_transport_t *tr);

/**
 * Feed raw bytes received from the Interrupt IN endpoint (handles fragmentation).
 * Advances the initialization state machine when complete HCI events arrive.
 */
void bt_usb_transport_on_intr_in(bt_usb_transport_t *tr,
                                 const uint8_t *data, size_t len);

/**
 * Start Bluetooth Classic device discovery (HCI_Inquiry, Extended Inquiry Mode).
 * duration_1280ms is in units of 1.28s (e.g. 4 = ~5.12s, range 1..48).
 */
int bt_usb_transport_start_inquiry(bt_usb_transport_t *tr, uint8_t duration_1280ms);

/**
 * Cancel an active inquiry scan.
 */
int bt_usb_transport_stop_inquiry(bt_usb_transport_t *tr);

#ifdef __cplusplus
}
#endif

#endif /* XBOX360_BT_USB_TRANSPORT_H */
