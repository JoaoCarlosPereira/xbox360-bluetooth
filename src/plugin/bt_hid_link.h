#ifndef BT_HID_LINK_H
#define BT_HID_LINK_H

#include "../hid/ds4_bt.h"
#include <stddef.h>
#include <stdint.h>

#define BT_HID_SLOTS 2

/* Liga o relatorio do DS4 no estado que o plugin publica no XamInput. */
void bt_hid_link_bind(int slot, x360_gamepad_state_t *report, bool *ps);

/* Abre HID Control e Interrupt no ACL autenticado. 0 se o primeiro L2CAP entrou na fila. */
int bt_hid_link_start(int slot, uint16_t acl_handle);
void bt_hid_link_stop(int slot);

/* Bytes de um URB bulk IN (um ou mais pacotes HCI ACL, com sobra zerada). */
void bt_hid_link_on_acl(const uint8_t *data, size_t len);

/* Quadro de vibracao 0x11. 0 se o canal HID desse slot aceitou. */
int bt_hid_link_output(int slot, const uint8_t *data, size_t len);

/* Reenvia o quadro de saida enquanto o HID estiver aberto. O DS4 cai sem isso. */
void bt_hid_link_tick(uint64_t now_ms);

#endif
