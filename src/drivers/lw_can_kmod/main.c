// SPDX-License-Identifier: GPL-2.0
/*
 * lw_can — SocketCAN driver for the LW USB CAN-FD module.
 *
 * Data path:
 *   RX  — a bulk-IN URB streams 84-byte packets; a byte-wise assembler re-syncs on
 *         the SOF marker, validates CRC, and dispatches frames, bus-error events and
 *         transmit confirmations.
 *   TX  — ndo_start_xmit builds a packet (sof + crc), stores the skb in an echo slot
 *         and tags the packet with that slot as the cookie, then submits a bulk-OUT
 *         URB. The echo is completed when the device returns a matching TX_EVENT.
 *
 * Author: Leo Walker
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/usb.h>
#include <linux/netdevice.h>
#include <linux/crc32.h>

#include <linux/can.h>
#include <linux/can/dev.h>
#include <linux/can/error.h>
#include <linux/can/length.h>
#include <linux/can/skb.h>

#include "lw_can_protocol.h"

#define LW_CAN_MAX_TX_URBS  10  /* == echo_skb_max passed to alloc_candev() */
#define LW_CAN_RX_BUFFER_SZ 512

/* Per-transmit context. echo_index doubles as the echo-skb slot; ==
 * LW_CAN_MAX_TX_URBS marks the slot free. */
struct lw_can_tx_context {
	struct lw_can *priv;
	u32 echo_index;
};

struct lw_can {
	struct can_priv    can; /* MUST be first — alloc_candev()/netdev_priv() */
	struct usb_device *udev;
	struct net_device *netdev;
	__u8 ep_in;             /* bulk IN  endpoint address (device -> host) */
	__u8 ep_out;            /* bulk OUT endpoint address (host -> device) */

	struct usb_anchor rx_submitted;
	struct usb_anchor tx_submitted;

	/* RX byte-stream assembler (single RX URB, so no locking needed). */
	union {
		struct lw_can_packet pkt;
		u8 bytes[sizeof(struct lw_can_packet)];
	} rx_asm;
	u32 rx_asm_len;

	struct lw_can_tx_context tx_contexts[LW_CAN_MAX_TX_URBS];
	spinlock_t tx_ctx_lock;
	atomic_t tx_active;     /* number of in-flight TX frames */
};

/*
 * Bit-timing limits. The kernel computes brp/seg/sjw from the requested bitrate,
 * LW_CAN_CLOCK_HZ and these constants, then hands us the segments to program.
 *
 * Nominal (CINBTCFG): TSEG1 <= 256, TSEG2 <= 128, SJW <= 128, BRP <= 256.
 * Data    (CIDBTCFG): TSEG1 <= 32,  TSEG2 <= 16,  SJW <= 16,  BRP <= 256.
 */
static const struct can_bittiming_const lw_can_bittiming_const = {
	.name      = KBUILD_MODNAME,
	.tseg1_min = 2,
	.tseg1_max = 256,
	.tseg2_min = 1,
	.tseg2_max = 128,
	.sjw_max   = 128,
	.brp_min   = 1,
	.brp_max   = 256,
	.brp_inc   = 1,
};

static const struct can_bittiming_const lw_can_data_bittiming_const = {
	.name      = KBUILD_MODNAME,
	.tseg1_min = 1,
	.tseg1_max = 32,
	.tseg2_min = 1,
	.tseg2_max = 16,
	.sjw_max   = 16,
	.brp_min   = 1,
	.brp_max   = 256,
	.brp_inc   = 1,
};

/* CRC-32 (IEEE 802.3) over every byte before the crc field, matching the firmware. */
static u32 lw_can_crc(const struct lw_can_packet *pkt)
{
	return crc32_le(~0u, (const u8 *)pkt,
			offsetof(struct lw_can_packet, crc)) ^ ~0u;
}

/* ---- Control transfers --------------------------------------------------- */

static int lw_can_ctrl(struct lw_can *priv, __u8 request, __u16 value,
		       const void *data, __u16 size)
{
	return usb_control_msg_send(priv->udev, 0, request,
				    USB_DIR_OUT | USB_TYPE_VENDOR | USB_RECIP_DEVICE,
				    value, 0, data, size, 1000, GFP_KERNEL);
}

