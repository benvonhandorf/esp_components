#pragma once

/*
 * A single-producer, single-consumer byte ring: no lock, no FreeRTOS, so it has a host
 * test with two real threads. The producer only moves `head`, the consumer only `tail`;
 * the release store of one is what the other's acquire load pairs with, which is what
 * makes the bytes between them visible on a dual-core chip.
 *
 * One storage byte stays empty so full and empty differ, so the usable capacity is
 * size - 1.
 */

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *buf;
    size_t size;                 /* storage bytes; capacity is size - 1 */
    atomic_size_t head;          /* next byte the producer writes */
    atomic_size_t tail;          /* next byte the consumer reads */
} audio_stream_ring_t;

void audio_stream_ring_init(audio_stream_ring_t *r, uint8_t *storage, size_t size);

size_t audio_stream_ring_capacity(const audio_stream_ring_t *r);

/* Bytes held. Exact from either side, an estimate from a third. */
size_t audio_stream_ring_fill(const audio_stream_ring_t *r);

/* Producer. All of `len` bytes or none: a partial block would shift the channels. */
bool audio_stream_ring_write(audio_stream_ring_t *r, const uint8_t *data, size_t len);

/* Consumer. Up to `max` bytes; the number read. */
size_t audio_stream_ring_read(audio_stream_ring_t *r, uint8_t *out, size_t max);
