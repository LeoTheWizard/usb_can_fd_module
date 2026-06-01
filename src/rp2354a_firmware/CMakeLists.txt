add_executable(usb_can_fd_firmware
    main.c
    core0.c
    can_queue.c
)

target_link_libraries(usb_can_fd_firmware
    pico_stdlib
    hardware_spi
    lw_mcp251xfd
)

pico_enable_stdio_usb(usb_can_fd_firmware 1)
pico_enable_stdio_uart(usb_can_fd_firmware 0)

pico_add_extra_outputs(usb_can_fd_firmware)