/*
 * Send register-level bit-timing segments to the device. The MCP251xFD encodes
 * the (actual-1) form itself; we forward the kernel-computed time-quanta counts.
 * tseg1 carries prop_seg + phase_seg1 (the controller has no separate prop seg).
 */
static int lw_can_send_bittiming(struct lw_can *priv, __u8 request,
				 const struct can_bittiming *bt)
{
	struct lw_can_bittiming payload = {
		.brp   = cpu_to_le16(bt->brp),
		.tseg1 = cpu_to_le16(bt->prop_seg + bt->phase_seg1),
		.tseg2 = bt->phase_seg2,
		.sjw   = bt->sjw,
	};

	return lw_can_ctrl(priv, request, 0, &payload, sizeof(payload));
}

static int lw_can_set_bittiming(struct net_device *netdev)
{
	struct lw_can *priv = netdev_priv(netdev);

	return lw_can_send_bittiming(priv, LW_CAN_REQ_SET_BITTIMING,
				     &priv->can.bittiming);
}

static int lw_can_set_data_bittiming(struct net_device *netdev)
{
	struct lw_can *priv = netdev_priv(netdev);

	return lw_can_send_bittiming(priv, LW_CAN_REQ_SET_DATA_BITTIMING,
				     &priv->can.fd.data_bittiming);
}

/* ---- TX context pool ----------------------------------------------------- */

static void lw_can_init_tx_contexts(struct lw_can *priv)
{
	int i;

	spin_lock_init(&priv->tx_ctx_lock);
	atomic_set(&priv->tx_active, 0);
	for (i = 0; i < LW_CAN_MAX_TX_URBS; i++) {
		priv->tx_contexts[i].priv = priv;
		priv->tx_contexts[i].echo_index = LW_CAN_MAX_TX_URBS; /* free */
	}
}

static struct lw_can_tx_context *lw_can_get_tx_context(struct lw_can *priv)
{
	struct lw_can_tx_context *ctx = NULL;
	unsigned long flags;
	int i;

	spin_lock_irqsave(&priv->tx_ctx_lock, flags);
	for (i = 0; i < LW_CAN_MAX_TX_URBS; i++) {
		if (priv->tx_contexts[i].echo_index == LW_CAN_MAX_TX_URBS) {
			priv->tx_contexts[i].echo_index = i;
			ctx = &priv->tx_contexts[i];
			break;
		}
	}
	spin_unlock_irqrestore(&priv->tx_ctx_lock, flags);
	return ctx;
}

static void lw_can_put_tx_context(struct lw_can_tx_context *ctx)
{
	struct lw_can *priv = ctx->priv;
	unsigned long flags;

	spin_lock_irqsave(&priv->tx_ctx_lock, flags);
	ctx->echo_index = LW_CAN_MAX_TX_URBS;
	spin_unlock_irqrestore(&priv->tx_ctx_lock, flags);
}

/* Free echo skbs for frames delivered but never confirmed (e.g. on close). */
static void lw_can_flush_tx_contexts(struct lw_can *priv)
{
	int i;

	for (i = 0; i < LW_CAN_MAX_TX_URBS; i++) {
		if (priv->tx_contexts[i].echo_index != LW_CAN_MAX_TX_URBS) {
			can_free_echo_skb(priv->netdev, i, NULL);
			priv->tx_contexts[i].echo_index = LW_CAN_MAX_TX_URBS;
		}
	}
	atomic_set(&priv->tx_active, 0);
}

/* ---- RX path ------------------------------------------------------------- */

