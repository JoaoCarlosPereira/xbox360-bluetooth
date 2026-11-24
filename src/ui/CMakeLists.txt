cmake_minimum_required(VERSION 3.25)
project(BTManager C)

add_executable(BTManager
    bt_manager_app.c
    ../manager/bt_manager.c
    ../hid/ds4_bt.c
    ../hci/hci_core.c
)

target_include_directories(BTManager PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
)

set_target_properties(BTManager PROPERTIES C_STANDARD 17 C_EXTENSIONS ON)
target_compile_options(BTManager PRIVATE -Wall -Wextra -Werror)

xsdk_add_xex(BTManager)
