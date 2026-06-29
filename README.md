# Ring Buffer — Memory-Safe Circular Buffer in C

A small, thread-safe, fixed-capacity circular buffer implementation in plain C.
It uses a statically allocated array (no dynamic allocation), a mutex for
thread-safety, and a power-of-two capacity for efficient wrap-around.

## Features

- Fixed-size buffer (no `malloc`/`free`)
- O(1) reads and writes with bitmask wrap-around
- POSIX `pthread` mutex for safe producer/consumer use from threads
- Drop counting on overflow (no silent overwrites)
- Simple API and deterministic behavior for real-time systems

## Project structure

```
ring-buffer/
├── include/
│   └── ring_buffer.h     # public API and types
├── src/
│   ├── ring_buffer.c     # implementation
│   └── sanity.c          # simple walkthrough / demo
├── tests/
│   └── stress_test.c     # concurrent producer/consumer verification
└── Makefile
```

## Build

Requires a C compiler and `make`.

```sh
make
```

This produces the `sanity` and `stress_test` binaries.

## Run

Run the sanity check to see the API behavior and example output:

```sh
./sanity
```

Run the stress test to verify the correctness invariant under concurrent
load (producer + consumer threads):

```sh
./stress_test
```

The stress test checks the invariant:

```
total_written == total_read + in_buffer + total_dropped
```

## Design notes

- No dynamic memory: the buffer stores `packet_t slots[RING_CAPACITY]`.
- Power-of-two capacity enables `index = (index + 1) & RING_MASK` wrap.
- Mutex-protected operations; every exit path unlocks on error.
- On overflow, the implementation increments a drop counter instead of
  overwriting oldest data silently.

## Where to look

- API declarations: [include/ring_buffer.h](include/ring_buffer.h)
- Implementation: [src/ring_buffer.c](src/ring_buffer.c)
- Sanity/demo: [src/sanity.c](src/sanity.c)
- Concurrency proof: [tests/stress_test.c](tests/stress_test.c)

## Notes

This implementation targets deterministic, memory-safe environments such
as embedded or real-time systems where dynamic allocation is disallowed.
