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

#define CAN_TX_FIFO_DEPTH 8
#define CAN_TEF_DEPTH     8 // Transmit Event FIFO; >= TX FIFO depth so it never overflows.

// Message RAM is 2 KB. Each 64-byte FIFO object costs 8 + 64 = 72 bytes; each TEF
// entry (header only, no timestamp) costs 8 bytes. Budget:
//   TX(8)x72 + RX(19)x72 + TEF(8)x8 = 576 + 1368 + 64 = 2008 bytes (<= 2048).
// The library's pre-flight RAM check covers only the FIFOs below; the TEF is enabled
// separately in initialise_can_controller(), so keep this total in mind when tuning.
static mcp251xfd_fifo_config_t can_fifo_configs[] = {
    [CAN_TX_FIFO - 1] = {.tx = true, .depth = CAN_TX_FIFO_DEPTH, .payload = MCP251XFD_PLSIZE_64},
    [CAN_RX_FIFO - 1] = {.tx = false, .depth = 19, .payload = MCP251XFD_PLSIZE_64},
};

// Pending TX echo cookies, recorded when a frame is submitted and returned when its
// TEF (transmit event) arrives. Single TX FIFO + TEF are in-order, so a plain FIFO
// keeps cookies aligned with completions. Accessed only by core 0 (no locking).
#define TX_COOKIE_CAPACITY 16
static uint32_t tx_cookies[TX_COOKIE_CAPACITY];
static size_t   tx_cookie_head = 0;
static size_t   tx_cookie_tail = 0;

// Latched when a push to can_rx_queue is dropped because the queue is full (host too
// slow); reported to the host as an error message once the queue drains. core 0 only.
static bool rx_overflowed = false;

// ---- Module state ----------------------------------------------------------

static MCP251XFD *can_controller = NULL;

// Live status snapshot, published here by core 0 and read by core 1 for the USB
// GET_STATUS query. Declared in ipc.h.
device_status_t g_device_status = {0};

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

// ---- LED status ------------------------------------------------------------
//
// Three status LEDs show CAN bus health — exactly one is lit while the bus is open,
// all off while it is closed:
//   green  — bus open and error-active (healthy)
//   yellow — error-warning or error-passive (degraded, still communicating)
//   red    — bus-off (cannot communicate)
// A fourth LED blinks on TX/RX activity.

// Set the CAN bus state: drives the three status LEDs and publishes it for GET_STATUS.
// Error-warning and error-passive both show yellow; config/offline shows nothing.
static void set_bus_state(can_bus_state_t s)
{
    g_device_status.bus_state = (uint8_t)s;
    gpio_put(GPIO_LED_GREEN, s == CAN_BUS_STATE_ACTIVE);
    gpio_put(GPIO_LED_YELLOW, s == CAN_BUS_STATE_WARNING || s == CAN_BUS_STATE_PASSIVE);
    gpio_put(GPIO_LED_RED, s == CAN_BUS_STATE_BUS_OFF);
}

// Activity LED: pulse-stretched so a single frame is a visible blink and continuous
// traffic shows as a steady blink. Driven from the core 0 loop only (no locking).
#define ACTIVITY_ON_US 20000u  // minimum on time per blink
#define ACTIVITY_OFF_US 20000u // minimum off time between blinks

static bool activity_pending = false; // traffic seen since the last blink started
static bool activity_led_on = false;
static uint32_t activity_changed_us = 0;

static void note_activity(void)
{
    activity_pending = true;
}

