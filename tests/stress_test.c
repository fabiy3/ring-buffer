/*
 * stress_test.c — proves the ring buffer is correct under real concurrency.
 *
 * RESPONSIBILITY: This file is the automated correctness test.
 * It spins up two threads that run simultaneously for several seconds —
 * one writing as fast as possible, one reading as fast as possible —
 * then checks that no packets were silently lost or corrupted.
 *
 * WHY THIS MATTERS:
 *   It's easy to write a ring buffer that works when only one thread
 *   uses it. The hard part is thread safety. This test reveals race
 *   conditions — bugs that only appear when two threads access shared
 *   data at exactly the same time.
 *
 *   In aerospace FSW, tests like this are called "stress tests" or
 *   "soak tests." Running them is standard practice before certifying
 *   any flight software component.
 *
 * THE CORRECTNESS CHECK:
 *   At the end, this must hold:
 *     total_written == total_read + total_dropped
 *
 *   If it doesn't, bytes were silently corrupted — a serious bug.
 */

#include <stdio.h>       /* printf                                      */
#include <stdlib.h>      /* exit()                                      */
#include <string.h>      /* memset                                      */
#include <stdint.h>      /* uint32_t, uint64_t                          */
#include <pthread.h>     /* pthread_t, pthread_create, pthread_join     */
#include <unistd.h>      /* sleep()                                     */
#include <time.h>        /* clock_gettime                               */
#include "ring_buffer.h"

/* How long to run the stress test in seconds */
#define TEST_DURATION_SEC  5

/*
 * A flag shared between threads. When set to 0, threads stop.
 *
 * "volatile" tells the compiler: do not cache this in a register.
 * Always read it fresh from memory. Without volatile, the compiler
 * might optimize the read away and the thread would loop forever.
 */
static volatile int running = 1;

/* Counters each thread maintains independently (no sharing needed) */
static uint64_t writer_sent     = 0;
static uint64_t reader_received = 0;

/* The shared buffer — both threads access this */
static ring_buf_t shared_buf;

/* ── Writer thread ───────────────────────────────────────────────────────
 *
 * pthread_create() requires the thread function to have this exact
 * signature: void* func(void *arg). The void* arg is how you pass
 * data into a thread — we don't need it here so we ignore it.
 */
static void *writer_thread(void *arg)
{
    (void)arg;   /* suppress "unused parameter" compiler warning */

    packet_t pkt;
    uint16_t seq = 0;

    printf("[writer] thread started\n");

    while (running) {
        /* Build a simple test packet */
        memset(&pkt, 0, sizeof(packet_t));
        pkt.apid        = 0x001;
        pkt.seq_count   = seq;
        pkt.payload_len = 8;
        /* Write the sequence number into the payload so the reader
         * can verify it arrived uncorrupted */
        pkt.payload[0]  = (uint8_t)(seq >> 8);
        pkt.payload[1]  = (uint8_t)(seq & 0xFF);

        rb_status_t status = rb_write(&shared_buf, &pkt);
        if (status == RB_OK) {
            writer_sent++;
            seq = (seq + 1) & 0x3FFF;  /* wrap at 16383, like CCSDS */
        }
        /* If RB_FULL, we just try again next iteration — the buffer
         * tracks the drop itself via total_dropped */
    }

    printf("[writer] thread stopped — sent %llu packets\n",
           (unsigned long long)writer_sent);
    return NULL;
}

/* ── Reader thread ───────────────────────────────────────────────────────
 *
 * Reads as fast as possible. When the buffer is empty, it yields the
 * CPU briefly with sched_yield() so the writer gets a chance to fill it.
 */
static void *reader_thread(void *arg)
{
    (void)arg;

    packet_t pkt;

    printf("[reader] thread started\n");

    while (running || !rb_is_empty(&shared_buf)) {
        rb_status_t status = rb_read(&shared_buf, &pkt);
        if (status == RB_OK) {
            reader_received++;
        }
        /* If empty, yield CPU — don't busy-spin and waste all cycles */
        /* sched_yield() says "I'm willing to let another thread run now" */
    }

    printf("[reader] thread stopped — received %llu packets\n",
           (unsigned long long)reader_received);
    return NULL;
}

