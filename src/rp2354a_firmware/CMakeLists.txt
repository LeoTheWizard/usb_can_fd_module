add_executable(usb_can_fd_firmware
    main.c
    core0.c
    core1.c
    can_queue.c
    usb_can.c
    usb_descriptors.c
    ${CMAKE_SOURCE_DIR}/common/crc32.c
)

target_include_directories(usb_can_fd_firmware PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/common
)

target_link_libraries(usb_can_fd_firmware
    pico_stdlib
    pico_multicore
    hardware_spi
    tinyusb_device
    tinyusb_board
    lw_mcp251xfd
)

# USB stdio disabled — TinyUSB is configured manually as a vendor class device.
# Enable UART stdio for debug output.
pico_enable_stdio_usb(usb_can_fd_firmware 0)
pico_enable_stdio_uart(usb_can_fd_firmware 1)

pico_add_extra_outputs(usb_can_fd_firmware)
