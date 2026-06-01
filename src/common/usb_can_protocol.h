/**
 * @file usb_can_protocol.h
 * @brief USB CAN-FD adapter wire protocol.
 *
 * Shared between the RP2350 firmware and host-side userspace tools (Linux, Windows).
 * All multi-byte fields are little-endian to match USB convention.
 *
 * Note: Linux kernel drivers cannot include this header directly because it
 * depends on lw_mcp251xfd/can.h. Kernel drivers should define their own
 * equivalent structs using __le32 / __u8 kernel types.
 *
 * Topology:
 *   EP0  (control) — configuration commands from host to device
 *   EP1 OUT (bulk) — host → device: CAN frames to transmit
 *   EP1 IN  (bulk) — device → host: received frames and bus events
 *
 * @author Leo Walker
 * @copyright (c) Leo Walker 2025 @ MIT License @cite MIT License
 */

#ifndef _USB_CAN_PROTOCOL_H_
#define _USB_CAN_PROTOCOL_H_

#include <stdint.h>
#include <lw_mcp251xfd/can.h>

// ---- Device identification -------------------------------------------------

/** TODO: Replace with a registered VID/PID pair before shipping. */
#define USB_CAN_VID 0xCAFE
#define USB_CAN_PID 0x0CDA

// ---- Control endpoint (EP0) ------------------------------------------------

/**
 * @brief Vendor-specific bRequest codes.
 * bmRequestType = 0x40 (vendor, host-to-device, device recipient) for all.
 */
#define USB_CAN_REQ_OPEN          0x01 /**< Open the CAN bus. No data payload. */
#define USB_CAN_REQ_CLOSE         0x02 /**< Close the CAN bus. No data payload. */
#define USB_CAN_REQ_SET_BITTIMING 0x03 /**< Set bitrates. Data = usb_can_bittiming_t. */
#define USB_CAN_REQ_SET_MODE      0x04 /**< Set operating mode. wValue = mcp251xfd_opmode_t. */

/**
 * @brief Payload for USB_CAN_REQ_SET_BITTIMING (8 bytes, host-to-device).
 * Both fields carry a can_baudrates_t value in little-endian byte order.
 */
typedef struct __attribute__((packed)) usb_can_bittiming
{
    uint32_t nominal_baud; /**< Nominal (arbitration) phase baud rate. */
    uint32_t data_baud;    /**< Data phase baud rate (CAN FD only). */
} usb_can_bittiming_t;

// ---- Bulk endpoints (EP1 IN / EP1 OUT) ------------------------------------

/**
 * @brief Message type tag in usb_can_packet_t.type.
 * Determines which member of the payload union is valid.
 * Bulk OUT packets from the host are always USB_CAN_MSG_FRAME.
 */
#define USB_CAN_MSG_FRAME 0x00 /**< CAN data frame; payload.frame is valid. */
#define USB_CAN_MSG_ERROR 0x01 /**< Bus error event; payload.error is valid. */

/**
 * @brief Bus error event payload.
 * Carried in usb_can_packet_t.frame.data when type == USB_CAN_MSG_ERROR.
 * Fields mirror mcp251xfd_error_state_t (CiTREC) and mcp251xfd_diagnostics_t (CiBDIAG0-1).
 */
typedef struct __attribute__((packed)) usb_can_error
{
    /* Bus state — from CiTREC */
    uint8_t  rec;               /**< Receive error counter (0-255). */
    uint8_t  tec;               /**< Transmit error counter (0-255). */
    uint8_t  error_warn;        /**< TEC or REC >= 96. */
    uint8_t  rx_passive;        /**< REC >= 128 — receive error-passive. */
    uint8_t  tx_passive;        /**< TEC >= 128 — transmit error-passive. */
    uint8_t  bus_off;           /**< TEC > 255 — node is bus-off, no TX possible. */
    uint8_t  rx_overflow;       /**< RX FIFO dropped at least one frame since last read. */
    /* Diagnostic counters — from CiBDIAG0-1, cleared on read */
    uint8_t  nominal_rx_errors; /**< Nominal phase RX error count. */
    uint8_t  nominal_tx_errors; /**< Nominal phase TX error count. */
    uint8_t  data_rx_errors;    /**< Data phase RX error count. */
    uint8_t  data_tx_errors;    /**< Data phase TX error count. */
    uint16_t error_frame_count; /**< Total error frames detected. */
    /* Per-type error flags */
    uint8_t  nbit0_err;         /**< Nominal phase: dominant bit error. */
    uint8_t  nbit1_err;         /**< Nominal phase: recessive bit error. */
    uint8_t  nack_err;          /**< Nominal phase: ACK error. */
    uint8_t  nform_err;         /**< Nominal phase: form error. */
    uint8_t  nstuff_err;        /**< Nominal phase: stuff error. */
    uint8_t  ncrc_err;          /**< Nominal phase: CRC error. */
    uint8_t  dbit0_err;         /**< Data phase: dominant bit error. */
    uint8_t  dbit1_err;         /**< Data phase: recessive bit error. */
    uint8_t  dform_err;         /**< Data phase: form error. */
    uint8_t  dstuff_err;        /**< Data phase: stuff error. */
    uint8_t  dcrc_err;          /**< Data phase: CRC error. */
    uint8_t  txbo_err;          /**< Node entered bus-off since last read. */
    uint8_t  dlc_mismatch;      /**< Received DLC exceeded configured payload size. */
} usb_can_error_t;

/**
 * @brief Wire packet for both bulk endpoints.
 *
 * EP1 OUT (host → device): type must be USB_CAN_MSG_FRAME. timestamp_us is ignored.
 * EP1 IN  (device → host): type determines which payload member to read.
 *
 * Total size: 8 + sizeof(payload union) bytes.
 */
typedef struct __attribute__((packed)) usb_can_packet
{
    uint32_t timestamp_us; /**< Hardware RX timestamp; 0 for host TX requests. */
    uint8_t  type;         /**< USB_CAN_MSG_* — determines which payload member is valid. */
    uint8_t  _reserved[3];
    union {
        can_frame_t     frame; /**< Valid when type == USB_CAN_MSG_FRAME. */
        usb_can_error_t error; /**< Valid when type == USB_CAN_MSG_ERROR. */
    } payload;
} usb_can_packet_t;

#endif /* _USB_CAN_PROTOCOL_H_ */
