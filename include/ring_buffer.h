#ifndef RING_BUFFER_H
#define RING_BUFFER_H

/*
 * ring_buffer.h — the "dictionary" for this entire project.
 */

#include <stdint.h>      /* uint8_t, uint32_t — fixed-size integers     */
#include <stddef.h>      /* size_t — type for sizes and lengths          */
#include <pthread.h>     /* pthread_mutex_t — thread safety              */

/* ── Constants ──────────────────────────────────────────────────────────
 *
 * #define creates a named constant. The compiler replaces every use of
 * RING_CAPACITY with 64 before compiling. No memory is used.
 *
 * WHY POWER OF 2: Using 64 (2^6) lets us use a fast bitmask trick
 * instead of slow modulo (%) when advancing the head/tail pointers.
 * This matters on embedded processors running at low clock speeds.
 */
#define RING_CAPACITY    64       /* max packets the buffer can hold     */
#define RING_MASK        (RING_CAPACITY - 1)  /* bitmask for wrap-around */
#define MAX_PAYLOAD_LEN  256      /* max bytes in one packet's payload   */

/* ── Packet type ─────────────────────────────────────────────────────────
 *
 * This represents one unit of data stored in the buffer.
 * We're modeling a simplified satellite telemetry packet —
 * the kind used in CCSDS (the real protocol NASA uses).
 *
 * typedef struct { ... } name_t;
 *   — "typedef" lets us write "packet_t" instead of "struct packet"
 *   — the "_t" suffix is a C convention meaning "this is a type"
 */
typedef struct {
    uint16_t apid;                    /* which subsystem sent this       */
    uint16_t seq_count;               /* packet sequence number 0-16383  */
    uint32_t timestamp_ms;            /* when it was received            */
    uint16_t payload_len;             /* how many bytes in payload[]     */
    uint8_t  payload[MAX_PAYLOAD_LEN];/* the actual data bytes           */
} packet_t;

/* ── Buffer status codes ─────────────────────────────────────────────────
 *
 * An enum is a list of named integers. We use it for return values
 * so callers can check what happened with a readable name instead of
 * a magic number like -1 or -2.
 */
typedef enum {
    RB_OK       =  0,   /* operation succeeded                          */
    RB_FULL     = -1,   /* write failed — no space left                 */
    RB_EMPTY    = -2,   /* read failed — nothing to read                */
    RB_NULL     = -3    /* caller passed a NULL pointer                  */
} rb_status_t;

/* ── Statistics snapshot ─────────────────────────────────────────────────
 *
 * A read-only view of the buffer's health at one moment in time.
 * The stats() function fills one of these in and returns it to the caller.
 * This mirrors Health & Status (H&S) reporting in real spacecraft FSW.
 */
typedef struct {
    uint32_t capacity;           /* total slots (always RING_CAPACITY)  */
    uint32_t used;               /* packets currently in the buffer     */
    uint32_t free;               /* empty slots remaining               */
    float    utilization_pct;    /* used / capacity * 100               */
    uint64_t total_written;      /* lifetime packets successfully written*/
    uint64_t total_read;         /* lifetime packets successfully read  */
    uint64_t total_dropped;      /* packets lost due to overflow        */
    float    drop_rate_pct;      /* dropped / (written+dropped) * 100   */
} rb_stats_t;

/* ── The ring buffer itself ──────────────────────────────────────────────
 *
 * This is the main data structure. Think of it as the "object" in
 * an object-oriented language — it holds both the data (slots[])
 * and the state (head, tail, count).
 *
 * Every function below takes a pointer to one of these.
 *
 * WHY A FIXED ARRAY:
 *   In aerospace, we avoid malloc() in flight-critical code.
 *   malloc() can fail, fragment memory, and has non-deterministic
 *   timing. A fixed array allocated once at startup is predictable
 *   and safe. This is a real rule in avionics software (DO-178C).
 */
typedef struct {
    packet_t        slots[RING_CAPACITY]; /* the circular array of packets */
    uint32_t        head;                 /* index of next slot to READ    */
    uint32_t        tail;                 /* index of next slot to WRITE   */
    uint32_t        count;                /* number of packets currently held */
    uint64_t        total_written;        /* lifetime successful writes     */
    uint64_t        total_read;           /* lifetime successful reads      */
    uint64_t        total_dropped;        /* overflow events (lost packets) */
    pthread_mutex_t lock;                 /* mutex: only one thread at a time */
} ring_buf_t;

/* ── Function declarations ───────────────────────────────────────────────
 *
 * These tell the compiler "these functions exist — trust me."
 * The actual code is in ring_buffer.c. This is how C connects files:
 * you declare functions here, implement them there, and every file
 * that includes this header can call them.
 *
 * "const" on a pointer means the function promises not to modify
 * what the pointer points to — a safety guarantee to the caller.
 */

/*
 * rb_init — must be called ONCE before any other function.
 * Sets head/tail/count to 0 and initializes the mutex.
 * Returns RB_OK on success, RB_NULL if rb is NULL.
 */
rb_status_t rb_init(ring_buf_t *rb);

/*
 * rb_write — copy one packet into the buffer.
 * Returns RB_OK on success.
 * Returns RB_FULL if the buffer has no space (packet is dropped).
 * Returns RB_NULL if either pointer is NULL.
 * Thread-safe: safe to call from a different thread than rb_read().
 */
rb_status_t rb_write(ring_buf_t *rb, const packet_t *pkt);

/*
 * rb_read — remove one packet from the buffer and copy it to pkt_out.
 * Returns RB_OK on success.
 * Returns RB_EMPTY if there is nothing to read.
 * Returns RB_NULL if either pointer is NULL.
 * Thread-safe: safe to call from a different thread than rb_write().
 */
rb_status_t rb_read(ring_buf_t *rb, packet_t *pkt_out);

/*
 * rb_stats — fill a rb_stats_t with a snapshot of the buffer's state.
 * Does not modify the buffer — safe to call at any time.
 * Returns RB_NULL if either pointer is NULL, RB_OK otherwise.
 */
rb_status_t rb_stats(ring_buf_t *rb, rb_stats_t *out);

/*
 * rb_is_empty / rb_is_full — convenience checkers.
 * Return 1 (true) or 0 (false). Thread-safe.
 */
int rb_is_empty(ring_buf_t *rb);
int rb_is_full(ring_buf_t *rb);

/*
 * rb_destroy — release the mutex when you're done with the buffer.
 * Call this once at program shutdown. Do not use the buffer after this.
 */
void rb_destroy(ring_buf_t *rb);

#endif /* RING_BUFFER_H */