static void lw_can_rx_frame(struct lw_can *priv, const struct lw_can_packet *pkt)
{
	struct net_device *netdev = priv->netdev;
	const struct lw_can_frame *f = &pkt->payload.frame;
	struct sk_buff *skb;
	struct canfd_frame *cfd;
	struct can_frame *cf;
	bool rtr = f->flags & LW_CAN_FLAG_RTR;          /* classic-only remote request */
	bool fd = (f->flags & LW_CAN_FLAG_FDF) && !rtr;
	bool eff = f->flags & LW_CAN_FLAG_EFF;
	u32 can_id = le32_to_cpu(f->id) | (eff ? CAN_EFF_FLAG : 0);
	u8 len;

	if (fd) {
		len = can_fd_dlc2len(f->dlc);
		skb = alloc_canfd_skb(netdev, &cfd);
		if (!skb) {
			netdev->stats.rx_dropped++;
			return;
		}
		cfd->can_id = can_id;
		cfd->len = len;
		if (f->flags & LW_CAN_FLAG_BRS)
			cfd->flags |= CANFD_BRS;
		if (f->flags & LW_CAN_FLAG_ESI)
			cfd->flags |= CANFD_ESI;
		memcpy(cfd->data, f->data, len);
	} else {
		len = can_cc_dlc2len(f->dlc);
		skb = alloc_can_skb(netdev, &cf);
		if (!skb) {
			netdev->stats.rx_dropped++;
			return;
		}
		cf->can_id = can_id;
		cf->len = len;
		if (rtr)
			cf->can_id |= CAN_RTR_FLAG; /* remote frame: id + dlc, no data */
		else
			memcpy(cf->data, f->data, len);
	}

	netdev->stats.rx_packets++;
	netdev->stats.rx_bytes += len;
	netif_rx(skb);
}

static void lw_can_rx_error(struct lw_can *priv, const struct lw_can_packet *pkt)
{
	struct net_device *netdev = priv->netdev;
	const struct lw_can_error *e = &pkt->payload.error;
	enum can_state tx_state, rx_state;
	struct can_frame *cf;
	struct sk_buff *skb;

	if (e->bus_off) {
		tx_state = rx_state = CAN_STATE_BUS_OFF;
	} else {
		tx_state = e->tx_passive ? CAN_STATE_ERROR_PASSIVE :
			   (e->tec >= 96 ? CAN_STATE_ERROR_WARNING :
					   CAN_STATE_ERROR_ACTIVE);
		rx_state = e->rx_passive ? CAN_STATE_ERROR_PASSIVE :
			   (e->rec >= 96 ? CAN_STATE_ERROR_WARNING :
					   CAN_STATE_ERROR_ACTIVE);
	}

	skb = alloc_can_err_skb(netdev, &cf);

	if (e->bus_off) {
		if (priv->can.state != CAN_STATE_BUS_OFF) {
			if (cf)
				cf->can_id |= CAN_ERR_BUSOFF;
			can_bus_off(netdev);
		}
	} else {
		/* Updates priv->can.state, can_stats and the CAN_ERR_CRTL bits. */
		can_change_state(netdev, cf, tx_state, rx_state);
	}

	if (e->rx_overflow || e->sw_overflow) {
		netdev->stats.rx_over_errors++;
		netdev->stats.rx_errors++;
		if (cf) {
			cf->can_id |= CAN_ERR_CRTL;
			cf->data[1] |= CAN_ERR_CRTL_RX_OVERFLOW;
		}
	}

	if (!skb)
		return;

	/* Always report the raw error counters. */
	cf->can_id |= CAN_ERR_CNT;
	cf->data[6] = e->tec;
	cf->data[7] = e->rec;

	netif_rx(skb);
}

static void lw_can_rx_tx_event(struct lw_can *priv, const struct lw_can_packet *pkt)
{
	struct net_device *netdev = priv->netdev;
	u32 cookie = le32_to_cpu(pkt->payload.tx_event.cookie);

	if (cookie >= LW_CAN_MAX_TX_URBS) {
		netdev_warn_once(netdev, "TX event with bad cookie %u\n", cookie);
		return;
	}
	if (priv->tx_contexts[cookie].echo_index != cookie)
		return; /* slot already freed (e.g. after bus-off) */

	netdev->stats.tx_packets++;
	netdev->stats.tx_bytes += can_get_echo_skb(netdev, cookie, NULL);
	lw_can_put_tx_context(&priv->tx_contexts[cookie]);
	atomic_dec(&priv->tx_active);
	netif_wake_queue(netdev);
}

