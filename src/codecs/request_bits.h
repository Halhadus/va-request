/*
 * Small bitstream readers used to recover slice header fields that VA-API
 * does not expose but the V4L2 stateless interfaces require.
 *
 * The RBSP variant transparently strips H.264/HEVC emulation prevention
 * bytes; bit positions are counted in RBSP bits, matching the semantics
 * of the V4L2 *_bit_size control fields.
 *
 * Copyright (C) 2026 Ondrej Jirman <megi@xff.cz>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef VAV4L2_REQUEST_BITS_H
#define VAV4L2_REQUEST_BITS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct vav4l2_bits {
    const uint8_t *data;
    size_t size;        /* in bytes */
    size_t byte;        /* next byte to load */
    uint8_t cur;        /* current byte */
    unsigned int left;  /* bits left in cur */
    size_t pos;     /* bits consumed (RBSP domain when stripping) */
    unsigned int zeros; /* consecutive zero bytes seen */
    bool rbsp;      /* strip emulation prevention bytes */
    bool error;     /* ran past the end */
};

static inline void vav4l2_bits_init(struct vav4l2_bits *b, const uint8_t *data,
                   size_t size, bool rbsp)
{
    b->data = data;
    b->size = size;
    b->byte = 0;
    b->cur = 0;
    b->left = 0;
    b->pos = 0;
    b->zeros = 0;
    b->rbsp = rbsp;
    b->error = false;
}

static inline uint32_t vav4l2_bits_bit(struct vav4l2_bits *b)
{
    if (!b->left) {
        if (b->byte >= b->size) {
            b->error = true;
            return 0;
        }

        if (b->rbsp && b->zeros >= 2 && b->data[b->byte] == 0x03) {
            b->byte++;
            b->zeros = 0;
            if (b->byte >= b->size || b->data[b->byte] > 3) {
                b->error = true;
                return 0;
            }
        }

        b->cur = b->data[b->byte++];
        b->zeros = b->cur ? 0 : b->zeros + 1;
        b->left = 8;
    }

    b->left--;
    b->pos++;
    return (b->cur >> b->left) & 1;
}

static inline uint32_t vav4l2_bits_read(struct vav4l2_bits *b, unsigned int count)
{
    uint32_t value = 0;

    if (count > 32) {
        b->error = true;
        return 0;
    }
    while (count-- && !b->error)
        value = (value << 1) | vav4l2_bits_bit(b);

    return value;
}

static inline void vav4l2_bits_skip(struct vav4l2_bits *b, size_t count)
{
    while (count-- && !b->error)
        vav4l2_bits_bit(b);
}

/* Exp-Golomb ue(v) */
static inline uint32_t vav4l2_bits_ue(struct vav4l2_bits *b)
{
    unsigned int zeros = 0;

    while (!vav4l2_bits_bit(b)) {
        if (b->error)
            return 0;
        if (zeros >= 32) {
            /* Malformed code: flag the error so callers relying on
             * b->error (and info->valid) treat the parse as failed
             * instead of silently trusting a desynced result. */
            b->error = true;
            return 0;
        }
        zeros++;
    }
    if (zeros >= 32) {
        /* 32 leading zeros then a one: the value does not fit in 32 bits */
        b->error = true;
        return 0;
    }

    return (1u << zeros) - 1 + vav4l2_bits_read(b, zeros);
}

/* Exp-Golomb se(v) */
static inline int32_t vav4l2_bits_se(struct vav4l2_bits *b)
{
    uint32_t value = vav4l2_bits_ue(b);

    if (value & 1)
        return (int32_t)((value + 1) / 2);
    return -(int32_t)(value / 2);
}

#endif /* VAV4L2_REQUEST_BITS_H */
