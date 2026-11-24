#ifndef XBOX360_USBD_HAL_H
#define XBOX360_USBD_HAL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Xbox 360 Kernel (xboxkrnl.exe) USBD ordinals
 * Verified against kernelext21250.0.def and hiddriver360/hiddriver/main.cpp
 */
#define XBOX_ORD_USBD_ADD_DEVICE_COMPLETE         740u
#define XBOX_ORD_USBD_CANCEL_ASYNC_TRANSFER       741u
#define XBOX_ORD_USBD_GET_DEVICE_SPEED            742u
#define XBOX_ORD_USBD_GET_DEVICE_TOPOLOGY         743u
#define XBOX_ORD_USBD_GET_ENDPOINT_DESCRIPTOR     744u
#define XBOX_ORD_USBD_IS_DEVICE_AUTHENTICATED     745u
#define XBOX_ORD_USBD_OPEN_DEFAULT_ENDPOINT       746u
#define XBOX_ORD_USBD_OPEN_ENDPOINT               747u
#define XBOX_ORD_USBD_QUEUE_ASYNC_TRANSFER        748u
#define XBOX_ORD_USBD_QUEUE_CLOSE_DEFAULT_EP      749u
#define XBOX_ORD_USBD_QUEUE_CLOSE_ENDPOINT        750u
#define XBOX_ORD_USBD_REMOVE_DEVICE_COMPLETE      751u
#define XBOX_ORD_USBD_REGISTER_DRIVER_OBJECT      755u
#define XBOX_ORD_USBD_UNREGISTER_DRIVER_OBJECT    756u
#define XBOX_ORD_USBD_RESET_DEVICE                758u
#define XBOX_ORD_USBD_GET_DEVICE_DESCRIPTOR       759u

static inline uint16_t x360_bswap16(uint16_t v) {
    return (uint16_t)((v >> 8) | (v << 8));
}

/* Standard USB descriptors (packed, wire little-endian for 16-bit fields) */
#pragma pack(push, 1)
typedef struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} x360_usb_device_descriptor_t;

typedef struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bInterfaceNumber;
    uint8_t  bAlternateSetting;
    uint8_t  bNumEndpoints;
    uint8_t  bInterfaceClass;
    uint8_t  bInterfaceSubClass;
    uint8_t  bInterfaceProtocol;
    uint8_t  iInterface;
} x360_usb_interface_descriptor_t;

typedef struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bEndpointAddress;
    uint8_t  bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
} x360_usb_endpoint_descriptor_t;
#pragma pack(pop)

/*
 * Xbox 360 Kernel Transfer Request Block (TRB) structures
 * Layout verified from hiddriver360/hiddriver/main.cpp:44-69
 */
typedef struct {
    uint32_t endpoint;
    uint32_t callback;
    uint32_t savedEndpoint;
    uint8_t  padding[4];
    uint8_t  flags;
    uint8_t  controllerIndex; /* written by UsbdQueueAsyncTransfer */
    uint8_t  pad2;
    uint8_t  endpointIndex;   /* written by UsbdQueueAsyncTransfer */
    void    *buffer;
    uint32_t length;
} x360_usb_trb_t;

typedef struct {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;  /* Must be bswap16'd on Big-Endian Xbox 360 */
    uint16_t wIndex;  /* Must be bswap16'd on Big-Endian Xbox 360 */
    uint16_t wLength; /* Must be bswap16'd on Big-Endian Xbox 360 */
} x360_usb_setup_packet_t;

typedef struct {
    x360_usb_trb_t          trb;
    uint8_t                 pad[4];
    x360_usb_setup_packet_t packet;
} x360_usb_control_trb_t;

#ifdef __cplusplus
}
#endif

#endif /* XBOX360_USBD_HAL_H */
