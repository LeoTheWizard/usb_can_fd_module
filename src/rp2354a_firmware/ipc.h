/**
 * @file ipc.h
 * @brief Inter-core command definitions for core 1 → core 0 communication.
 *
 * Commands are sent through the RP2350 inter-core FIFO (multicore_fifo).
 * Each command is one 32-bit word. Commands with a payload push additional
 * words immediately after the command word; core 0 pops them in order.
 *
 * Protocol per command:
 *   IPC_CMD_OPEN         — [cmd]
 *   IPC_CMD_CLOSE        — [cmd]
 *   IPC_CMD_SET_MODE     — [cmd] [uint32_t opmode (mcp251xfd_opmode_t value)]
 *   IPC_CMD_SET_BITTIMING — [cmd] [uint32_t nominal_baud] [uint32_t data_baud]
 *
 * @author Leo Walker
 * @copyright (c) Leo Walker 2025 @ MIT License @cite MIT License
 */

#ifndef _IPC_H_
#define _IPC_H_

#define IPC_CMD_OPEN             0x01U
#define IPC_CMD_CLOSE            0x02U
#define IPC_CMD_SET_MODE         0x03U
#define IPC_CMD_SET_BITTIMING    0x04U
#define IPC_CMD_SET_TERMINATION  0x05U /**< Payload: one word — 1 = enable, 0 = disable. */

#endif /* _IPC_H_ */
