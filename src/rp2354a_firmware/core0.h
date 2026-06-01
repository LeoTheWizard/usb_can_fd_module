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

typedef struct can_queue
{
    void *frames;
    size_t frame_size;
    size_t head;
    size_t tail;
    size_t capacity;
} can_queue_t;

void core0_main(void);

#endif /* _CORE0_H_ */