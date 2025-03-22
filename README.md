# libpussy: collection of basic things

Everything is work in progress and subject to change.

## Allocators

### arena allocator

[arena.h](include/arena.h)

Uses `mmap` as underlying allocator.

### pet allocator

[allocator.h](include/allocator.h)

The main allocator is bitmap-based. Other twos are for debugging purposes:
 * wrapper for malloc/realloc/free
 * debug allocator that detects bubblewrap corruption around allocated blocks
 
## Dump functions

[dump.h](include/dump.h)

Hex and bitmap dump functions.

## MMarray

[mmarray.h](include/mmarray.h)

Simple dynamic array using `mmap` as allocator.

## Ring buffers

[ringbuffer.h](include/ringbuffer.h)

Basic and thread-safe implementations of ring buffer.
Using `mmap` as allocator.

## Synchronization primitives

[sync.h](include/sync.h)

Synchronization primitives based on condition variable.

Event implementation only for now.

## Timespec utils

[timespec.h](include/timespec.h)

Utilities that work with `struct timespec`.