static void lw_can_handle_packet(struct lw_can *priv, const struct lw_can_packet *pkt)
{
	switch (pkt->type) {
	case LW_CAN_MSG_FRAME:
		lw_can_rx_frame(priv, pkt);
		break;
	case LW_CAN_MSG_ERROR:
		lw_can_rx_error(priv, pkt);
		break;
	case LW_CAN_MSG_TX_EVENT:
		lw_can_rx_tx_event(priv, pkt);
		break;
	default:
		break;
	}
}

/* Feed one received byte into the packet assembler, re-syncing on SOF. */
static void lw_can_rx_byte(struct lw_can *priv, u8 b)
{
	priv->rx_asm.bytes[priv->rx_asm_len++] = b;

	if (priv->rx_asm_len == 1 && b != (LW_CAN_SOF & 0xFF)) {
		priv->rx_asm_len = 0;
		return;
	}
	if (priv->rx_asm_len == 2 && b != (LW_CAN_SOF >> 8)) {
		/* Second byte wrong: this byte may itself start a new SOF. */
		priv->rx_asm.bytes[0] = b;
		priv->rx_asm_len = (b == (LW_CAN_SOF & 0xFF)) ? 1 : 0;
		return;
	}
	if (priv->rx_asm_len < sizeof(struct lw_can_packet))
		return;

	if (lw_can_crc(&priv->rx_asm.pkt) == le32_to_cpu(priv->rx_asm.pkt.crc))
		lw_can_handle_packet(priv, &priv->rx_asm.pkt);
	priv->rx_asm_len = 0;
}

static void lw_can_read_bulk_callback(struct urb *urb)
{
	struct lw_can *priv = urb->context;
	struct net_device *netdev = priv->netdev;
	const u8 *buf = urb->transfer_buffer;
	u32 i;
	int err;

	switch (urb->status) {
	case 0:
		break;
	case -ENOENT:
	case -EPIPE:
	case -EPROTO:
	case -ESHUTDOWN:
		/* Unlinked / device gone: free our buffer (the core frees the URB). */
		usb_free_coherent(priv->udev, LW_CAN_RX_BUFFER_SZ,
				  urb->transfer_buffer, urb->transfer_dma);
		return;
	default:
		netdev_info(netdev, "RX urb aborted: %d\n", urb->status);
		goto resubmit;
	}

	for (i = 0; i < urb->actual_length; i++)
		lw_can_rx_byte(priv, buf[i]);

resubmit:
	usb_anchor_urb(urb, &priv->rx_submitted);
	err = usb_submit_urb(urb, GFP_ATOMIC);
	if (err) {
		usb_unanchor_urb(urb);
		/* No resubmit: free the buffer; the core frees the URB after we return. */
		usb_free_coherent(priv->udev, LW_CAN_RX_BUFFER_SZ,
				  urb->transfer_buffer, urb->transfer_dma);
		if (err == -ENODEV)
			netif_device_detach(netdev);
		else
			netdev_warn(netdev, "RX resubmit failed: %d\n", err);
	}
}

static int lw_can_start_rx(struct lw_can *priv)
{
	struct urb *urb;
	void *buf;
	int err;

	urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!urb)
		return -ENOMEM;

	buf = usb_alloc_coherent(priv->udev, LW_CAN_RX_BUFFER_SZ, GFP_KERNEL,
				 &urb->transfer_dma);
	if (!buf) {
		usb_free_urb(urb);
		return -ENOMEM;
	}

	usb_fill_bulk_urb(urb, priv->udev,
			  usb_rcvbulkpipe(priv->udev, priv->ep_in),
			  buf, LW_CAN_RX_BUFFER_SZ, lw_can_read_bulk_callback, priv);
	urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
	usb_anchor_urb(urb, &priv->rx_submitted);

	err = usb_submit_urb(urb, GFP_KERNEL);
	if (err) {
		usb_unanchor_urb(urb);
		usb_free_coherent(priv->udev, LW_CAN_RX_BUFFER_SZ, buf,
				  urb->transfer_dma);
		usb_free_urb(urb);
		return err;
	}

	/* The URB is held by the anchor; drop our local reference. */
	usb_free_urb(urb);
	priv->rx_asm_len = 0;
	return 0;
}

