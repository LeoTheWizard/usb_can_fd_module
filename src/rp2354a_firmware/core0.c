/**
 * @file core0.c
 * @brief Core 0 program — CAN bus handling.
 *
 * Owns the MCP251xFD controller over SPI.
 * Responsibilities:
 *   - Initialise SPI, MCP251xFD, GPIO.
 *   - Service the MCP251xFD interrupt: read received frames, report errors.
 *   - Drain can_tx_queue and submit frames to the MCP251xFD TX FIFO.
 *   - Service IPC commands from core 1 (open/close bus, set bitrate/mode).
 *
 * @author Leo Walker
 * @copyright (c) Leo Walker 2025 @ MIT License @cite MIT License
 */

#include "core0.h"
#include "main.h"
#include "hardware_defs.h"
#include "ipc.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <lw_mcp251xfd/mcp251xfd.h>

#include <pico/stdlib.h>
#include <pico/multicore.h>
#include <hardware/spi.h>
#include <hardware/gpio.h>

// ---- Queue storage ---------------------------------------------------------

#define CAN_QUEUE_CAPACITY 1024

// can_queue masks indices with (capacity - 1), so capacity must be a power of two.
_Static_assert((CAN_QUEUE_CAPACITY & (CAN_QUEUE_CAPACITY - 1)) == 0,
               "CAN_QUEUE_CAPACITY must be a power of two");

static can_message_t can_tx_buffer[CAN_QUEUE_CAPACITY];
can_queue_t can_tx_queue = CAN_QUEUE_STATIC_INIT(can_tx_buffer, CAN_QUEUE_CAPACITY);

static can_message_t can_rx_buffer[CAN_QUEUE_CAPACITY];
can_queue_t can_rx_queue = CAN_QUEUE_STATIC_INIT(can_rx_buffer, CAN_QUEUE_CAPACITY);

// ---- MCP251xFD FIFO assignments --------------------------------------------

#define CAN_TX_FIFO 1
#define CAN_RX_FIFO 2

// Message RAM is 2 KB. With 64-byte payloads each object costs 8 + 64 = 72 bytes,
// so TX(8) + RX(20) = 28 objects = 2016 bytes, which fits the budget. Passing these
// to mcp251xfd_initialise keeps the unused TX Queue disabled and runs its RAM check.
static mcp251xfd_fifo_config_t can_fifo_configs[] = {
    [CAN_TX_FIFO - 1] = {.tx = true, .depth = 8, .payload = MCP251XFD_PLSIZE_64},
    [CAN_RX_FIFO - 1] = {.tx = false, .depth = 20, .payload = MCP251XFD_PLSIZE_64},
};

// ---- Module state ----------------------------------------------------------

static MCP251XFD *can_controller = NULL;

// ---- SPI platform callbacks ------------------------------------------------

static void spi_transfer_cb(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len)
{
    (void)ctx;
    spi_write_read_blocking(CAN_SPI_PORT, tx, rx, len);
}

static void spi_chip_select_cb(void *ctx, bool selected)
{
    (void)ctx;
    gpio_put(GPIO_CAN_CS, !selected);
}

static void delay_us_cb(uint32_t us)
{
    sleep_us(us);
}

// ---- Initialisation --------------------------------------------------------

