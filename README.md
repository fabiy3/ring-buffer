# Memory-Safe Ring Buffer in C

A thread-safe, fixed-capacity circular buffer modeled after the data
structures used in spacecraft flight software (FSW). Built in C with
no dynamic memory allocation — a hard requirement in real-time aerospace
systems certified under DO-178C.

---

## What this is

A **ring buffer** (also called a circular buffer) is a fixed-size array
that behaves like a circle. Instead of shifting elements when you read,
a "head" pointer advances forward. Instead of appending to the end, a
"tail" pointer advances and wraps around when it hits the end of the
array. The result is O(1) reads and writes with zero memory allocation.

```
Slots:  [P3][P4][  ][  ][  ][P0][P1][P2]
              ^tail           ^head

Head = next slot to read  (oldest data)
Tail = next slot to write (newest data goes here)
```

---

## Why aerospace software uses this pattern

Spacecraft flight software follows strict rules that disallow `malloc()`
and `free()` in flight-critical code paths. The reason: dynamic memory
allocation can fail, fragment, and has non-deterministic timing — all
unacceptable when controlling a spacecraft.

The ring buffer solves this by allocating a fixed array once at startup
and never touching the heap again. This design pattern appears in:

- **Satellite telemetry pipelines** — buffer packets between a receive
  thread and a parse/dispatch thread
- **Avionics data buses** — ARINC 429 and MIL-STD-1553 drivers use
  ring buffers to decouple hardware interrupts from software processing
- **Ground station software** — buffer incoming CCSDS frames between
  network I/O and mission data processing
- **Real-time operating systems** — FreeRTOS, VxWorks, and RTEMS all
  ship ring buffer implementations for inter-task communication

---

## Project structure

```
ring-buffer/
├── include/
│   └── ring_buffer.h     ← THE DICTIONARY
│                            All types, constants, and function
│                            declarations live here. Every .c file
│                            includes this. Never contains real code.
│
├── src/
│   ├── ring_buffer.c     ← THE LIBRARY
│   │                        Implements every function declared in
│   │                        ring_buffer.h. No main(). Pure logic.
│   │                        This is the file you would ship as a
│   │                        reusable component in a real codebase.
│   │
│   └── sanity.c            ← THE WALKTHROUGH
│                            A sanity check of every feature.
│                            Run this first to understand the behavior
│                            before reading the implementation.
│
├── tests/
│   └── stress_test.c     ← THE PROOF
│                            Launches two threads simultaneously —
│                            one writing, one reading — for 5 seconds.
│                            Verifies no packets are silently lost.
│                            This is how you prove thread safety.
│
└── Makefile              ← THE BUILD SCRIPT
                             Type `make` to compile everything.
                             Type `make clean` to remove binaries.
```

---

## How to build and run

**Prerequisites:** GCC and Make installed. On Ubuntu/WSL:

```bash
sudo apt install build-essential
```

**Build:**

```bash
make
```

**Run the Sanity Check first** — it walks through every feature with printed output:

```bash
./sanity
```

**Then run the stress test** — proves thread safety under heavy load:

```bash
./stress_test
```

---

## Sample output

### Sanity Check

```
── Step 2: Write 5 packets from 3 different subsystems
  wrote APID=0x001 seq=0
  wrote APID=0x002 seq=0
  wrote APID=0x001 seq=1
  [after writes] used=5/64  written=5  read=0  dropped=0

── Step 3: Read packets back (should come out in same order)
  read  APID=0x001 seq=0
  read  APID=0x002 seq=0
  read  APID=0x001 seq=1

── Step 4: Fill the buffer to capacity (64 slots), then try to overflow
  Buffer is full: yes
  Overflow correctly detected — extra packet dropped
```

### Stress test

```
[t=1s] written=8896    read=8896    dropped=21555880  buffer=0/64
[t=5s] written=45120   read=45120   dropped=108611635 buffer=0/64

  RESULT: PASS — no packets lost or corrupted
          written (45120) == read (45120) + buffered (0)
```