static void service_activity_led(void)
{
    uint32_t now = time_us_32();

    if (activity_led_on)
    {
        if (now - activity_changed_us >= ACTIVITY_ON_US)
        {
            gpio_put(GPIO_LED_ACTIVITY, 0);
            activity_led_on = false;
            activity_changed_us = now;
        }
    }
    else if (activity_pending && now - activity_changed_us >= ACTIVITY_OFF_US)
    {
        activity_pending = false;
        gpio_put(GPIO_LED_ACTIVITY, 1);
        activity_led_on = true;
        activity_changed_us = now;
    }
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
                                       MCP251XFD_INT_TEF |
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

    // Enable the Transmit Event FIFO so transmitted frames are confirmed back to the
    // host. Timestamps come from time_us_32() at read time (matching the RX path), so
    // the hardware TEF timestamp is not requested. Must be done in config mode.
    mcp251xfd_tef_config_t tef_cfg = {.depth = CAN_TEF_DEPTH, .timestamps = false};
    mcp251xfd_enable_tef(can_controller, &tef_cfg);

    // Enable on-board 120Ω termination resistor.
    gpio_put(GPIO_CAN_120R_ENABLE, 1);
    g_device_status.termination = 1;

    // Stay in configuration mode (off the bus) until the host opens the interface;
    // IPC_CMD_OPEN transitions to NORMAL. mcp251xfd_initialise() already left the
    // controller in config mode, so the device does not drive or ACK the bus yet.
    mcp251xfd_change_opmode(can_controller, MCP251XFD_OPMODE_CONFIG, 100000);
    set_bus_state(CAN_BUS_STATE_CONFIG);
}

// ---- Helpers ---------------------------------------------------------------

// Push to the RX queue; if it is full, latch rx_overflowed so the drop is reported.
static bool rx_queue_push(const can_message_t *msg)
{
    if (can_queue_push(&can_rx_queue, msg) != 0)
    {
        rx_overflowed = true;
        return false;
    }
    return true;
}

static void tx_cookie_push(uint32_t cookie)
{
    size_t next = (tx_cookie_head + 1) % TX_COOKIE_CAPACITY;
    if (next == tx_cookie_tail)
        return; // Full (should not happen: capacity >= TX FIFO depth). Drop.
    tx_cookies[tx_cookie_head] = cookie;
    tx_cookie_head = next;
}

static uint32_t tx_cookie_pop(void)
{
    if (tx_cookie_tail == tx_cookie_head)
        return 0; // Empty: no matching cookie (e.g. after a bus-off reset).
    uint32_t cookie = tx_cookies[tx_cookie_tail];
    tx_cookie_tail = (tx_cookie_tail + 1) % TX_COOKIE_CAPACITY;
    return cookie;
}

static void tx_cookie_reset(void)
{
    tx_cookie_head = tx_cookie_tail = 0;
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

        rx_queue_push(&msg);
        note_activity();
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

    rx_queue_push(&msg);

    g_device_status.tec = state.tec;
    g_device_status.rec = state.rec;
    if (state.bus_off)
        set_bus_state(CAN_BUS_STATE_BUS_OFF);
    else if (state.tx_passive || state.rx_passive)
        set_bus_state(CAN_BUS_STATE_PASSIVE);
    else if (state.error_warn)
        set_bus_state(CAN_BUS_STATE_WARNING);
    else
        set_bus_state(CAN_BUS_STATE_ACTIVE);

    if (state.bus_off)
    {
        // Drop pending TX cookies so they don't mis-pair with later transmit events.
        // Recovery is host-controlled (IPC_CMD_RESTART) to respect SocketCAN restart
        // semantics and to avoid blocking this loop in the bus-off recovery wait.
        tx_cookie_reset();
    }
}

// Drain the Transmit Event FIFO, emitting a confirmation per transmitted frame.
static void service_can_tef(void)
{
    mcp251xfd_tef_entry_t entry;
    while (mcp251xfd_read_tef(can_controller, &entry) == MCP251XFD_RETURN_OK)
    {
        can_message_t msg = {0};
        msg.type            = CAN_MSG_TX_EVENT;
        msg.timestamp       = time_us_32();
        msg.tx_event.cookie = tx_cookie_pop();
        msg.tx_event.id     = entry.id;
        msg.tx_event.flags  = entry.flags;
        msg.tx_event.dlc    = entry.dlc;
        rx_queue_push(&msg);
    }
}

