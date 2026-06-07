/**
 * @file ipc.h
 * @brief Inter-core command definitions for core 1 → core 0 communication.
 *
 * Commands are sent through the RP2350 inter-core FIFO (multicore_fifo).
 * Each command is one 32-bit word. Commands with a payload push additional
 * words immediately after the command word; core 0 pops them in order.
 *
 * A CAN bit-timing phase (brp/tseg1/tseg2/sjw) is packed into two words:
 *   word0 = brp | (tseg1 << 16)
 *   word1 = tseg2 | (sjw << 8)
 * See IPC_BITTIMING_PACK_* / IPC_BITTIMING_UNPACK_* helpers below.
 *
 * Protocol per command:
 *   IPC_CMD_OPEN              — [cmd]
 *   IPC_CMD_CLOSE             — [cmd]
 *   IPC_CMD_SET_MODE          — [cmd] [uint32_t opmode (mcp251xfd_opmode_t value)]
 *   IPC_CMD_SET_BITTIMING     — [cmd] [word0] [word1]  (nominal phase)
 *   IPC_CMD_SET_DATA_BITTIMING — [cmd] [word0] [word1] (data phase)
 *   IPC_CMD_SET_TERMINATION   — [cmd] [uint32_t enable (1 = on, 0 = off)]
 *   IPC_CMD_RESTART           — [cmd]  (recover from bus-off, resume NORMAL)
 *
 * @author Leo Walker
 * @copyright (c) Leo Walker 2025 @ MIT License @cite MIT License
 */

#ifndef _IPC_H_
#define _IPC_H_

#include <stdint.h>

/**
 * @brief CAN bus state reported in device_status_t.bus_state and over the wire
 * (USB_CAN_STATE_* in usb_can_protocol.h share these numeric values).
 */
typedef enum can_bus_state
{
    CAN_BUS_STATE_CONFIG = 0, // Off the bus (config mode / not yet opened).
    CAN_BUS_STATE_ACTIVE,     // Error-active (healthy).
    CAN_BUS_STATE_WARNING,    // Error-warning.
    CAN_BUS_STATE_PASSIVE,    // Error-passive.
    CAN_BUS_STATE_BUS_OFF,    // Bus-off.
} can_bus_state_t;

/**
 * @brief Live device status, written by core 0 and read by core 1 (USB control handler).
 * Each field is a single byte so individual reads are atomic; a status snapshot can
 * tolerate momentary cross-field skew, so no lock is used.
 */
typedef struct device_status
{
    volatile uint8_t bus_state;   // can_bus_state_t value.
    volatile uint8_t tec;         // Transmit error counter (last known).
    volatile uint8_t rec;         // Receive error counter (last known).
    volatile uint8_t termination; // 1 if the on-board 120Ω resistor is enabled.
} device_status_t;

extern device_status_t g_device_status;

#define IPC_CMD_OPEN              0x01U
#define IPC_CMD_CLOSE             0x02U
#define IPC_CMD_SET_MODE          0x03U
#define IPC_CMD_SET_BITTIMING     0x04U /**< Payload: two packed bit-timing words (nominal). */
#define IPC_CMD_SET_TERMINATION   0x05U /**< Payload: one word — 1 = enable, 0 = disable. */
#define IPC_CMD_SET_DATA_BITTIMING 0x06U /**< Payload: two packed bit-timing words (data phase). */
#define IPC_CMD_RESTART           0x07U /**< No payload: recover from bus-off and resume NORMAL. */

/* Pack/unpack a bit-timing phase to/from two inter-core FIFO words. */
#define IPC_BITTIMING_PACK_W0(brp, tseg1)  ((uint32_t)(brp) | ((uint32_t)(tseg1) << 16))
#define IPC_BITTIMING_PACK_W1(tseg2, sjw)  ((uint32_t)(tseg2) | ((uint32_t)(sjw) << 8))
#define IPC_BITTIMING_UNPACK_BRP(w0)       ((uint16_t)((w0) & 0xFFFFU))
#define IPC_BITTIMING_UNPACK_TSEG1(w0)     ((uint16_t)((w0) >> 16))
#define IPC_BITTIMING_UNPACK_TSEG2(w1)     ((uint8_t)((w1) & 0xFFU))
#define IPC_BITTIMING_UNPACK_SJW(w1)       ((uint8_t)(((w1) >> 8) & 0xFFU))

#endif /* _IPC_H_ */