---

## Key implementation decisions

### No malloc — fixed array only

```c
typedef struct {
    packet_t slots[RING_CAPACITY];  /* fixed array, allocated on the stack */
    ...
} ring_buf_t;
```

`RING_CAPACITY` is a compile-time constant. The entire buffer is allocated
once when the struct is created. No heap, no fragmentation, no failure modes.

### Bitmask instead of modulo

```c
/* Slow — integer division every time */
tail = (tail + 1) % RING_CAPACITY;

/* Fast — single AND instruction */
tail = (tail + 1) & RING_MASK;   /* RING_MASK = RING_CAPACITY - 1 = 63 */
```

This works because `RING_CAPACITY` is a power of 2 (64 = 2⁶). ANDing with
63 (`0b00111111`) zeroes out any bit above position 5, keeping the result
in the range 0–63. This is a standard embedded systems optimization.

### Mutex for thread safety

```c
pthread_mutex_lock(&rb->lock);
/* critical section — only one thread here at a time */
pthread_mutex_unlock(&rb->lock);
```

Every read and write locks a mutex before touching shared state and
unlocks before returning — including on error paths. Forgetting to
unlock on an error return is a classic deadlock bug; this implementation
avoids it by unlocking at every exit point.

### Drop counting

On overflow, the buffer increments `total_dropped` instead of silently
overwriting old data. This mirrors how real ground systems handle overflow
— the drop rate is reported in health and status telemetry so operators
know when the buffer is undersized for the data rate.

### Correctness invariant

At any point in time, this must hold:

```
total_written == total_read + count + total_dropped
```

The stress test verifies this after 5 seconds of concurrent load.

---

## Integrating into the CCSDS parser

To use this buffer in the CCSDS telemetry parser project, replace the
inline `recvfrom()` → `parse()` call in `main.c` with two threads:

```c
/* Thread 1: receive packets from UDP, push into ring buffer */
void *receiver_thread(void *arg) {
    packet_t pkt;
    while (1) {
        /* receive from UDP socket into pkt ... */
        rb_write(&shared_buf, &pkt);
    }
}

/* Thread 2: pull from ring buffer, parse, dispatch */
void *parser_thread(void *arg) {
    packet_t pkt;
    while (1) {
        if (rb_read(&shared_buf, &pkt) == RB_OK) {
            ccsds_parse(...);
            ccsds_dispatch(...);
        }
    }
}
```

This decouples network I/O from packet processing — if the parser is
momentarily slow, incoming packets queue in the buffer instead of
being dropped at the socket level.

---

## Concepts demonstrated

| Concept | Where |
|---|---|
| Fixed-size arrays (no malloc) | `ring_buffer.h` — `slots[RING_CAPACITY]` |
| Struct design | `ring_buffer.h` — `ring_buf_t` |
| Bitmask wrap-around | `ring_buffer.c` — `& RING_MASK` |
| Mutex / thread safety | `ring_buffer.c` — every function |
| Return code conventions | `ring_buffer.h` — `rb_status_t` enum |
| Pointer safety (NULL checks) | `ring_buffer.c` — every function entry |
| POSIX threads | `tests/stress_test.c` — `pthread_create` |
| Correctness verification | `tests/stress_test.c` — invariant check |

---

## Aerospace relevance

This implementation reflects real constraints from spacecraft flight
software development:

- **DO-178C** (avionics software standard) prohibits dynamic memory
  allocation in flight-critical code — hence the fixed array
- **CCSDS** (the satellite telemetry standard) uses sequence-counted
  packets — hence the `seq_count` field in `packet_t`
- **Health and Status reporting** in FSW always tracks drop rates —
  hence `total_dropped` and the `rb_stats()` function
- **Two-thread producer/consumer** is the standard FSW architecture
  for separating I/O from processing — hence the mutex design
