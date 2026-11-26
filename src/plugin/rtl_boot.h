#ifndef BT_RTL_BOOT_H
#define BT_RTL_BOOT_H

#include "../transport/bt_usb_transport.h"
#include <stdbool.h>

/* O ROM 8761 nao faz inquiry ate receber o firmware. true = pode parear e buscar. */
bool rtl_boot_ready(bt_usb_transport_t *tr);
bool rtl_boot_pending(void);
void rtl_boot_reset(void);

#endif