static void initialise_spi(void)
{
    spi_init(CAN_SPI_PORT, CAN_SPI_BAUDRATE);
    gpio_set_function(GPIO_CAN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(GPIO_CAN_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(GPIO_CAN_MISO, GPIO_FUNC_SPI);
    gpio_set_function(GPIO_CAN_CS, GPIO_FUNC_SIO);
    gpio_set_dir(GPIO_CAN_CS, GPIO_OUT);
    gpio_put(GPIO_CAN_CS, 1);
}

static void initialise_leds(void)
{
    gpio_init(GPIO_LED_ACTIVITY);
    gpio_init(GPIO_LED_GREEN);
    gpio_init(GPIO_LED_YELLOW);
    gpio_init(GPIO_LED_RED);
    gpio_set_dir(GPIO_LED_ACTIVITY, GPIO_OUT);
    gpio_set_dir(GPIO_LED_GREEN, GPIO_OUT);
    gpio_set_dir(GPIO_LED_YELLOW, GPIO_OUT);
    gpio_set_dir(GPIO_LED_RED, GPIO_OUT);
}

static void initialise_can_int_gpio(void)
{
    gpio_init(GPIO_CAN_INT);
    gpio_set_dir(GPIO_CAN_INT, GPIO_IN);
    gpio_pull_up(GPIO_CAN_INT);
}

static void configure_can_filters(void)
{
    // Accept all frames into the RX FIFO.
    mcp251xfd_configure_filter(can_controller, 0, 0x000, 0x000, false, CAN_RX_FIFO);
}

static void configure_can_interrupts(void)
{
    mcp251xfd_configure_interrupts(can_controller,
                                   MCP251XFD_INT_RX |
                                       MCP251XFD_INT_CAN_ERROR |
                                       MCP251XFD_INT_RX_OVFLOW |
                                       MCP251XFD_INT_INVALID);
}

static void initialise_can_controller(void)
{
    if (can_controller != NULL)
    {
        mcp251xfd_destroy_instance(can_controller);
        can_controller = NULL;
    }

    can_controller = mcp251xfd_create_instance();

    mcp251xfd_config_t cfg = {
        .elapsed_us = time_us_32,
        .delay_func = delay_us_cb,
        .chip_enable = spi_chip_select_cb,
        .spi_transfer = spi_transfer_cb,
        .iface = CAN_SPI_PORT,
        .fosc = MCP251XFD_FOSC_40MHZ,
        .nominal_baud = CAN_BAUD_500KBPS,
        .data_baud = CAN_BAUD_500KBPS,
        .enable_ecc = false,
        .model = MODEL_MCP2518FD,
        .fifo_configs = can_fifo_configs,
        .fifo_count = sizeof(can_fifo_configs) / sizeof(can_fifo_configs[0]),
    };
    mcp251xfd_initialise(can_controller, &cfg);

    // FIFOs are set up by mcp251xfd_initialise from cfg; configure filters and interrupts.
    configure_can_filters();
    configure_can_interrupts();

    // Enable on-board 120Ω termination resistor.
    gpio_put(GPIO_CAN_120R_ENABLE, 1);

    mcp251xfd_change_opmode(can_controller, MCP251XFD_OPMODE_NORMAL, 100000);
    gpio_put(GPIO_LED_GREEN, 1);
}

// ---- CAN interrupt service -------------------------------------------------

static void service_can_rx(void)
{
    uint8_t pending = 0;
    if (mcp251xfd_rx_pending(can_controller, CAN_RX_FIFO, &pending) != MCP251XFD_RETURN_OK)
        return;

    while (pending > 0)
    {
        can_message_t msg = {0};
        msg.type = CAN_MSG_FRAME;
        msg.timestamp = time_us_32();

        if (mcp251xfd_get_received(can_controller, CAN_RX_FIFO, &msg.frame) != MCP251XFD_RETURN_OK)
            break;

        can_queue_push(&can_rx_queue, &msg);
        gpio_put(GPIO_LED_ACTIVITY, 1);
        pending--;
    }
}

static void service_can_error(uint32_t int_flags)
{
    can_message_t msg = {0};
    msg.type = CAN_MSG_ERROR;
    msg.timestamp = time_us_32();

    mcp251xfd_error_state_t state = {0};
    if (mcp251xfd_get_error_state(can_controller, &state) == MCP251XFD_RETURN_OK)
    {
        msg.error.rec = state.rec;
        msg.error.tec = state.tec;
        msg.error.error_warn = state.error_warn;
        msg.error.rx_passive = state.rx_passive;
        msg.error.tx_passive = state.tx_passive;
        msg.error.bus_off = state.bus_off;
    }

    mcp251xfd_diagnostics_t diag = {0};
    if (mcp251xfd_read_diagnostics(can_controller, &diag) == MCP251XFD_RETURN_OK)
    {
        msg.error.nominal_rx_errors = diag.nominal_rx_errors;
        msg.error.nominal_tx_errors = diag.nominal_tx_errors;
        msg.error.data_rx_errors = diag.data_rx_errors;
        msg.error.data_tx_errors = diag.data_tx_errors;
        msg.error.error_frame_count = diag.error_frame_count;
        msg.error.nbit0_err = diag.nbit0_err;
        msg.error.nbit1_err = diag.nbit1_err;
        msg.error.nack_err = diag.nack_err;
        msg.error.nform_err = diag.nform_err;
        msg.error.nstuff_err = diag.nstuff_err;
        msg.error.ncrc_err = diag.ncrc_err;
        msg.error.dbit0_err = diag.dbit0_err;
        msg.error.dbit1_err = diag.dbit1_err;
        msg.error.dform_err = diag.dform_err;
        msg.error.dstuff_err = diag.dstuff_err;
        msg.error.dcrc_err = diag.dcrc_err;
        msg.error.txbo_err = diag.txbo_err;
        msg.error.dlc_mismatch = diag.dlc_mismatch;
    }

    if (int_flags & MCP251XFD_INT_RX_OVFLOW)
    {
        bool overflowed = false;
        mcp251xfd_get_rx_overflow(can_controller, CAN_RX_FIFO, &overflowed);
        msg.error.rx_overflow = overflowed;
    }

    can_queue_push(&can_rx_queue, &msg);
    gpio_put(GPIO_LED_RED, state.bus_off || state.tx_passive || state.rx_passive);
    gpio_put(GPIO_LED_YELLOW, state.error_warn);

    if (state.bus_off)
        mcp251xfd_recover_bus_off(can_controller, 200000);
}

static void service_can_interrupt(void)
{
    uint32_t flags = 0;
    if (mcp251xfd_get_interrupt_flags(can_controller, &flags) != MCP251XFD_RETURN_OK)
        return;

    if (flags & MCP251XFD_INT_RX)
        service_can_rx();

    if (flags & (MCP251XFD_INT_CAN_ERROR | MCP251XFD_INT_RX_OVFLOW | MCP251XFD_INT_INVALID))
        service_can_error(flags);

    mcp251xfd_clear_interrupt_flags(can_controller, flags);
}

// ---- TX service ------------------------------------------------------------

static void service_can_tx(void)
{
    can_message_t msg;
    while (can_queue_peek(&can_tx_queue, &msg) == 0)
    {
        mcp251xfd_return_t ret = mcp251xfd_transmit(can_controller, CAN_TX_FIFO, &msg.frame);
        if (ret == MCP251XFD_RETURN_TX_FIFO_FULL)
            break; // Leave the frame in the queue; retry next iteration.

        can_queue_pop(&can_tx_queue, &msg);
        gpio_put(GPIO_LED_ACTIVITY, 1);
    }
}

// ---- IPC service -----------------------------------------------------------

static void service_ipc(void)
{
    if (!multicore_fifo_rvalid())
        return;

    uint32_t cmd = multicore_fifo_pop_blocking();

    switch (cmd)
    {
    case IPC_CMD_OPEN:
        mcp251xfd_change_opmode(can_controller, MCP251XFD_OPMODE_NORMAL, 100000);
        gpio_put(GPIO_LED_GREEN, 1);
        break;

    case IPC_CMD_CLOSE:
        mcp251xfd_change_opmode(can_controller, MCP251XFD_OPMODE_SLEEP, 100000);
        gpio_put(GPIO_LED_GREEN, 0);
        break;

    case IPC_CMD_SET_MODE:
    {
        uint32_t mode = multicore_fifo_pop_blocking();
        mcp251xfd_change_opmode(can_controller, (mcp251xfd_opmode_t)mode, 100000);
        break;
    }

    case IPC_CMD_SET_BITTIMING:
    {
        uint32_t nominal = multicore_fifo_pop_blocking();
        uint32_t data = multicore_fifo_pop_blocking();
        // Bitrate changes require config mode.
        mcp251xfd_change_opmode(can_controller, MCP251XFD_OPMODE_CONFIG, 100000);
        mcp251xfd_set_baudrates(can_controller, nominal, data);
        mcp251xfd_change_opmode(can_controller, MCP251XFD_OPMODE_NORMAL, 100000);
        break;
    }

    case IPC_CMD_SET_TERMINATION:
    {
        uint32_t enable = multicore_fifo_pop_blocking();
        gpio_put(GPIO_CAN_120R_ENABLE, enable ? 1 : 0);
        break;
    }

    default:
        break;
    }
}

// ---- Entry point -----------------------------------------------------------

void core0_main(void)
{
    initialise_spi();
    initialise_leds();
    initialise_can_controller();
    initialise_can_int_gpio();

    while (true)
    {
        service_ipc();

        // INT pin is active-low; stays asserted until all MCP251xFD flags are cleared.
        while (!gpio_get(GPIO_CAN_INT))
            service_can_interrupt();

        service_can_tx();

        gpio_put(GPIO_LED_ACTIVITY, 0);
    }
}
