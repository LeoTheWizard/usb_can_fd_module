/**
 * @file core0.c
 * @brief Implementation of the CAN bus handling logic.
 *
 * @author Leo Walker
 * @copyright (c) Leo Walker 2025 @ MIT License @cite MIT License
 */

#include "core0.h"

#include <stdbool.h>
#include <stdint.h>

/// My MCP2518FD CAN controller driver.
#include <lw_mcp251xfd/mcp251xfd.h>

/// Pico SDK headers.
#include <pico/stdlib.h>
#include <hardware/spi.h>
#include <hardware/gpio.h>

#define SPI_PORT spi1
#define SPI_BAUDRATE 15000000
#define SPI_SCK_PIN 14
#define SPI_MOSI_PIN 15
#define SPI_MISO_PIN 12
#define SPI_CS_PIN 13

static MCP251XFD *can_controller = NULL;

// TX Queue.
static can_message_t can_tx_buffer_memory[32];
can_queue_t can_tx_queue = CAN_QUEUE_STATIC_INIT(can_tx_buffer_memory, 32);

// RX Queue.
static can_message_t can_rx_buffer_memory[32];
can_queue_t can_rx_queue = CAN_QUEUE_STATIC_INIT(can_rx_buffer_memory, 32);

#pragma region SPI Bus
static void initialise_spi(void)
{
    spi_init(SPI_PORT, SPI_BAUDRATE);
    gpio_set_function(SPI_SCK_PIN, GPIO_FUNC_SPI);  // SCK
    gpio_set_function(SPI_MOSI_PIN, GPIO_FUNC_SPI); // MOSI
    gpio_set_function(SPI_MISO_PIN, GPIO_FUNC_SPI); // MISO
    gpio_set_function(SPI_CS_PIN, GPIO_FUNC_SIO);   // CS
    gpio_set_dir(SPI_CS_PIN, GPIO_OUT);
    gpio_put(SPI_CS_PIN, 1); // Set CS high (inactive)
}

static void spi_transfer(void *ctx, const uint8_t *tx_buf, uint8_t *rx_buf, size_t len)
{
    spi_write_read_blocking(SPI_PORT, tx_buf, rx_buf, len);
}

static void spi_chip_select(void *ctx, bool selected)
{
    gpio_put(SPI_CS_PIN, !selected);
}

static void delay_us(uint32_t us)
{
    sleep_us(us);
}

#pragma endregion SPI Bus

static void initialise_can_controller(void)
{
    if (can_controller != NULL)
    {
        mcp251xfd_destroy_instance(can_controller);
        can_controller = NULL;
    }

    // Allocate memory for device context.
    can_controller = mcp251xfd_create_instance();

    // Set  configuration parameters for the MCP2518FD CAN controller.
    mcp251xfd_config_t can_config = {
        .elapsed_us = time_us_32,
        .delay_func = delay_us,
        .chip_enable = spi_chip_select,
        .spi_transfer = spi_transfer,
        .iface = SPI_PORT,                // No additional context needed for SPI functions.
        .fosc = MCP251XFD_FOSC_40MHZ,     // Set based on your hardware design.
        .nominal_baud = CAN_BAUD_500KBPS, // Set desired nominal baud rate.
        .data_baud = CAN_BAUD_500KBPS,    // Set desired data baud rate for CAN FD.
        .enable_ecc = false,
        .model = MODEL_MCP2518FD, // Set based on your specific chip model.
        .fifo_configs = NULL,     // Use default FIFO configuration.
        .fifo_count = 0};

    // Initialise the CAN controller with the config.
    mcp251xfd_initialise(can_controller, &can_config);
}

void core0_main(void)
{
    initialise_spi();
    initialise_can_controller();

    while (true)
    {
    }
}