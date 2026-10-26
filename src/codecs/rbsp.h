/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef VAV4L2_RBSP_H
#define VAV4L2_RBSP_H
/* Bounded RBSP access. Bit offsets exclude emulation-prevention bytes. */
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
struct rbsp_reader { uint8_t *data; size_t size, bit; int error; };
struct rbsp_writer { uint8_t *data; size_t capacity, bit; int error; };
static inline int rbsp_open(struct rbsp_reader *r, const uint8_t *data, size_t size)
{
    *r = (struct rbsp_reader){0};
    if (size > SIZE_MAX / 8 || !(r->data = malloc(size ? size : 1))) return -1;
    unsigned zeros = 0;
    for (size_t i = 0; i < size; i++) {
        unsigned v = data[i];
        if (zeros == 2 && v == 3) {
            if (i + 1 == size || data[i + 1] > 3) { r->error = 1; break; }
            zeros = 0; continue;
        }
        r->data[r->size++] = v; zeros = v ? 0 : zeros < 2 ? zeros + 1 : 2;
    }
    return 0;
}
static inline uint32_t rbsp_get(struct rbsp_reader *r, unsigned n)
{
    if (n > 32 || r->bit > r->size * 8 || n > r->size * 8 - r->bit) { r->error = 1; return 0; }
    uint32_t v = 0;
    while (n--) { v = (v << 1) | ((r->data[r->bit / 8] >> (7 - r->bit % 8)) & 1); r->bit++; }
    return v;
}
static inline void rbsp_skip(struct rbsp_reader *r, size_t n)
{
    if (r->bit > r->size * 8 || n > r->size * 8 - r->bit) r->error = 1;
    else r->bit += n;
}
static inline uint32_t rbsp_ue(struct rbsp_reader *r)
{
    unsigned n = 0;
    while (!rbsp_get(r, 1) && !r->error) if (++n > 31) { r->error = 1; return 0; }
    return ((UINT32_C(1) << n) - 1) + rbsp_get(r, n);
}
static inline void rbsp_put(struct rbsp_writer *w, unsigned n, uint32_t v)
{
    if (n > 32 || w->bit / 8 > w->capacity || n > w->capacity * 8 - w->bit) { w->error = 1; return; }
    while (n--) {
        unsigned mask = 128u >> (w->bit % 8);
        uint8_t *p = &w->data[w->bit / 8];
        *p = (*p & ~mask) | ((v >> n & 1) ? mask : 0); w->bit++;
    }
}
static inline void rbsp_write_ue(struct rbsp_writer *w, uint32_t v)
{
    if (v == UINT32_MAX) { w->error = 1; return; }
    unsigned n = 0; for (uint32_t x = v + 1; x >>= 1;) n++;
    rbsp_put(w, n, 0); rbsp_put(w, n + 1, v + 1);
}
static inline void rbsp_write_se(struct rbsp_writer *w, int64_t v)
{
    if (v <= -(int64_t)INT32_MAX / 2 || v > INT32_MAX / 2) { w->error = 1; return; }
    rbsp_write_ue(w, v > 0 ? 2 * (uint32_t)v - 1 : 2 * (uint32_t)-v);
}
static inline void rbsp_copy(struct rbsp_writer *w, struct rbsp_reader *r, size_t end)
{
    if (end < r->bit || end > r->size * 8) { r->error = 1; return; }
    if (!r->error && !w->error && !(r->bit % 8) && !(w->bit % 8)) {
        size_t bytes = (end - r->bit) / 8;
        if (w->bit / 8 > w->capacity || bytes > w->capacity - w->bit / 8) { w->error = 1; return; }
        memcpy(w->data + w->bit / 8, r->data + r->bit / 8, bytes);
        w->bit += bytes * 8; r->bit += bytes * 8;
    }
    while (r->bit < end && !r->error && !w->error) {
        unsigned n = end - r->bit > 32 ? 32 : end - r->bit;
        rbsp_put(w, n, rbsp_get(r, n));
    }
}
static inline void rbsp_align(struct rbsp_writer *w)
{
    rbsp_put(w, 1, 1); while (w->bit % 8 && !w->error) rbsp_put(w, 1, 0);
}
static inline int rbsp_nal(struct rbsp_writer *w, const uint8_t *header, unsigned headers,
                           uint8_t *dst, size_t cap, size_t *used)
{
    if (w->error || w->bit % 8 || *used > cap || cap - *used < 4u + headers) return -1;
    size_t n = *used; memcpy(dst + n, (uint8_t[]){0,0,0,1}, 4); n += 4;
    memcpy(dst + n, header, headers); n += headers; unsigned zeros = 0;
    for (size_t i = 0; i < w->bit / 8; i++) {
        unsigned v = w->data[i];
        if (zeros == 2 && v <= 3) { if (n == cap) return -1; dst[n++] = 3; zeros = 0; }
        if (n == cap) return -1;
        dst[n++] = v; zeros = v ? 0 : zeros + 1;
    }
    *used = n; return 0;
}
static inline unsigned rbsp_ceil_log2(unsigned n)
{
    unsigned bits = 0; for (unsigned x = n ? n - 1 : 0; x; x >>= 1) bits++; return bits;
}
#endif
