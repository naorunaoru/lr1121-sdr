# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.13)
if(NOT PICO_SDK_PATH)
    if(DEFINED ENV{PICO_SDK_PATH})
        set(PICO_SDK_PATH "$ENV{PICO_SDK_PATH}")
    else()
        set(PICO_SDK_PATH "${CMAKE_CURRENT_LIST_DIR}/../external/pico-sdk")
    endif()
endif()
if(NOT EXISTS "${PICO_SDK_PATH}/external/pico_sdk_import.cmake")
    message(FATAL_ERROR "Set PICO_SDK_PATH to a Pico SDK checkout")
endif()
include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)
project(lr1121_bridge C CXX ASM)
set(CMAKE_C_STANDARD 11)
# Only the generated, verified binding enables packed rates on that module.
set(LR1121_VERIFIED_UID "" CACHE STRING "UID verified by lr1121/install.py")
string(LENGTH "${LR1121_VERIFIED_UID}" UID_LENGTH)
if(NOT LR1121_VERIFIED_UID STREQUAL "" AND
   (NOT UID_LENGTH EQUAL 16 OR NOT LR1121_VERIFIED_UID MATCHES "^[0-9a-f]+$"))
    message(FATAL_ERROR "LR1121_VERIFIED_UID must be empty or 16 lowercase hex digits")
endif()
if(NOT picotool_DIR)
    set(picotool_DIR "${CMAKE_CURRENT_LIST_DIR}/../build/tools/picotool")
endif()
if(NOT EXISTS "${picotool_DIR}/picotoolConfig.cmake")
    message(FATAL_ERROR "Build the pinned picotool first (tools/build_pico.py), or set picotool_DIR")
endif()
pico_sdk_init()
add_executable(lr1121_bridge firmware/main.c firmware/protocol.c firmware/stream.c firmware/stream_format.c firmware/packed_stream.c firmware/stream_queue.c)
target_link_libraries(lr1121_bridge pico_stdlib hardware_spi hardware_dma pico_unique_id pico_multicore)
target_compile_options(lr1121_bridge PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(lr1121_bridge PRIVATE
    LR1121_VERIFIED_UID="${LR1121_VERIFIED_UID}"
    PICO_STDIO_USB_DEFAULT_CRLF=0
    PICO_STDIO_USB_ENABLE_IRQ_BACKGROUND_TASK=0
    PICO_STDIO_USB_ENABLE_RESET_VIA_BAUD_RATE=0
    PICO_STDIO_USB_ENABLE_RESET_VIA_VENDOR_INTERFACE=0
    PICO_ENABLE_USB_RESET_VIA_BAUD_RATE=0
    PICO_ENABLE_USB_RESET_VIA_VENDOR_INTERFACE=0
    CFG_TUD_CDC_TX_BUFSIZE=16384
    CFG_TUD_CDC_EP_BUFSIZE=2048
    CFG_TUD_CDC_RX_BUFSIZE=2048
    USBD_PRODUCT="LR1121 SPI Bridge"
)
pico_enable_stdio_usb(lr1121_bridge 1)
pico_enable_stdio_uart(lr1121_bridge 0)
pico_set_program_name(lr1121_bridge "lr1121_bridge")
pico_set_program_version(lr1121_bridge "1.0.7")
pico_add_extra_outputs(lr1121_bridge)
