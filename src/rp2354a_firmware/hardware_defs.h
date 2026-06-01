/**
 * @file hardware_defs.h
 * @brief Hardware definitions and configurations for the RP2354A firmware.
 * Defines pin assignments, SPI configurations, and other hardware-related constants for the module PCB.
 *
 * @author Leo Walker
 * @copyright (c) Leo Walker 2025 @ MIT License @cite MIT License
 */

#ifndef _HARDWARE_DEFS_H_
#define _HARDWARE_DEFS_H_

#include <hardware/spi.h>

#define CAN_SPI_PORT spi1
#define CAN_SPI_BAUDRATE 15000000

enum gpio_pins
{
    // CAN Controller SPI pins.
    GPIO_CAN_SCK = 14,
    GPIO_CAN_MOSI = 15,
    GPIO_CAN_MISO = 12,
    GPIO_CAN_CS = 13,

    // LED pins.
    GPIO_LED_ACTIVITY = 19,
    GPIO_LED_GREEN = 9,
    GPIO_LED_YELLOW = 10,
    GPIO_LED_RED = 11
};

#endif /* _HARDWARE_DEFS_H_ */