/* ---- TX path ------------------------------------------------------------- */

static void lw_can_write_bulk_callback(struct urb *urb)
{
	struct lw_can_tx_context *ctx = urb->context;
	struct lw_can *priv = ctx->priv;
	struct net_device *netdev = priv->netdev;

	usb_free_coherent(urb->dev, urb->transfer_buffer_length,
			  urb->transfer_buffer, urb->transfer_dma);

	if (urb->status == 0)
		return; /* delivered to device; await TX_EVENT to complete the echo */

	/* Failed or unlinked: complete the echo now so it does not leak. */
	if (ctx->echo_index != LW_CAN_MAX_TX_URBS) {
		can_free_echo_skb(netdev, ctx->echo_index, NULL);
		lw_can_put_tx_context(ctx);
		atomic_dec(&priv->tx_active);
		netif_wake_queue(netdev);
	}
}

static netdev_tx_t lw_can_start_xmit(struct sk_buff *skb,
				     struct net_device *netdev)
{
	struct lw_can *priv = netdev_priv(netdev);
	struct canfd_frame *cfd = (struct canfd_frame *)skb->data;
	struct lw_can_tx_context *ctx;
	struct lw_can_packet *pkt;
	struct urb *urb;
	bool fd, eff, rtr;
	int err;

	if (can_dev_dropped_skb(netdev, skb))
		return NETDEV_TX_OK;

	ctx = lw_can_get_tx_context(priv);
	if (!ctx) {
		/* Safety net; we stop the queue when the last slot is taken. */
		netif_stop_queue(netdev);
		return NETDEV_TX_BUSY;
	}

	urb = usb_alloc_urb(0, GFP_ATOMIC);
	if (!urb)
		goto err_free_ctx;

	pkt = usb_alloc_coherent(priv->udev, sizeof(*pkt), GFP_ATOMIC,
				 &urb->transfer_dma);
	if (!pkt)
		goto err_free_urb;

	fd = can_is_canfd_skb(skb);
	eff = cfd->can_id & CAN_EFF_FLAG;
	rtr = !fd && (cfd->can_id & CAN_RTR_FLAG); /* remote request: classic only */

	memset(pkt, 0, sizeof(*pkt));
	pkt->sof = cpu_to_le16(LW_CAN_SOF);
	pkt->type = LW_CAN_MSG_FRAME;
	pkt->timestamp_us = cpu_to_le32(ctx->echo_index); /* echo cookie */
	pkt->payload.frame.id =
		cpu_to_le32(cfd->can_id & (eff ? CAN_EFF_MASK : CAN_SFF_MASK));
	pkt->payload.frame.flags = eff ? LW_CAN_FLAG_EFF : 0;
	if (fd) {
		pkt->payload.frame.flags |= LW_CAN_FLAG_FDF;
		if (cfd->flags & CANFD_BRS)
			pkt->payload.frame.flags |= LW_CAN_FLAG_BRS;
		if (cfd->flags & CANFD_ESI)
			pkt->payload.frame.flags |= LW_CAN_FLAG_ESI;
		pkt->payload.frame.dlc = can_fd_len2dlc(cfd->len);
	} else {
		pkt->payload.frame.dlc = cfd->len; /* classic: len == dlc (0-8) */
	}
	if (rtr)
		pkt->payload.frame.flags |= LW_CAN_FLAG_RTR; /* no data bytes for a remote frame */
	else
		memcpy(pkt->payload.frame.data, cfd->data, cfd->len);
	pkt->crc = cpu_to_le32(lw_can_crc(pkt));

	/* Stash the skb for local echo; completed by the device's TX_EVENT. */
	can_put_echo_skb(skb, netdev, ctx->echo_index, 0);

	usb_fill_bulk_urb(urb, priv->udev,
			  usb_sndbulkpipe(priv->udev, priv->ep_out),
			  pkt, sizeof(*pkt), lw_can_write_bulk_callback, ctx);
	urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
	usb_anchor_urb(urb, &priv->tx_submitted);

