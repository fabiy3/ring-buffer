/*
 * ring_buffer.c — the core implementation of the ring buffer.
 *
 * RESPONSIBILITY: This file does ONE thing — implement the ring buffer
 * data structure. It contains no main(), no threads, no printing.
 * It is a pure library: just functions that operate on ring_buf_t.
 *
 * WHAT IS A RING BUFFER:
 *   A fixed-size circular array. Instead of shifting elements when you
 *   read, you advance a "head" pointer. Instead of appending to the end,
 *   you advance a "tail" pointer and wrap around when you hit the end.
 *   The result: O(1) reads and writes, always, with zero memory allocation.
 *
 *   Imagine 8 slots in a circle:
 *
 *       [0][1][2][3][4][5][6][7]
 *              ^H          ^T
 *
 *   Head (H) = next slot to read.
 *   Tail (T) = next slot to write.
 *   Writing moves T forward. Reading moves H forward.
 *   When either reaches slot 7, it wraps back to 0.
 */

#include <string.h>        /* memcpy, memset                            */
#include "ring_buffer.h"   /* our own types — quotes, not <>            */

/* ── rb_init ─────────────────────────────────────────────────────────────
 *
 * Zero out the whole struct and initialize the mutex.
 *
 * memset(ptr, 0, size) fills a block of memory with zero bytes.
 * Setting head, tail, and count to 0 means "empty buffer, start here."
 *
 * pthread_mutex_init() initializes the mutex that protects this buffer.
 * The second argument (NULL) means "use default mutex settings."
 * A mutex ensures only one thread can modify head/tail/count at a time.
 */
rb_status_t rb_init(ring_buf_t *rb)
{
    if (!rb)    /* NULL check — always do this before dereferencing */
        return RB_NULL;

    memset(rb, 0, sizeof(ring_buf_t));
    pthread_mutex_init(&rb->lock, NULL);
    return RB_OK;
}

/* ── rb_write ────────────────────────────────────────────────────────────
 *
 * Copy one packet into the next available slot, then advance tail.
 *
 * THREAD SAFETY:
 *   We lock the mutex at the start and unlock at every exit point.
 *   This guarantees that even if two threads call rb_write() at exactly
 *   the same time, they won't corrupt each other's state.
 *
 *   The pattern is always:
 *     lock → do work → unlock
 *   Never return without unlocking — that would deadlock the program.
 *
 * THE BITMASK TRICK:
 *   tail = (tail + 1) & RING_MASK
 *
 *   RING_CAPACITY = 64 = 0b01000000
 *   RING_MASK     = 63 = 0b00111111
 *
 *   ANDing with RING_MASK zeroes out any bit above position 5,
 *   so the result is always 0–63. This is equivalent to % 64
 *   but compiles to a single CPU instruction instead of a division.
 */
rb_status_t rb_write(ring_buf_t *rb, const packet_t *pkt)
{
    if (!rb || !pkt)
        return RB_NULL;

    pthread_mutex_lock(&rb->lock);

    if (rb->count >= RING_CAPACITY) {
        /*
         * Buffer is full. We count the drop but do NOT overwrite
         * old packets. Overwriting would corrupt in-order delivery,
         * which matters for sequence-counted telemetry streams.
         * Some ring buffers DO overwrite — this is a design choice.
         */
        rb->total_dropped++;
        pthread_mutex_unlock(&rb->lock);
        return RB_FULL;
    }

    /*
     * memcpy(destination, source, num_bytes)
     * Copies the caller's packet into our slot at tail.
     * We copy by value so the caller can safely reuse their buffer
     * immediately after this call returns.
     */
    memcpy(&rb->slots[rb->tail], pkt, sizeof(packet_t));

    rb->tail = (rb->tail + 1) & RING_MASK;   /* advance tail, wrap at 64 */
    rb->count++;
    rb->total_written++;

    pthread_mutex_unlock(&rb->lock);
    return RB_OK;
}

