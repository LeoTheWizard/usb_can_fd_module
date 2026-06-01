/**
 * @file main.c
 * @author Leo Walker
 *
 * @brief Main entry point for the RP2354A firmware.
 *
 * @copyright (c) 2025 Leo Walker @ MIT License @cite MIT License
 *
 */

#include <stdlib.h>

#include <pico/stdlib.h>
#include <pico/multicore.h>

#include "core0.h"
#include "core1.h"

int main(void)
{
    // Start core 1 program. USB Comms.
    multicore_launch_core1(core1_main);

    // Start core 0 program. CAN Bus Comms.
    core0_main();

    return EXIT_SUCCESS;
}