#ifndef BT_RTL_BOOT_H
#define BT_RTL_BOOT_H

#include "../transport/bt_usb_transport.h"
#include <stdbool.h>

/* Prepara o chipset (quirks ou carga de firmware Realtek). true = pode parear e buscar. */
bool rtl_boot_ready(bt_usb_transport_t *tr);
bool rtl_boot_pending(void);
bool rtl_boot_fw_missing(void);
void rtl_boot_reset(void);

#endif
