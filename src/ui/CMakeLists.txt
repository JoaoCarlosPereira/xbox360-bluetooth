cmake_minimum_required(VERSION 3.25)
project(BTManager C)

find_program(BTMANAGER_PYTHON NAMES python3 REQUIRED)
set(DASHBOARD_ASSETS "${CMAKE_CURRENT_SOURCE_DIR}/assets")
set(DASHBOARD_ATLAS "${CMAKE_CURRENT_BINARY_DIR}/dashboard_atlas.h")
add_custom_command(OUTPUT "${DASHBOARD_ATLAS}"
    COMMAND "${BTMANAGER_PYTHON}" "${DASHBOARD_ASSETS}/make_atlas.py" "${DASHBOARD_ATLAS}"
    DEPENDS "${DASHBOARD_ASSETS}/make_atlas.py"
            "${DASHBOARD_ASSETS}/Lato-Regular.ttf" "${DASHBOARD_ASSETS}/Lato-Heavy.ttf"
    VERBATIM
)

add_executable(BTManager
    bt_manager_app.c
    dashboard.c
    dashboard_draw.c
    "${DASHBOARD_ATLAS}"
    ../manager/bt_manager.c
    ../manager/bt_phase1.c
    ../manager/bt_trace.c
    ../hid/ds4_bt.c
    ../hci/hci_core.c
)

target_include_directories(BTManager PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
    "${CMAKE_CURRENT_BINARY_DIR}"
    "${XSDK_ROOT}/src"
)

set_target_properties(BTManager PROPERTIES C_STANDARD 17 C_EXTENSIONS ON)
target_compile_options(BTManager PRIVATE -Wall -Wextra -Werror)

option(BTMANAGER_XENIA "Present frames through VdSwap for Xenia testing" OFF)
if(BTMANAGER_XENIA)
    target_compile_definitions(BTManager PRIVATE BTMANAGER_XENIA)
else()
    target_sources(BTManager PRIVATE ../manager/console_mailbox.c)
endif()

xsdk_add_xex(BTManager TITLE_ID 0x42544D47 NAME "Bluetooth Manager")
