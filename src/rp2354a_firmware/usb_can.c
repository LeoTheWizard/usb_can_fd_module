/**
 * @file usb_can.c
 * @brief USB-CAN bridge implementation.
 * Runs on core 1. Drives the TinyUSB task loop and shuttles frames between
 * the USB bulk endpoints and the shared CAN TX/RX queues.
 *
 * @author Leo Walker
 * @copyright (c) Leo Walker 2025 @ MIT License @cite MIT License
 */

#include "usb_can.h"

#include <string.h>
#include <tusb.h>

void usb_can_init(void)
{
    tusb_init();
}

void usb_can_task(can_queue_t *rx_queue, can_queue_t *tx_queue)
{
    tud_task();

    if (!tud_vendor_mounted())
        return;

    // Device → Host: drain the CAN RX queue into the USB bulk IN FIFO.
    can_message_t msg;
    while (tud_vendor_write_available() >= sizeof(usb_can_packet_t) &&
           can_queue_pop(rx_queue, &msg) == 0)
    {
        usb_can_packet_t pkt = {
            .timestamp_us = (uint32_t)msg.timestamp,
            .type         = (uint8_t)msg.type,
            ._reserved    = {0, 0, 0},
        };

        if (msg.type == CAN_MSG_FRAME)
            pkt.payload.frame = msg.frame;
        else if (msg.type == CAN_MSG_ERROR)
            pkt.payload.error = msg.error;

        tud_vendor_write(&pkt, sizeof(pkt));
    }
    tud_vendor_write_flush();

    // Host → Device: read complete USB packets and push onto the CAN TX queue.
    while (tud_vendor_available() >= sizeof(usb_can_packet_t))
    {
        usb_can_packet_t pkt;
        if (tud_vendor_read(&pkt, sizeof(pkt)) != sizeof(pkt))
            break;

        can_message_t out = {
            .timestamp = pkt.timestamp_us,
            .type      = CAN_MSG_FRAME,
            .frame     = pkt.payload.frame,
        };
        can_queue_push(tx_queue, &out);
    }
}

// ---- TinyUSB control request callback --------------------------------------

// Staging buffer for SET_BITTIMING data phase.
static usb_can_bittiming_t pending_bittiming;

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                tusb_control_request_t const *request)
{
    if (request->bmRequestType_bit.type != TUSB_REQ_TYPE_VENDOR)
        return false;

    switch (request->bRequest)
    {
    case USB_CAN_REQ_OPEN:
        if (stage == CONTROL_STAGE_SETUP)
        {
            // TODO: signal core 0 via inter-core FIFO to call mcp251xfd_change_opmode(NORMAL)
            return tud_control_status(rhport, request);
        }
        break;

    case USB_CAN_REQ_CLOSE:
        if (stage == CONTROL_STAGE_SETUP)
        {
            // TODO: signal core 0 via inter-core FIFO to call mcp251xfd_change_opmode(SLEEP)
            return tud_control_status(rhport, request);
        }
        break;

    case USB_CAN_REQ_SET_BITTIMING:
        if (stage == CONTROL_STAGE_SETUP)
            return tud_control_xfer(rhport, request, &pending_bittiming,
                                    sizeof(pending_bittiming));
        if (stage == CONTROL_STAGE_ACK)
        {
            // TODO: signal core 0 via inter-core FIFO to call
            //       mcp251xfd_set_baudrates(nominal_baud, data_baud)
            (void)pending_bittiming;
        }
        break;

    case USB_CAN_REQ_SET_MODE:
        if (stage == CONTROL_STAGE_SETUP)
        {
            // wValue carries the mcp251xfd_opmode_t to switch to
            // TODO: signal core 0 via inter-core FIFO to call mcp251xfd_change_opmode(wValue)
            return tud_control_status(rhport, request);
        }
        break;

    default:
        return false;
    }

    return true;
}
