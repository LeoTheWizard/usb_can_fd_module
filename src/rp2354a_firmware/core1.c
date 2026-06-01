/**
 * @file core1.c
 * @brief Entry point for core 1 program.
 * Implements USB communication with the host and handles reception of CAN frames from core 0.
 *
 * @author Leo Walker
 * @copyright (c) Leo Walker 2025 @ MIT License @cite MIT License
 */

#include "core1.h"
#include "usb_can.h"
#include "main.h"

#include <pico/stdlib.h>

void core1_main(void)
{
    usb_can_init();

    while (true)
    {
        usb_can_task(&can_rx_queue, &can_tx_queue);
    }
}
