/**
 * @file can_queue.c
 * @brief Implementation of a simple atomic circular buffer for CAN messages.
 *
 * @author Leo Walker
 * @copyright (c) Leo Walker 2025 @ MIT License @cite MIT License
 */

#include "can_queue.h"

void can_queue_init(can_queue_t *queue, can_message_t *buffer, size_t capacity)
{
    queue->messages = buffer;
    queue->capacity = capacity;
    atomic_store_explicit(&queue->head, 0, memory_order_relaxed);
    atomic_store_explicit(&queue->tail, 0, memory_order_relaxed);
}

int can_queue_push(can_queue_t *queue, const can_message_t *message)
{
    size_t tail = atomic_load_explicit(&queue->tail, memory_order_relaxed);
    size_t next_tail = (tail + 1) % queue->capacity;

    if (next_tail == atomic_load_explicit(&queue->head, memory_order_acquire))
        return -1;

    queue->messages[tail] = *message;

    atomic_store_explicit(&queue->tail, next_tail, memory_order_release);
    return 0;
}

int can_queue_pop(can_queue_t *queue, can_message_t *message)
{
    size_t head = atomic_load_explicit(&queue->head, memory_order_relaxed);

    if (head == atomic_load_explicit(&queue->tail, memory_order_acquire))
        return -1;

    *message = queue->messages[head];

    atomic_store_explicit(&queue->head, (head + 1) % queue->capacity, memory_order_release);
    return 0;
}

size_t can_queue_size(const can_queue_t *queue)
{
    size_t head = atomic_load_explicit(&queue->head, memory_order_acquire);
    size_t tail = atomic_load_explicit(&queue->tail, memory_order_acquire);
    return (tail + queue->capacity - head) % queue->capacity;
}

void can_queue_clear(can_queue_t *queue)
{
    atomic_store_explicit(&queue->head, 0, memory_order_seq_cst);
    atomic_store_explicit(&queue->tail, 0, memory_order_seq_cst);
}
