/**
 * @file usb_can.h
 * @brief USB-CAN bridge — firmware-side API.
 *
 * Wire protocol definitions live in common/usb_can_protocol.h and are shared
 * with the host drivers. This header exposes only the firmware entry points.
 *
 * @author Leo Walker
 * @copyright (c) Leo Walker 2025 @ MIT License @cite MIT License
 */

#ifndef _USB_CAN_H_
#define _USB_CAN_H_

#include <usb_can_protocol.h>
#include "can_queue.h"

/**
 * @brief Initialises the TinyUSB device stack.
 * Must be called once before usb_can_task().
 */
void usb_can_init(void);

/**
 * @brief Services the USB device stack and bridges frames between USB and the CAN queues.
 * Must be called repeatedly from the core 1 main loop — never block inside this function.
 *
 * @param rx_queue CAN frames received from the bus, ready to forward to the host.
 * @param tx_queue CAN frames received from the host, ready to transmit on the bus.
 */
void usb_can_task(can_queue_t *rx_queue, can_queue_t *tx_queue);

#endif // _USB_CAN_H_