	/* Stop the queue once every echo slot is in use. */
	if (atomic_inc_return(&priv->tx_active) >= LW_CAN_MAX_TX_URBS)
		netif_stop_queue(netdev);

	err = usb_submit_urb(urb, GFP_ATOMIC);
	if (err) {
		can_free_echo_skb(netdev, ctx->echo_index, NULL);
		atomic_dec(&priv->tx_active);
		usb_unanchor_urb(urb);
		usb_free_coherent(priv->udev, sizeof(*pkt), pkt, urb->transfer_dma);
		usb_free_urb(urb);
		lw_can_put_tx_context(ctx);
		if (err == -ENODEV) {
			netif_device_detach(netdev);
		} else {
			netdev_warn(netdev, "TX submit failed: %d\n", err);
			netif_wake_queue(netdev); /* a slot freed up again */
		}
		return NETDEV_TX_OK;
	}

	usb_free_urb(urb); /* held by the anchor */
	return NETDEV_TX_OK;

err_free_urb:
	usb_free_urb(urb);
err_free_ctx:
	lw_can_put_tx_context(ctx);
	dev_kfree_skb(skb);
	netdev->stats.tx_dropped++;
	return NETDEV_TX_OK;
}

/* ---- netdev ops ---------------------------------------------------------- */

static int lw_can_open(struct net_device *netdev)
{
	struct lw_can *priv = netdev_priv(netdev);
	int err;

	err = open_candev(netdev);
	if (err)
		return err;

	err = lw_can_start_rx(priv);
	if (err) {
		netdev_err(netdev, "failed to start RX: %d\n", err);
		goto err_close;
	}

	err = lw_can_ctrl(priv, LW_CAN_REQ_OPEN, 0, NULL, 0);
	if (err) {
		netdev_err(netdev, "OPEN request failed: %d\n", err);
		goto err_kill_rx;
	}

	netif_start_queue(netdev);
	return 0;

err_kill_rx:
	usb_kill_anchored_urbs(&priv->rx_submitted);
err_close:
	close_candev(netdev);
	return err;
}

static int lw_can_close(struct net_device *netdev)
{
	struct lw_can *priv = netdev_priv(netdev);

	netif_stop_queue(netdev);

	lw_can_ctrl(priv, LW_CAN_REQ_CLOSE, 0, NULL, 0);

	usb_kill_anchored_urbs(&priv->tx_submitted);
	usb_kill_anchored_urbs(&priv->rx_submitted);
	lw_can_flush_tx_contexts(priv);

	close_candev(netdev);
	return 0;
}

/*
 * Restart after bus-off (manual `ip link ... restart`, or automatic via restart-ms).
 * The device stays bus-off until told, so command the recovery; also clear echo slots
 * stuck from the bus-off, whose TX_EVENTs the firmware dropped.
 */
