/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef VAV4L2_BYTESTREAM_H
#define VAV4L2_BYTESTREAM_H
#include <stdlib.h>
#include <string.h>
#include "va_v4l2.h"
struct bits { uint8_t data[8192]; size_t bit; int error; };
static inline void put(struct bits *b, unsigned n, uint32_t v)
{
    if (n > 32 || b->bit > sizeof(b->data) * 8 || n > sizeof(b->data) * 8 - b->bit) { b->error = 1; return; }
    for (unsigned i = n; i; i--, b->bit++) b->data[b->bit / 8] |= ((v >> (i - 1)) & 1) << (7 - b->bit % 8);
}
static inline void ue(struct bits *b, uint32_t v)
{
    if (v == UINT32_MAX) { b->error = 1; return; }
    unsigned n = 0; uint32_t code = v + 1;
    for (uint32_t x = code; x >>= 1;) n++;
    put(b, n, 0); put(b, n + 1, code);
}
static inline void se(struct bits *b, int v) { ue(b, v > 0 ? 2u * (unsigned)v - 1 : 2u * (unsigned)(-(int64_t)v)); }
static inline int nal(struct bits *b, unsigned type, int hevc, uint8_t *dst, size_t cap, size_t *used)
{
    put(b, 1, 1); while (!b->error && b->bit % 8) put(b, 1, 0);
    if (b->error || *used > cap || cap - *used < 6) return -1;
    size_t n = *used; dst[n++] = 0; dst[n++] = 0; dst[n++] = 0; dst[n++] = 1;
    dst[n++] = hevc ? type << 1 : 0x60 | type; if (hevc) dst[n++] = 1;
    unsigned zeros = 0;
    for (size_t i = 0; i < b->bit / 8; i++) {
        unsigned x = b->data[i];
        if (zeros >= 2 && x <= 3) { if (n >= cap) return -1; dst[n++] = 3; zeros = 0; }
        if (n >= cap) return -1;
        dst[n++] = x; zeros = x ? 0 : zeros + 1;
    }
    *used = n; return 0;
}
static inline unsigned scan(unsigned index, unsigned side, int diagonal)
{
    unsigned n = 0;
    for (unsigned sum = 0; sum < 2 * side - 1; sum++) {
        for (unsigned j = 0; j <= sum; j++) {
            unsigned x = diagonal || (sum & 1) ? sum - j : j, y = sum - x;
            if (x < side && y < side && n++ == index) return y * side + x;
        }
    }
    return 0;
}

static inline int vav4l2_packet_buffers_valid(enum vav4l2_codec codec, struct vav4l2_buffer **buffers, int count)
{
    if (!buffers || count <= 0 || count > 256) return 0;
    for (int i = 0; i < count; i++) {
        struct vav4l2_buffer *b = buffers[i];
        if (!b || !b->data || (b->allocated_elements && b->num_elements > b->allocated_elements)) return 0;
        size_t need = 0;
        if (b->type == VAPictureParameterBufferType)
            need = codec == VAV4L2_CODEC_H264 ? sizeof(VAPictureParameterBufferH264) :
                codec == VAV4L2_CODEC_HEVC ? sizeof(VAPictureParameterBufferHEVC) :
                codec == VAV4L2_CODEC_AV1 ? sizeof(VADecPictureParameterBufferAV1) : sizeof(VADecPictureParameterBufferVP9);
        else if (b->type == VAIQMatrixBufferType)
            need = codec == VAV4L2_CODEC_H264 ? sizeof(VAIQMatrixBufferH264) : sizeof(VAIQMatrixBufferHEVC);
        if (b->size < need) return 0;
    }
    return 1;
}
VAStatus vav4l2_h264_headers(struct vav4l2_context *, const struct vav4l2_collected *, struct vav4l2_buffer **, int, const uint8_t *, size_t, uint8_t *, size_t, size_t *, struct vav4l2_packet_state *);
VAStatus vav4l2_hevc_headers(struct vav4l2_context *, const struct vav4l2_collected *, const uint8_t *, size_t, uint8_t *, size_t, size_t *, int);
VAStatus vav4l2_h264_rewrite(const struct vav4l2_collected *, struct vav4l2_buffer **, int, const uint8_t *, size_t, uint8_t *, size_t, size_t *);
VAStatus vav4l2_hevc_rewrite(const struct vav4l2_collected *, struct vav4l2_buffer **, int, const uint8_t *, size_t, uint8_t *, size_t, size_t *);
VAStatus vav4l2_av1_packet_prepare(struct vav4l2_context *, struct vav4l2_buffer **, int, uint8_t *, size_t, size_t *, struct vav4l2_av1_packet **);
void vav4l2_av1_packet_free(struct vav4l2_av1_packet *);
#endif
