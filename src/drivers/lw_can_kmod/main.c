// SPDX-License-Identifier: GPL-2.0
/*
 * lw_can — SocketCAN driver for the LW USB CAN-FD module.
 *
 * Milestone 1 scaffold: enumerates the device, registers a CAN netdev (canX),
 * and wires open/close to the device's control requests. The actual frame data
 * path (RX URBs, TX URBs + echo) is stubbed and marked with TODOs for the next
 * milestones.
 *
 * Author: Leo Walker
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/usb.h>
#include <linux/netdevice.h>
#include <linux/crc32.h>

#include <linux/can.h>
#include <linux/can/dev.h>

#include "lw_can_protocol.h"

/* Echo slots for can_put_echo_skb / can_get_echo_skb (used from milestone 3). */
#define LW_CAN_MAX_TX_URBS 10

struct lw_can {
	struct can_priv    can;     /* MUST be first — alloc_candev()/netdev_priv() */
	struct usb_device *udev;
	struct net_device *netdev;
	__u8 ep_in;                 /* bulk IN  endpoint address (device -> host) */
	__u8 ep_out;                /* bulk OUT endpoint address (host -> device) */
};

/*
 * Nominal bit-timing limits for the MCP251xFD (CINBTCFG): TSEG1 <= 256,
 * TSEG2 <= 128, SJW <= 128, BRP <= 256. The kernel uses these to compute
 * brp/seg/sjw from the requested bitrate and LW_CAN_CLOCK_HZ.
 *
 * TODO (milestone 5): add a data_bittiming_const and ctrlmode FD for CAN FD.
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

/* ---- Control transfers --------------------------------------------------- */

static int lw_can_ctrl(struct lw_can *priv, __u8 request, __u16 value,
		       const void *data, __u16 size)
{
	return usb_control_msg_send(priv->udev, 0, request,
				    USB_DIR_OUT | USB_TYPE_VENDOR | USB_RECIP_DEVICE,
				    value, 0, data, size, 1000, GFP_KERNEL);
}

/*
 * Simplest bittiming path for now: forward the requested bitrate and let the
 * firmware compute the segments. The kernel still validates against
 * lw_can_bittiming_const above.
 *
 * TODO (TODO #6 in firmware notes): send the kernel-computed brp/seg/sjw instead
 * once the firmware protocol accepts register-level bit timing.
 */
static int lw_can_set_bittiming(struct net_device *netdev)
{
	struct lw_can *priv = netdev_priv(netdev);
	struct can_bittiming *bt = &priv->can.bittiming;
	struct lw_can_bittiming payload = {
		.nominal_baud = cpu_to_le32(bt->bitrate),
		.data_baud    = cpu_to_le32(bt->bitrate),
	};

	return lw_can_ctrl(priv, LW_CAN_REQ_SET_BITTIMING, 0,
			   &payload, sizeof(payload));
}

static int lw_can_set_mode(struct net_device *netdev, enum can_mode mode)
{
	switch (mode) {
	case CAN_MODE_START:
		/* TODO (milestone 4): clear bus-off state on the device. */
		netif_wake_queue(netdev);
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

/* ---- netdev ops ---------------------------------------------------------- */

static int lw_can_open(struct net_device *netdev)
{
	struct lw_can *priv = netdev_priv(netdev);
	int err;

	err = open_candev(netdev);
	if (err)
		return err;

	err = lw_can_ctrl(priv, LW_CAN_REQ_OPEN, 0, NULL, 0);
	if (err) {
		netdev_err(netdev, "OPEN request failed: %d\n", err);
		close_candev(netdev);
		return err;
	}

	/* TODO (milestone 2): allocate and submit RX URBs on priv->ep_in here. */

	netif_start_queue(netdev);
	return 0;
}

static int lw_can_close(struct net_device *netdev)
{
	struct lw_can *priv = netdev_priv(netdev);

	netif_stop_queue(netdev);

	/* TODO (milestone 2): kill in-flight RX/TX URBs here. */

	lw_can_ctrl(priv, LW_CAN_REQ_CLOSE, 0, NULL, 0);
	close_candev(netdev);
	return 0;
}

static netdev_tx_t lw_can_start_xmit(struct sk_buff *skb,
				     struct net_device *netdev)
{
	if (can_dev_dropped_skb(netdev, skb))
		return NETDEV_TX_OK;

	/*
	 * TODO (milestone 3): build a struct lw_can_packet (sof + crc via
	 * crc32_le(~0, p, offsetof(crc)) ^ ~0), can_put_echo_skb(), and submit a
	 * bulk OUT URB; complete TX + can_get_echo_skb() in the URB callback.
	 */
	netdev_warn_once(netdev, "TX not implemented yet; dropping frame\n");
	netdev->stats.tx_dropped++;
	dev_kfree_skb(skb);
	return NETDEV_TX_OK;
}

static const struct net_device_ops lw_can_netdev_ops = {
	.ndo_open       = lw_can_open,
	.ndo_stop       = lw_can_close,
	.ndo_start_xmit = lw_can_start_xmit,
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

	priv->can.clock.freq        = LW_CAN_CLOCK_HZ;
	priv->can.bittiming_const   = &lw_can_bittiming_const;
	priv->can.do_set_bittiming  = lw_can_set_bittiming;
	priv->can.do_set_mode       = lw_can_set_mode;
	/* TODO: ctrlmode_supported |= LISTENONLY | LOOPBACK | ONE_SHOT | FD ... */

	netdev->netdev_ops = &lw_can_netdev_ops;
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
