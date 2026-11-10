#include "audio_stream_ring.h"

#include <string.h>

void audio_stream_ring_init(audio_stream_ring_t *r, uint8_t *storage, size_t size)
{
    r->buf = storage;
    r->size = size;
    atomic_init(&r->head, 0);
    atomic_init(&r->tail, 0);
}

size_t audio_stream_ring_capacity(const audio_stream_ring_t *r)
{
    return r->size - 1;
}

size_t audio_stream_ring_fill(const audio_stream_ring_t *r)
{
    size_t head = atomic_load_explicit(&r->head, memory_order_acquire);
    size_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);
    return (head + r->size - tail) % r->size;
}

bool audio_stream_ring_write(audio_stream_ring_t *r, const uint8_t *data, size_t len)
{
    size_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);
    size_t used = (head + r->size - tail) % r->size;
    if (len > r->size - 1 - used) {
        return false;
    }
    size_t first = r->size - head;
    if (first > len) {
        first = len;
    }
    memcpy(r->buf + head, data, first);
    memcpy(r->buf, data + first, len - first);
    atomic_store_explicit(&r->head, (head + len) % r->size, memory_order_release);
    return true;
}

size_t audio_stream_ring_read(audio_stream_ring_t *r, uint8_t *out, size_t max)
{
    size_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    size_t head = atomic_load_explicit(&r->head, memory_order_acquire);
    size_t used = (head + r->size - tail) % r->size;
    size_t n = used < max ? used : max;
    size_t first = r->size - tail;
    if (first > n) {
        first = n;
    }
    memcpy(out, r->buf + tail, first);
    memcpy(out + first, r->buf, n - first);
    atomic_store_explicit(&r->tail, (tail + n) % r->size, memory_order_release);
    return n;
}
