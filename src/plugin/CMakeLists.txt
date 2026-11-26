cmake_minimum_required(VERSION 3.20)
project(bt360_proto C)

add_library(bt360_proto OBJECT
    plugin_main.c
    rtl_boot.c
    ../chipset/rtl8761b.c
    ../manager/console_mailbox.c
    ../manager/bt_trace.c
    ../manager/bt_phase1.c
    ../manager/bt_pair_session.c
    ../manager/bt_manager.c
    ../hid/ds4_bt.c
    ../hid/x360_pad_inject.c
    ../l2cap/l2cap.c
    bt_hid_link.c
    ppc_detour.c
    ../hal/xbox360_usbd.c
    ../transport/bt_usb_transport.c
    ../hci/hci_core.c
)

target_include_directories(bt360_proto PRIVATE
    "${XSDK_ROOT}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
)

target_compile_options(bt360_proto PRIVATE -Wall -Wextra -Werror -std=gnu17)

xsdk_add_plugin(bt360_proto BASE 0x91440000)