/* ── rb_read ─────────────────────────────────────────────────────────────
 *
 * Copy the oldest packet out of the buffer, then advance head.
 *
 * "Oldest" because we read from head — the slot that was written
 * earliest. This gives us FIFO (First In, First Out) ordering,
 * which preserves packet sequence order. Critical for telemetry.
 *
 * We clear the slot after reading (memset to 0). This is optional
 * but makes debugging easier — you can inspect memory and see which
 * slots have been consumed.
 */
rb_status_t rb_read(ring_buf_t *rb, packet_t *pkt_out)
{
    if (!rb || !pkt_out)
        return RB_NULL;

    pthread_mutex_lock(&rb->lock);

    if (rb->count == 0) {
        pthread_mutex_unlock(&rb->lock);
        return RB_EMPTY;
    }

    /* Copy the packet at head out to the caller */
    memcpy(pkt_out, &rb->slots[rb->head], sizeof(packet_t));

    /* Clear the slot we just vacated — aids debugging */
    memset(&rb->slots[rb->head], 0, sizeof(packet_t));

    rb->head = (rb->head + 1) & RING_MASK;   /* advance head, wrap at 64 */
    rb->count--;
    rb->total_read++;

    pthread_mutex_unlock(&rb->lock);
    return RB_OK;
}

/* ── rb_stats ────────────────────────────────────────────────────────────
 *
 * Take a consistent snapshot of the buffer's state.
 *
 * We lock before reading any fields to prevent a race condition:
 * without the lock, a writer thread could increment count between
 * our read of count and our read of total_written, producing
 * a snapshot where the numbers don't add up.
 *
 * The (float) cast is necessary because integer division in C
 * truncates: 3 / 64 = 0, not 0.046. Casting one operand to float
 * forces the whole expression to float arithmetic.
 */
rb_status_t rb_stats(ring_buf_t *rb, rb_stats_t *out)
{
    if (!rb || !out)
        return RB_NULL;

    pthread_mutex_lock(&rb->lock);

    out->capacity         = RING_CAPACITY;
    out->used             = rb->count;
    out->free             = RING_CAPACITY - rb->count;
    out->utilization_pct  = (float)rb->count / RING_CAPACITY * 100.0f;
    out->total_written    = rb->total_written;
    out->total_read       = rb->total_read;
    out->total_dropped    = rb->total_dropped;

    uint64_t attempts = rb->total_written + rb->total_dropped;
    out->drop_rate_pct = attempts > 0
                         ? (float)rb->total_dropped / attempts * 100.0f
                         : 0.0f;

    pthread_mutex_unlock(&rb->lock);
    return RB_OK;
}

/* ── rb_is_empty / rb_is_full ────────────────────────────────────────────
 *
 * Convenience helpers that return 1 (true) or 0 (false).
 * Lock briefly just to read count safely.
 */
int rb_is_empty(ring_buf_t *rb)
{
    if (!rb) return 1;
    pthread_mutex_lock(&rb->lock);
    int empty = (rb->count == 0);
    pthread_mutex_unlock(&rb->lock);
    return empty;
}

int rb_is_full(ring_buf_t *rb)
{
    if (!rb) return 0;
    pthread_mutex_lock(&rb->lock);
    int full = (rb->count >= RING_CAPACITY);
    pthread_mutex_unlock(&rb->lock);
    return full;
}

/* ── rb_destroy ──────────────────────────────────────────────────────────
 *
 * Release the mutex when the program is done with this buffer.
 * After calling this, the buffer must not be used again.
 *
 * In C there is no garbage collector — resources you initialize,
 * you must clean up. pthread_mutex_destroy() releases the OS-level
 * resources the mutex holds.
 */
void rb_destroy(ring_buf_t *rb)
{
    if (!rb) return;
    pthread_mutex_destroy(&rb->lock);
}
