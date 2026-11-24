CC ?= gcc
CFLAGS ?= -Wall -Wextra -Werror -O2 -std=c11 -I../hci -I../transport -I../hal -I../chipset -I../l2cap -I../hid -I../manager

TARGETS := hci_probe test_usb_transport usbdevfs_probe test_l2cap_ds4 ds4_live test_manager_ui

all: $(TARGETS)

hci_probe: main.c ../hci/hci_core.c ../hci/hci_core.h
	$(CC) $(CFLAGS) main.c ../hci/hci_core.c -o hci_probe

test_usb_transport: test_usb_transport.c ../transport/bt_usb_transport.c ../transport/bt_usb_transport.h ../hci/hci_core.c ../hci/hci_core.h ../hal/xbox360_usbd.h
	$(CC) $(CFLAGS) test_usb_transport.c ../transport/bt_usb_transport.c ../hci/hci_core.c -o test_usb_transport

usbdevfs_probe: usbdevfs_probe.c ../transport/bt_usb_transport.c ../transport/bt_usb_transport.h ../hci/hci_core.c ../hci/hci_core.h ../chipset/rtl8761b.c ../chipset/rtl8761b.h ../hal/xbox360_usbd.h
	$(CC) $(CFLAGS) usbdevfs_probe.c ../transport/bt_usb_transport.c ../hci/hci_core.c ../chipset/rtl8761b.c -o usbdevfs_probe

test_l2cap_ds4: test_l2cap_ds4.c ../l2cap/l2cap.c ../l2cap/l2cap.h ../hid/ds4_bt.c ../hid/ds4_bt.h ../chipset/rtl8761b.c ../chipset/rtl8761b.h ../hci/hci_core.c ../hci/hci_core.h
	$(CC) $(CFLAGS) test_l2cap_ds4.c ../l2cap/l2cap.c ../hid/ds4_bt.c ../chipset/rtl8761b.c ../hci/hci_core.c -o test_l2cap_ds4

ds4_live: ds4_live.c ../l2cap/l2cap.c ../l2cap/l2cap.h ../hid/ds4_bt.c ../hid/ds4_bt.h ../transport/bt_usb_transport.c ../transport/bt_usb_transport.h ../hci/hci_core.c ../hci/hci_core.h
	$(CC) $(CFLAGS) ds4_live.c ../l2cap/l2cap.c ../hid/ds4_bt.c ../transport/bt_usb_transport.c ../hci/hci_core.c -o ds4_live

test_manager_ui: test_manager_ui.c ../manager/bt_manager.c ../manager/bt_manager.h ../hid/ds4_bt.c ../hid/ds4_bt.h ../hci/hci_core.c ../hci/hci_core.h
	$(CC) $(CFLAGS) test_manager_ui.c ../manager/bt_manager.c ../hid/ds4_bt.c ../hci/hci_core.c -o test_manager_ui

test: test_usb_transport test_l2cap_ds4 test_manager_ui
	./test_usb_transport
	./test_l2cap_ds4
	./test_manager_ui

clean:
	rm -f $(TARGETS)

.PHONY: all test clean
