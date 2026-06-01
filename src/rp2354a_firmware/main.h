/**
 * @file main.h
 * @brief Global definitions for the CAN module firmware.
 */

#ifndef _MAIN_H_
#define _MAIN_H_

#include "can_queue.h"

#define FIRMWARE_VERSION "0.1.0"

extern can_queue_t can_tx_queue;
extern can_queue_t can_rx_queue;

#endif /* _MAIN_H_ */