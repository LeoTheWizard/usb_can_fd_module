/**
 * @file can_queue.h
 * @brief A simple atomic circular buffer implementation for CAN frames.
 * Single producer (core 1) and single consumer (core 0) design, no locks needed.
 *
 * @author Leo Walker
 * @copyright (c) Leo Walker 2025 @ MIT License @cite MIT License
 */

#ifndef _CAN_QUEUE_H_
#define _CAN_QUEUE_H_

#include <stddef.h>
#include <stdatomic.h>

#include <lw_mcp251xfd/can.h>

/**
 * @struct can_message
 * @brief CAN message structure with timestamp for ordering and latency measurement.
 */
typedef struct can_message
{
    size_t timestamp; // Timestamp in microseconds, for ordering and latency measurement.
    can_frame_t frame;
} can_message_t;

/**
 * @struct can_queue
 * @brief Lock-free SPSC circular buffer for CAN messages.
 * head and tail are atomic so no mutex is required on the hot path.
 */
typedef struct can_queue
{
    can_message_t *messages;
    atomic_size_t head;
    atomic_size_t tail;
    size_t capacity;
} can_queue_t;

/**
 * @brief Static initializer for a can_queue_t.
 * Use at declaration time to avoid needing a separate can_queue_init() call.
 *
 * @param buf  Pointer to a can_message_t array to use as the backing store.
 * @param cap  Number of elements in the array.
 */
#define CAN_QUEUE_STATIC_INIT(buf, cap) \
    {.messages = (buf), .head = 0, .tail = 0, .capacity = (cap)}

/**
 * @brief Initialises a can_queue_t at runtime with the given backing buffer.
 * Use CAN_QUEUE_STATIC_INIT instead when the queue can be declared statically.
 *
 * @param queue    Pointer to the can_queue_t to initialise.
 * @param buffer   Pointer to a can_message_t array to use as the backing store.
 * @param capacity Number of elements in the array.
 */
void can_queue_init(can_queue_t *queue, can_message_t *buffer, size_t capacity);

/**
 * @brief Pushes a message onto the tail of the queue.
 * Called by the producer (core 1) only.
 *
 * @param queue   Pointer to the can_queue_t.
 * @param message Pointer to the message to copy into the queue.
 * @return 0 on success, -1 if the queue is full.
 */
int can_queue_push(can_queue_t *queue, const can_message_t *message);

/**
 * @brief Pops a message from the head of the queue.
 * Called by the consumer (core 0) only.
 *
 * @param queue   Pointer to the can_queue_t.
 * @param message Pointer to a can_message_t to copy the result into.
 * @return 0 on success, -1 if the queue is empty.
 */
int can_queue_pop(can_queue_t *queue, can_message_t *message);

/**
 * @brief Returns the number of messages currently in the queue.
 * This is a snapshot — the value may change immediately after return in a concurrent context.
 *
 * @param queue Pointer to the can_queue_t to check.
 * @return size_t Number of messages currently in the queue.
 */
size_t can_queue_size(const can_queue_t *queue);

/**
 * @brief Resets the queue to empty.
 * Only safe to call when neither producer nor consumer is actively using the queue
 * (e.g. during initialisation or after bus-off recovery before restarting cores).
 *
 * @param queue Pointer to the can_queue_t to clear.
 */
void can_queue_clear(can_queue_t *queue);

#endif /* _CAN_QUEUE_H_ */