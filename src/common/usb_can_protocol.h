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
#include <stddef.h>
#include <lw_mcp251xfd/can.h>
#include "crc32.h"

// ---- Device identification -------------------------------------------------

/** TODO: Replace with a registered VID/PID pair before shipping. */
#define USB_CAN_VID 0xCAFE
#define USB_CAN_PID 0x0CDA

// ---- Control endpoint (EP0) ------------------------------------------------

/**
 * @brief Vendor-specific bRequest codes.
 * bmRequestType = 0x40 (vendor, host-to-device, device recipient) for all.
 */
/**
 * Open the CAN bus. No data payload. The device flushes its host→device (bulk OUT)
 * buffer on receipt so framing starts clean; the host should likewise discard any
 * buffered device→host (bulk IN) data until the first packet whose CRC validates.
 */
#define USB_CAN_REQ_OPEN             0x01
#define USB_CAN_REQ_CLOSE            0x02 /**< Close the CAN bus. No data payload. */
#define USB_CAN_REQ_SET_BITTIMING    0x03 /**< Set bitrates. Data = usb_can_bittiming_t. */
#define USB_CAN_REQ_SET_MODE         0x04 /**< Set operating mode. wValue = mcp251xfd_opmode_t. */
#define USB_CAN_REQ_SET_TERMINATION  0x05 /**< Enable/disable 120Ω termination. wValue = 1 (on) or 0 (off). */

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
 * @brief Start-of-frame marker; the first field of every bulk packet.
 *
 * Lets a desynchronised receiver re-align to a packet boundary: scan the byte
 * stream for this little-endian marker (0x55, 0xAA), then confirm the candidate
 * with the CRC before accepting the packet. USB bulk is lossless and ordered, so
 * this is only needed at session start or after a host-side partial read.
 */
#define USB_CAN_SOF 0xAA55

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
 * EP1 OUT (host → device): sof must be USB_CAN_SOF, type must be USB_CAN_MSG_FRAME,
 *                          timestamp_us is ignored.
 * EP1 IN  (device → host): type determines which payload member to read.
 *
 * Every packet begins with sof (USB_CAN_SOF) so a receiver can re-align to a
 * packet boundary after desync; see USB_CAN_SOF. The crc field is a CRC-32
 * (IEEE 802.3) checksum of all preceding bytes in the packet (i.e.
 * offsetof(usb_can_packet_t, crc) bytes starting at sof, so the marker is
 * covered too). Receivers must discard packets where the computed CRC does not
 * match crc, and re-align to the next sof.
 *
 * Field order keeps every member naturally aligned; total size is a fixed 84 bytes.
 */
typedef struct __attribute__((packed)) usb_can_packet
{
    uint16_t sof;          /**< Start-of-frame marker, always USB_CAN_SOF. */
    uint8_t  type;         /**< USB_CAN_MSG_* — determines which payload member is valid. */
    uint8_t  _reserved;
    uint32_t timestamp_us; /**< Hardware RX timestamp; 0 for host TX requests. */
    union {
        can_frame_t     frame; /**< Valid when type == USB_CAN_MSG_FRAME. */
        usb_can_error_t error; /**< Valid when type == USB_CAN_MSG_ERROR. */
    } payload;
    uint32_t crc; /**< CRC-32 of all bytes before this field (includes sof). */
} usb_can_packet_t;

/* The wire format is a fixed 84 bytes. A host that redefines this struct must
 * reproduce the same layout (note can_frame_t is padded to 72 bytes). */
#if defined(__cplusplus)
static_assert(sizeof(usb_can_packet_t) == 84, "usb_can_packet_t must be 84 bytes on the wire");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(usb_can_packet_t) == 84, "usb_can_packet_t must be 84 bytes on the wire");
#endif

/** Computes the CRC for a packet about to be sent. Fills pkt->crc in-place. */
static inline void usb_can_packet_set_crc(usb_can_packet_t *pkt)
{
    pkt->crc = crc32_compute((const uint8_t *)pkt, offsetof(usb_can_packet_t, crc));
}

/** Returns 1 if the packet CRC is valid, 0 if it is corrupt. */
static inline int usb_can_packet_check_crc(const usb_can_packet_t *pkt)
{
    uint32_t computed = crc32_compute((const uint8_t *)pkt, offsetof(usb_can_packet_t, crc));
    return computed == pkt->crc;
}

#endif /* _USB_CAN_PROTOCOL_H_ */
