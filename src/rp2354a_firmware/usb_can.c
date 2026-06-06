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
#include "ipc.h"

#include <string.h>
#include <tusb.h>
#include <pico/multicore.h>

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
            .sof          = USB_CAN_SOF,
            .type         = (uint8_t)msg.type,
            ._reserved    = 0,
            .timestamp_us = (uint32_t)msg.timestamp,
        };

        if (msg.type == CAN_MSG_FRAME)
            pkt.payload.frame = msg.frame;
        else if (msg.type == CAN_MSG_ERROR)
            pkt.payload.error = msg.error;

        usb_can_packet_set_crc(&pkt);
        tud_vendor_write(&pkt, sizeof(pkt));
    }
    tud_vendor_write_flush();

    // Host → Device: read complete USB packets and push onto the CAN TX queue.
    while (tud_vendor_available() >= sizeof(usb_can_packet_t))
    {
        // Re-align to the start-of-frame marker: drop any leading byte that is not
        // the low byte of USB_CAN_SOF so a desynced stream heals itself.
        uint8_t head;
        if (!tud_vendor_peek(&head))
            break;
        if (head != (uint8_t)(USB_CAN_SOF & 0xFFu))
        {
            uint8_t discard;
            tud_vendor_read(&discard, 1);
            continue;
        }

        usb_can_packet_t pkt;
        if (tud_vendor_read(&pkt, sizeof(pkt)) != sizeof(pkt))
            break;

        // Reject a false marker match or corruption; the next iteration re-aligns.
        if (pkt.sof != USB_CAN_SOF || !usb_can_packet_check_crc(&pkt))
            continue;

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
            // Drop any stale host→device bytes buffered before this session so the
            // first post-open packet starts on a clean frame boundary.
            tud_vendor_read_flush();
            multicore_fifo_push_blocking(IPC_CMD_OPEN);
            return tud_control_status(rhport, request);
        }
        break;

    case USB_CAN_REQ_CLOSE:
        if (stage == CONTROL_STAGE_SETUP)
        {
            multicore_fifo_push_blocking(IPC_CMD_CLOSE);
            return tud_control_status(rhport, request);
        }
        break;

    case USB_CAN_REQ_SET_BITTIMING:
        if (stage == CONTROL_STAGE_SETUP)
            return tud_control_xfer(rhport, request, &pending_bittiming,
                                    sizeof(pending_bittiming));
        if (stage == CONTROL_STAGE_ACK)
        {
            multicore_fifo_push_blocking(IPC_CMD_SET_BITTIMING);
            multicore_fifo_push_blocking(pending_bittiming.nominal_baud);
            multicore_fifo_push_blocking(pending_bittiming.data_baud);
        }
        break;

    case USB_CAN_REQ_SET_MODE:
        if (stage == CONTROL_STAGE_SETUP)
        {
            multicore_fifo_push_blocking(IPC_CMD_SET_MODE);
            multicore_fifo_push_blocking((uint32_t)request->wValue);
            return tud_control_status(rhport, request);
        }
        break;

    case USB_CAN_REQ_SET_TERMINATION:
        if (stage == CONTROL_STAGE_SETUP)
        {
            multicore_fifo_push_blocking(IPC_CMD_SET_TERMINATION);
            multicore_fifo_push_blocking((uint32_t)request->wValue);
            return tud_control_status(rhport, request);
        }
        break;

    default:
        return false;
    }

    return true;
}

// ---- TinyUSB suspend/resume callbacks --------------------------------------

void tud_suspend_cb(bool remote_wakeup_en)
{
    (void)remote_wakeup_en;
    multicore_fifo_push_blocking(IPC_CMD_CLOSE);
}

void tud_resume_cb(void)
{
    // Host will send USB_CAN_REQ_OPEN when it is ready to use the bus.
}