static void service_can_interrupt(void)
{
    uint32_t flags = 0;
    if (mcp251xfd_get_interrupt_flags(can_controller, &flags) != MCP251XFD_RETURN_OK)
        return;

    if (flags & MCP251XFD_INT_RX)
        service_can_rx();

    if (flags & MCP251XFD_INT_TEF)
        service_can_tef();

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

        // Record the host's echo cookie so the matching TEF event can return it.
        tx_cookie_push((uint32_t)msg.timestamp);
        can_queue_pop(&can_tx_queue, &msg);
        note_activity();
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
        set_bus_state(CAN_BUS_STATE_ACTIVE);
        break;

    case IPC_CMD_CLOSE:
        mcp251xfd_change_opmode(can_controller, MCP251XFD_OPMODE_SLEEP, 100000);
        set_bus_state(CAN_BUS_STATE_CONFIG);
        break;

    case IPC_CMD_SET_MODE:
    {
        uint32_t mode = multicore_fifo_pop_blocking();
        mcp251xfd_change_opmode(can_controller, (mcp251xfd_opmode_t)mode, 100000);
        break;
    }

    case IPC_CMD_SET_BITTIMING:
    {
        uint32_t w0 = multicore_fifo_pop_blocking();
        uint32_t w1 = multicore_fifo_pop_blocking();
        mcp251xfd_bit_timing_t nominal = {
            .brp   = IPC_BITTIMING_UNPACK_BRP(w0),
            .tseg1 = IPC_BITTIMING_UNPACK_TSEG1(w0),
            .tseg2 = IPC_BITTIMING_UNPACK_TSEG2(w1),
            .sjw   = IPC_BITTIMING_UNPACK_SJW(w1),
        };
        // Bit-timing registers are writable only in config mode; stay there and let
        // IPC_CMD_OPEN return to NORMAL when the host brings the interface up.
        mcp251xfd_change_opmode(can_controller, MCP251XFD_OPMODE_CONFIG, 100000);
        mcp251xfd_set_bit_timing(can_controller, &nominal, NULL);
        break;
    }

    case IPC_CMD_SET_DATA_BITTIMING:
    {
        uint32_t w0 = multicore_fifo_pop_blocking();
        uint32_t w1 = multicore_fifo_pop_blocking();
        mcp251xfd_bit_timing_t data = {
            .brp   = IPC_BITTIMING_UNPACK_BRP(w0),
            .tseg1 = IPC_BITTIMING_UNPACK_TSEG1(w0),
            .tseg2 = IPC_BITTIMING_UNPACK_TSEG2(w1),
            .sjw   = IPC_BITTIMING_UNPACK_SJW(w1),
        };
        mcp251xfd_change_opmode(can_controller, MCP251XFD_OPMODE_CONFIG, 100000);
        mcp251xfd_set_bit_timing(can_controller, NULL, &data);
        break;
    }

    case IPC_CMD_SET_TERMINATION:
    {
        uint32_t enable = multicore_fifo_pop_blocking();
        gpio_put(GPIO_CAN_120R_ENABLE, enable ? 1 : 0);
        g_device_status.termination = enable ? 1 : 0;
        break;
    }

    case IPC_CMD_RESTART:
        // Host-requested recovery from bus-off (blocks briefly until the controller
        // completes the CAN recovery sequence; restart is rare and intentional).
        mcp251xfd_recover_bus_off(can_controller, 200000);
        tx_cookie_reset();
        g_device_status.tec = 0;
        g_device_status.rec = 0;
        set_bus_state(CAN_BUS_STATE_ACTIVE);
        break;

    default:
        break;
    }
}

// If an earlier RX-queue push was dropped, retry reporting it now that the queue may
// have drained. Sends a minimal error message flagging the software overflow.
static void service_sw_overflow_report(void)
{
    if (!rx_overflowed)
        return;

    can_message_t msg = {0};
    msg.type = CAN_MSG_ERROR;
    msg.timestamp = time_us_32();
    msg.error.sw_overflow = 1;

    if (can_queue_push(&can_rx_queue, &msg) == 0)
        rx_overflowed = false;
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
        service_sw_overflow_report();
        service_activity_led();
    }
}
