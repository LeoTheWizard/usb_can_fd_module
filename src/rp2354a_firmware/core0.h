/**
 * @file core0.h
 * @brief Entry point for core 0 program.
 * Handles the CAN controller and the transmission of CAN frames.
 * Updates Indicator LEDs based on the bus state.
 *
 * @author Leo Walker
 * @copyright (c) Leo Walker 2025 @ MIT License @cite MIT License
 */

#ifndef _CORE0_H_
#define _CORE0_H_

#include "can_queue.h"

extern can_queue_t can_tx_queue;
extern can_queue_t can_rx_queue;

void core0_main(void);

#endif /* _CORE0_H_ */