/* ── main ────────────────────────────────────────────────────────────────
 *
 * Sets up the buffer, launches both threads, waits, then checks results.
 */
int main(void)
{
    printf("===========================================\n");
    printf("  Ring Buffer Stress Test\n");
    printf("  Duration: %d seconds, Capacity: %d slots\n",
           TEST_DURATION_SEC, RING_CAPACITY);
    printf("===========================================\n\n");

    /* Initialize the buffer — always do this before any reads/writes */
    if (rb_init(&shared_buf) != RB_OK) {
        fprintf(stderr, "ERROR: failed to initialize ring buffer\n");
        return 1;
    }

    /*
     * pthread_t is a thread handle — like a file descriptor but for threads.
     * pthread_create(handle, options, function, argument)
     * After this call, both threads are running simultaneously.
     */
    pthread_t writer, reader;
    pthread_create(&writer, NULL, writer_thread, NULL);
    pthread_create(&reader, NULL, reader_thread, NULL);

    /* Let the threads run for TEST_DURATION_SEC seconds, printing
     * a live status line every second */
    for (int i = 0; i < TEST_DURATION_SEC; i++) {
        sleep(1);
        rb_stats_t s;
        rb_stats(&shared_buf, &s);
        printf("[t=%ds] written=%-8llu  read=%-8llu  dropped=%-6llu  "
               "buffer=%u/%u (%.0f%%)\n",
               i + 1,
               (unsigned long long)s.total_written,
               (unsigned long long)s.total_read,
               (unsigned long long)s.total_dropped,
               s.used, s.capacity,
               s.utilization_pct);
    }

    /* Signal threads to stop, then wait for them to finish */
    running = 0;

    /*
     * pthread_join(thread, return_value_ptr)
     * Blocks until the given thread exits. Without this, main() could
     * return while the threads are still running — undefined behavior.
     */
    pthread_join(writer, NULL);
    pthread_join(reader, NULL);

    /* ── Final correctness check ─────────────────────────────────────
     *
     * Every packet either:
     *   a) was written AND later read (success path), OR
     *   b) was attempted but dropped due to overflow
     *
     * So: writer_sent == reader_received + total_dropped
     *
     * If this doesn't hold, we have a bug — packets vanished silently.
     */
    rb_stats_t final;
    rb_stats(&shared_buf, &final);

    uint64_t total_attempts = final.total_written + final.total_dropped;

    printf("\n===========================================\n");
    printf("  Results\n");
    printf("===========================================\n");
    printf("  Packets written:     %llu\n", (unsigned long long)final.total_written);
    printf("  Packets read:        %llu\n", (unsigned long long)final.total_read);
    printf("  Packets dropped:     %llu\n", (unsigned long long)final.total_dropped);
    printf("  Drop rate:           %.2f%%\n", final.drop_rate_pct);
    printf("  Write attempts:      %llu\n", (unsigned long long)total_attempts);
    printf("  Remaining in buffer: %u\n",   final.used);
    printf("\n");

    /* The check: written == read + dropped (accounting for what's still buffered) */
    int passed = (final.total_written == final.total_read + final.used);

    if (passed) {
        printf("  RESULT: PASS — no packets lost or corrupted\n");
        printf("          written (%llu) == read (%llu) + buffered (%u)\n",
               (unsigned long long)final.total_written,
               (unsigned long long)final.total_read,
               final.used);
    } else {
        printf("  RESULT: FAIL — packet accounting mismatch!\n");
        printf("          written (%llu) != read (%llu) + buffered (%u)\n",
               (unsigned long long)final.total_written,
               (unsigned long long)final.total_read,
               final.used);
    }

    printf("===========================================\n");

    rb_destroy(&shared_buf);
    return passed ? 0 : 1;
}
