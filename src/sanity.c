/*
 * sanity.c — a simple walkthrough of the ring buffer's behavior.
 *
 * RESPONSIBILITY: Show how the ring buffer works in a readable,
 * step-by-step way. This is NOT a test — it doesn't verify correctness.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "ring_buffer.h"

/* ── Helper: print current buffer stats ─────────────────────────────── */
static void print_stats(ring_buf_t *rb, const char *label)
{
    rb_stats_t s;
    rb_stats(rb, &s);
    printf("  [%s] used=%u/%u  written=%llu  read=%llu  dropped=%llu\n",
           label,
           s.used, s.capacity,
           (unsigned long long)s.total_written,
           (unsigned long long)s.total_read,
           (unsigned long long)s.total_dropped);
}

/* ── Helper: build a test packet with a given APID and sequence number  */
static packet_t make_packet(uint16_t apid, uint16_t seq)
{
    packet_t pkt;
    memset(&pkt, 0, sizeof(packet_t));
    pkt.apid        = apid;
    pkt.seq_count   = seq;
    pkt.payload_len = 4;
    /* Store APID and seq in payload so we can verify them on read */
    pkt.payload[0]  = (uint8_t)(apid >> 8);
    pkt.payload[1]  = (uint8_t)(apid & 0xFF);
    pkt.payload[2]  = (uint8_t)(seq >> 8);
    pkt.payload[3]  = (uint8_t)(seq & 0xFF);
    return pkt;
}

int main(void)
{
    ring_buf_t rb;
    packet_t   pkt;
    rb_status_t status;

    printf("===========================================\n");
    printf("  Ring Buffer Sanity Check\n");
    printf("  Capacity: %d slots\n", RING_CAPACITY);
    printf("===========================================\n\n");

    /* ── Step 1: Initialize ─────────────────────────────────────────── */
    printf("── Step 1: Initialize the buffer\n");
    rb_init(&rb);
    printf("  Buffer created. Empty: %s\n\n", rb_is_empty(&rb) ? "yes" : "no");

    /* ── Step 2: Write some packets ─────────────────────────────────── */
    printf("── Step 2: Write 5 packets from 3 different subsystems\n");

    packet_t p1 = make_packet(0x001, 0);   /* Attitude control, seq 0 */
    packet_t p2 = make_packet(0x002, 0);   /* Power, seq 0            */
    packet_t p3 = make_packet(0x001, 1);   /* Attitude control, seq 1 */
    packet_t p4 = make_packet(0x003, 0);   /* Thermal, seq 0          */
    packet_t p5 = make_packet(0x002, 1);   /* Power, seq 1            */

    rb_write(&rb, &p1);  printf("  wrote APID=0x001 seq=0\n");
    rb_write(&rb, &p2);  printf("  wrote APID=0x002 seq=0\n");
    rb_write(&rb, &p3);  printf("  wrote APID=0x001 seq=1\n");
    rb_write(&rb, &p4);  printf("  wrote APID=0x003 seq=0\n");
    rb_write(&rb, &p5);  printf("  wrote APID=0x002 seq=1\n");

    print_stats(&rb, "after writes");
    printf("\n");

    /* ── Step 3: Read packets back in FIFO order ─────────────────────── */
    printf("── Step 3: Read packets back (should come out in same order)\n");

    while (rb_read(&rb, &pkt) == RB_OK) {
        printf("  read  APID=0x%03X seq=%u\n", pkt.apid, pkt.seq_count);
    }

    print_stats(&rb, "after reads");
    printf("\n");

    /* ── Step 4: Demonstrate overflow behavior ───────────────────────── */
    printf("── Step 4: Fill the buffer to capacity (%d slots), then try to overflow\n",
           RING_CAPACITY);

    for (int i = 0; i < RING_CAPACITY; i++) {
        packet_t p = make_packet(0x001, (uint16_t)i);
        rb_write(&rb, &p);
    }
    printf("  Buffer is full: %s\n", rb_is_full(&rb) ? "yes" : "no");

    /* Try to write one more — should be dropped */
    packet_t overflow_pkt = make_packet(0x001, 9999);
    status = rb_write(&rb, &overflow_pkt);
    if (status == RB_FULL) {
        printf("  Overflow correctly detected — extra packet dropped\n");
    }

    print_stats(&rb, "overflow test");
    printf("\n");

    /* ── Step 5: Read a few, write a few — demonstrate wrap-around ───── */
    printf("── Step 5: Demonstrate wrap-around (the 'ring' in ring buffer)\n");

    /* Read 10 packets to make some space */
    printf("  Reading 10 packets to make room...\n");
    for (int i = 0; i < 10; i++) {
        rb_read(&rb, &pkt);
    }

    /* Write 10 new ones — these will wrap around to the start of the array */
    printf("  Writing 10 new packets — tail pointer will wrap around...\n");
    for (int i = 0; i < 10; i++) {
        packet_t p = make_packet(0x004, (uint16_t)i);
        rb_write(&rb, &p);
    }

    print_stats(&rb, "after wrap");
    printf("  (head and tail are now at different positions in the array,\n");
    printf("   proving the circular nature works correctly)\n\n");

    /* ── Step 6: Read remaining packets until empty ───────────────────── */
    printf("── Step 6: Drain the buffer completely\n");
    uint64_t drained = 0;
    while (rb_read(&rb, &pkt) == RB_OK) {
        drained++;
    }
    printf("  Drained %llu packets\n", (unsigned long long)drained);

    /* Try to read from empty buffer */
    status = rb_read(&rb, &pkt);
    if (status == RB_EMPTY) {
        printf("  Underflow correctly detected — nothing left to read\n");
    }

    print_stats(&rb, "final");
    printf("\n");

    /* ── Cleanup ─────────────────────────────────────────────────────── */
    rb_destroy(&rb);
    printf("Buffer destroyed. Sanity check complete.\n");
    printf("===========================================\n");

    return 0;
}
