/* SPDX-License-Identifier: GPL-2.0 */
/*
 * lw_can_protocol.h — kernel-side mirror of the device wire protocol.
 *
 * This deliberately does NOT include the firmware's common/usb_can_protocol.h:
 * that header pulls in host headers (lw_mcp251xfd/can.h) and host types. We
 * redefine the same layout here with kernel fixed-width (__le32 / __u8) types.
 * The static_assert below enforces the fixed 84-byte wire size; if it fails the layout has
 * drifted from the firmware and frames will not parse.
 *
 * Keep this in sync with src/common/usb_can_protocol.h.
 */

#ifndef _LW_CAN_PROTOCOL_H_
#define _LW_CAN_PROTOCOL_H_

#include <linux/types.h>
#include <linux/build_bug.h>

/* Device identification — must match the firmware (placeholders for now). */
#define LW_CAN_VENDOR_ID  0xCAFE
#define LW_CAN_PRODUCT_ID 0x0CDA

/* MCP251xFD clock feeding the CAN bit-timing logic (see firmware fosc). */
#define LW_CAN_CLOCK_HZ 40000000

/* Vendor control requests (bmRequestType = 0x40, host-to-device, device recipient). */
#define LW_CAN_REQ_OPEN            0x01 /* no data */
#define LW_CAN_REQ_CLOSE           0x02 /* no data */
#define LW_CAN_REQ_SET_BITTIMING   0x03 /* data = struct lw_can_bittiming */
#define LW_CAN_REQ_SET_MODE        0x04 /* wValue = mcp251xfd_opmode_t */
#define LW_CAN_REQ_SET_TERMINATION 0x05 /* wValue = 1 (on) / 0 (off) */

/* Start-of-frame marker: first field of every bulk packet (little-endian 0x55,0xAA). */
#define LW_CAN_SOF 0xAA55

/* Bulk packet type tag. */
#define LW_CAN_MSG_FRAME 0x00
#define LW_CAN_MSG_ERROR 0x01

/* can_frame_flags_t mirror. */
#define LW_CAN_FLAG_EFF 0x01 /* extended frame */
#define LW_CAN_FLAG_FDF 0x02 /* CAN FD */
#define LW_CAN_FLAG_BRS 0x04 /* bit-rate switch */
#define LW_CAN_FLAG_ESI 0x08 /* error state indicator */

/* Payload for LW_CAN_REQ_SET_BITTIMING (8 bytes). */
struct lw_can_bittiming {
	__le32 nominal_baud;
	__le32 data_baud;
};

/*
 * CAN frame as carried on the wire. Mirrors the firmware's can_frame_t, which is
 * NOT packed and therefore 72 bytes (id 4 + flags 1 + dlc 1 + data 64, padded to
 * a multiple of 4). Do not add __packed here — that would make it 70 and break
 * the layout.
 */
struct lw_can_frame {
	__le32 id;
	__u8   flags;
	__u8   dlc;
	__u8   data[64];
};

/* Bus error / state event. Mirrors the firmware's __packed usb_can_error_t. */
struct lw_can_error {
	__u8   rec;
	__u8   tec;
	__u8   error_warn;
	__u8   rx_passive;
	__u8   tx_passive;
	__u8   bus_off;
	__u8   rx_overflow;
	__u8   nominal_rx_errors;
	__u8   nominal_tx_errors;
	__u8   data_rx_errors;
	__u8   data_tx_errors;
	__le16 error_frame_count;
	__u8   nbit0_err;
	__u8   nbit1_err;
	__u8   nack_err;
	__u8   nform_err;
	__u8   nstuff_err;
	__u8   ncrc_err;
	__u8   dbit0_err;
	__u8   dbit1_err;
	__u8   dform_err;
	__u8   dstuff_err;
	__u8   dcrc_err;
	__u8   txbo_err;
	__u8   dlc_mismatch;
} __packed;

/*
 * Bulk packet (both directions). Field order keeps every member naturally
 * aligned, so the struct is a fixed 84 bytes without packing — matching the
 * firmware. The crc covers all bytes before it (offsetof(crc)), sof included.
 */
struct lw_can_packet {
	__le16 sof;
	__u8   type;
	__u8   _reserved;
	__le32 timestamp_us;
	union {
		struct lw_can_frame frame;
		struct lw_can_error error;
	} payload;
	__le32 crc;
};

static_assert(sizeof(struct lw_can_packet) == 84,
	      "lw_can_packet must be 84 bytes to match the firmware wire format");

#endif /* _LW_CAN_PROTOCOL_H_ */
