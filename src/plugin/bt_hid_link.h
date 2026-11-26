#ifndef BT_HID_LINK_H
#define BT_HID_LINK_H

#include "../hid/ds4_bt.h"
#include <stddef.h>
#include <stdint.h>

/* Liga o relatorio do DS4 no estado que o plugin publica no XamInput. */
void bt_hid_link_bind(x360_gamepad_state_t *report, bool *ps);

/* Abre HID Control e Interrupt no ACL autenticado. */
/* 0 se o primeiro pacote L2CAP entrou na fila USB. */
int bt_hid_link_start(uint16_t acl_handle);
void bt_hid_link_stop(void);

/* Bytes de um URB bulk IN (um ou mais pacotes HCI ACL, com sobra zerada). */
void bt_hid_link_on_acl(const uint8_t *data, size_t len);

/* Quadro de vibracao 0x11. 0 se o canal HID aceitou. */
int bt_hid_link_output(const uint8_t *data, size_t len);

/* Reenvia o quadro de saida enquanto o HID estiver aberto. O DS4 cai sem isso. */
void bt_hid_link_tick(uint64_t now_ms);

#endif
