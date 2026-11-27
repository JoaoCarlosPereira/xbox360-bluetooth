CC ?= gcc
CFLAGS ?= -Wall -Wextra -Werror -O2 -std=c11 -I../hci -I../transport -I../hal -I../chipset -I../l2cap -I../hid -I../manager

TARGETS := hci_probe test_usb_transport usbdevfs_probe test_l2cap_ds4 ds4_live test_manager_ui test_phase1 test_pair_session test_transport_session test_pad_inject test_chipset

all: $(TARGETS)

hci_probe: main.c ../hci/hci_core.c ../hci/hci_core.h
	$(CC) $(CFLAGS) main.c ../hci/hci_core.c -o hci_probe

test_usb_transport: test_usb_transport.c ../transport/bt_usb_transport.c ../transport/bt_usb_transport.h ../hci/hci_core.c ../hci/hci_core.h ../hal/xbox360_usbd.h
	$(CC) $(CFLAGS) test_usb_transport.c ../transport/bt_usb_transport.c ../hci/hci_core.c -o test_usb_transport

usbdevfs_probe: usbdevfs_probe.c ../transport/bt_usb_transport.c ../transport/bt_usb_transport.h ../hci/hci_core.c ../hci/hci_core.h ../chipset/rtl8761b.c ../chipset/rtl8761b.h ../chipset/bt_chipset.c ../chipset/bt_chipset.h ../hal/xbox360_usbd.h
	$(CC) $(CFLAGS) usbdevfs_probe.c ../transport/bt_usb_transport.c ../hci/hci_core.c ../chipset/rtl8761b.c ../chipset/bt_chipset.c -o usbdevfs_probe

test_l2cap_ds4: test_l2cap_ds4.c ../l2cap/l2cap.c ../l2cap/l2cap.h ../hid/ds4_bt.c ../hid/ds4_bt.h ../chipset/rtl8761b.c ../chipset/rtl8761b.h ../hci/hci_core.c ../hci/hci_core.h
	$(CC) $(CFLAGS) test_l2cap_ds4.c ../l2cap/l2cap.c ../hid/ds4_bt.c ../chipset/rtl8761b.c ../hci/hci_core.c -o test_l2cap_ds4

ds4_live: ds4_live.c ../l2cap/l2cap.c ../l2cap/l2cap.h ../hid/ds4_bt.c ../hid/ds4_bt.h ../transport/bt_usb_transport.c ../transport/bt_usb_transport.h ../hci/hci_core.c ../hci/hci_core.h
	$(CC) $(CFLAGS) ds4_live.c ../l2cap/l2cap.c ../hid/ds4_bt.c ../transport/bt_usb_transport.c ../hci/hci_core.c -o ds4_live

test_manager_ui: test_manager_ui.c ../manager/bt_manager.c ../manager/bt_manager.h ../manager/bt_phase1.c ../manager/bt_phase1.h ../hid/ds4_bt.c ../hid/ds4_bt.h ../hci/hci_core.c ../hci/hci_core.h
	$(CC) $(CFLAGS) test_manager_ui.c ../manager/bt_manager.c ../manager/bt_phase1.c ../hid/ds4_bt.c ../hci/hci_core.c -o test_manager_ui

test_phase1: test_phase1.c ../manager/bt_phase1.c ../manager/bt_phase1.h
	$(CC) $(CFLAGS) test_phase1.c ../manager/bt_phase1.c -o test_phase1

test_pair_session: test_pair_session.c ../manager/bt_pair_session.c ../manager/bt_pair_session.h ../manager/bt_manager.c ../manager/bt_manager.h ../manager/bt_phase1.c ../manager/bt_phase1.h ../hid/ds4_bt.c ../hid/ds4_bt.h ../hci/hci_core.c ../hci/hci_core.h
	$(CC) $(CFLAGS) test_pair_session.c ../manager/bt_pair_session.c ../manager/bt_manager.c ../manager/bt_phase1.c ../hid/ds4_bt.c ../hci/hci_core.c -o test_pair_session

test_transport_session: test_transport_session.c ../transport/bt_usb_transport.c ../transport/bt_usb_transport.h ../hci/hci_core.c ../hci/hci_core.h
	$(CC) $(CFLAGS) test_transport_session.c ../transport/bt_usb_transport.c ../hci/hci_core.c -o test_transport_session

test_pad_inject: test_pad_inject.c ../plugin/ppc_detour.c ../plugin/ppc_detour.h ../hid/x360_pad_inject.c ../hid/x360_pad_inject.h ../hid/ds4_bt.c ../hid/ds4_bt.h ../manager/bt_phase1.c ../manager/bt_phase1.h
	$(CC) $(CFLAGS) test_pad_inject.c ../plugin/ppc_detour.c ../hid/x360_pad_inject.c ../hid/ds4_bt.c ../manager/bt_phase1.c -o test_pad_inject

test_chipset: test_chipset.c ../chipset/bt_chipset.c ../chipset/bt_chipset.h ../transport/bt_usb_transport.c ../transport/bt_usb_transport.h ../hci/hci_core.c ../hci/hci_core.h
	$(CC) $(CFLAGS) test_chipset.c ../chipset/bt_chipset.c ../transport/bt_usb_transport.c ../hci/hci_core.c -o test_chipset

test: test_usb_transport test_l2cap_ds4 test_manager_ui test_phase1 test_pair_session test_transport_session test_pad_inject test_chipset
	./test_usb_transport
	./test_l2cap_ds4
	./test_manager_ui
	./test_phase1
	./test_pair_session
	./test_transport_session
	./test_pad_inject
	./test_chipset

clean:
	rm -f $(TARGETS)

.PHONY: all test clean
