# Must come before project() — sets up the cross-compilation toolchain
if(DEFINED ENV{PICO_SDK_PATH})
    include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)
elseif(DEFINED PICO_SDK_PATH)
    include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)
else()
    message(FATAL_ERROR "PICO_SDK_PATH is not set. Pass -DPICO_SDK_PATH=... or set the environment variable.")
endif()

cmake_minimum_required(VERSION 3.13)
project(usb_can_fd_module C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

pico_sdk_init()

# External driver library (compiled for RP2350 via the Pico SDK toolchain)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../external/lw_mcp251xfd lw_mcp251xfd)

add_subdirectory(rp2354a_firmware)