static int lw_can_set_mode(struct net_device *netdev, enum can_mode mode)
{
	struct lw_can *priv = netdev_priv(netdev);
	int err;

	switch (mode) {
	case CAN_MODE_START:
		usb_kill_anchored_urbs(&priv->tx_submitted);
		lw_can_flush_tx_contexts(priv);

		err = lw_can_ctrl(priv, LW_CAN_REQ_RESTART, 0, NULL, 0);
		if (err) {
			netdev_err(netdev, "restart request failed: %d\n", err);
			return err;
		}

		priv->can.state = CAN_STATE_ERROR_ACTIVE;
		netif_wake_queue(netdev);
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

/*
 * Recovery if the TX queue stalls: a frame was delivered to the device but its
 * TX_EVENT never came back (firmware drop / TEF overflow), leaving its echo slot
 * stuck. Cancel any in-flight OUT URBs, release the stuck echo skbs, and re-open
 * the queue. Runs in process context from the netdev watchdog.
 */
static void lw_can_tx_timeout(struct net_device *netdev, unsigned int txqueue)
{
	struct lw_can *priv = netdev_priv(netdev);

	netdev_warn(netdev, "TX timeout, flushing pending transmits\n");
	netdev->stats.tx_errors++;

	usb_kill_anchored_urbs(&priv->tx_submitted);
	lw_can_flush_tx_contexts(priv);
	netif_wake_queue(netdev);
}

static const struct net_device_ops lw_can_netdev_ops = {
	.ndo_open       = lw_can_open,
	.ndo_stop       = lw_can_close,
	.ndo_start_xmit = lw_can_start_xmit,
	.ndo_tx_timeout = lw_can_tx_timeout,
	/* MTU is handled by the CAN core; for FD, call can_set_default_mtu()
	 * after enabling CAN_CTRLMODE_FD (milestone 5). */
};

/* ---- USB probe / disconnect ---------------------------------------------- */

static int lw_can_probe(struct usb_interface *intf,
			const struct usb_device_id *id)
{
	struct usb_endpoint_descriptor *ep_in, *ep_out;
	struct net_device *netdev;
	struct lw_can *priv;
	int err;

	err = usb_find_common_endpoints(intf->cur_altsetting,
					&ep_in, &ep_out, NULL, NULL);
	if (err) {
		dev_err(&intf->dev, "bulk IN/OUT endpoints not found\n");
		return err;
	}

	netdev = alloc_candev(sizeof(struct lw_can), LW_CAN_MAX_TX_URBS);
	if (!netdev)
		return -ENOMEM;

	priv = netdev_priv(netdev);
	priv->udev   = usb_get_dev(interface_to_usbdev(intf));
	priv->netdev = netdev;
	priv->ep_in  = ep_in->bEndpointAddress;
	priv->ep_out = ep_out->bEndpointAddress;

	init_usb_anchor(&priv->rx_submitted);
	init_usb_anchor(&priv->tx_submitted);
	lw_can_init_tx_contexts(priv);

	priv->can.clock.freq            = LW_CAN_CLOCK_HZ;
	priv->can.bittiming_const       = &lw_can_bittiming_const;
	priv->can.do_set_bittiming      = lw_can_set_bittiming;
	priv->can.fd.data_bittiming_const  = &lw_can_data_bittiming_const;
	priv->can.fd.do_set_data_bittiming = lw_can_set_data_bittiming;
	priv->can.do_set_mode           = lw_can_set_mode;
	priv->can.ctrlmode_supported    = CAN_CTRLMODE_FD;
	/* TODO: also LISTENONLY | LOOPBACK | ONE_SHOT once the firmware maps them. */

	netdev->netdev_ops = &lw_can_netdev_ops;
	netdev->watchdog_timeo = HZ;        /* recover a TX queue stalled on a lost TX_EVENT */
	netdev->flags |= IFF_ECHO;          /* enable local echo for TX path */

	SET_NETDEV_DEV(netdev, &intf->dev);
	usb_set_intfdata(intf, priv);

	err = register_candev(netdev);
	if (err) {
		dev_err(&intf->dev, "register_candev failed: %d\n", err);
		usb_put_dev(priv->udev);
		free_candev(netdev);
		return err;
	}

	netdev_info(netdev, "lw_can registered (%s)\n", netdev->name);
	return 0;
}

static void lw_can_disconnect(struct usb_interface *intf)
{
	struct lw_can *priv = usb_get_intfdata(intf);

	usb_set_intfdata(intf, NULL);
	if (!priv)
		return;

	unregister_candev(priv->netdev);
	usb_kill_anchored_urbs(&priv->tx_submitted);
	usb_kill_anchored_urbs(&priv->rx_submitted);
	usb_put_dev(priv->udev);
	free_candev(priv->netdev);
}

static const struct usb_device_id lw_can_table[] = {
	{ USB_DEVICE(LW_CAN_VENDOR_ID, LW_CAN_PRODUCT_ID) },
	{ }
};
MODULE_DEVICE_TABLE(usb, lw_can_table);

static struct usb_driver lw_can_driver = {
	.name       = KBUILD_MODNAME,
	.probe      = lw_can_probe,
	.disconnect = lw_can_disconnect,
	.id_table   = lw_can_table,
};

module_usb_driver(lw_can_driver);

MODULE_AUTHOR("Leo Walker");
MODULE_DESCRIPTION("SocketCAN driver for the LW USB CAN-FD module");
MODULE_LICENSE("GPL");
