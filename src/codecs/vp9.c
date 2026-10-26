/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Stateful packet construction adapted from Radxa libva-v4l2 (MIT); see NOTICE. */
#include <stdlib.h>
#include <string.h>
#include "va_v4l2.h"
const int vav4l2_has_vp9 = 1;
struct vp9_reader { const uint8_t *data; size_t size, bit; int error; };
static unsigned vp9_get(struct vp9_reader *r, unsigned n)
{
    unsigned v = 0;
    if (n > 32 || r->bit > r->size * 8 || n > r->size * 8 - r->bit) { r->error = 1; return 0; }
    while (n--) { v = (v << 1) | (r->data[r->bit / 8] >> (7 - r->bit % 8) & 1); r->bit++; }
    return v;
}
VAStatus vav4l2_vp9_packet(struct vav4l2_context *ctx, struct vav4l2_buffer **buffers, int count,
    uint8_t *dst, size_t capacity, size_t *size, struct vav4l2_packet_layout *layout)
{
    struct vav4l2_collected cb;
    for (int i = 0; i < count; i++) if (buffers[i] && buffers[i]->type == VAPictureParameterBufferType &&
        buffers[i]->size < sizeof(VADecPictureParameterBufferVP9)) return VA_STATUS_ERROR_INVALID_PARAMETER;
    vav4l2_collect_decode_buffers(buffers, count, &cb);
    const VADecPictureParameterBufferVP9 *p = cb.pic;
    unsigned profile = ctx->profile == VAProfileVP9Profile2 ? 2 : 0;
    if (!p || !cb.n_slice_params) return VA_STATUS_ERROR_INVALID_PARAMETER;
    if ((ctx->profile != VAProfileVP9Profile0 && ctx->profile != VAProfileVP9Profile2) || p->profile != profile ||
        p->bit_depth != (profile ? 10 : 8) || !p->pic_fields.bits.subsampling_x || !p->pic_fields.bits.subsampling_y)
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    if (p->frame_width != ctx->width || p->frame_height != ctx->height) return VA_STATUS_ERROR_INVALID_PARAMETER;
    size_t bytes = 0;
    VAStatus status = vav4l2_packetize_slices(VAV4L2_CODEC_VP9, buffers, count, NULL, UINT32_MAX, &bytes);
    if (status != VA_STATUS_SUCCESS) return status;
    if (bytes < 3 || !p->frame_header_length_in_bytes || !p->first_partition_size ||
        (size_t)p->frame_header_length_in_bytes + p->first_partition_size > bytes) return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (bytes > capacity) return VA_STATUS_ERROR_ALLOCATION_FAILED;
    uint8_t *data = malloc(bytes); if (!data) return VA_STATUS_ERROR_ALLOCATION_FAILED;
    status = vav4l2_packetize_slices(VAV4L2_CODEC_VP9, buffers, count, data, bytes, &bytes);
    struct vp9_reader r = {.data = data, .size = p->frame_header_length_in_bytes};
    unsigned marker = vp9_get(&r, 2), raw_profile = vp9_get(&r, 1); raw_profile |= vp9_get(&r, 1) << 1;
    unsigned existing = vp9_get(&r, 1), frame_type = vp9_get(&r, 1), show = vp9_get(&r, 1), resilient = vp9_get(&r, 1);
    if (existing) status = VA_STATUS_ERROR_UNIMPLEMENTED;
    else if (marker != 2 || raw_profile != profile || frame_type != p->pic_fields.bits.frame_type ||
        show != p->pic_fields.bits.show_frame || resilient != p->pic_fields.bits.error_resilient_mode) status = VA_STATUS_ERROR_INVALID_PARAMETER;
    unsigned refresh = 255;
    if (status == VA_STATUS_SUCCESS && !show && frame_type) {
        unsigned intra = vp9_get(&r, 1);
        if (!resilient) vp9_get(&r, 2);
        if (intra) {
            if (vp9_get(&r, 24) != 0x498342) r.error = 1;
            if (profile) {
                if (vp9_get(&r, 1) || vp9_get(&r, 3) == 7) r.error = 1;
                vp9_get(&r, 1);
            }
        }
        refresh = vp9_get(&r, 8);
        if (!refresh) status = VA_STATUS_ERROR_UNIMPLEMENTED;
    }
    if (r.error) status = VA_STATUS_ERROR_INVALID_PARAMETER;
    size_t extra = show ? 0 : 1;
    if (status == VA_STATUS_SUCCESS && extra > capacity - bytes) status = VA_STATUS_ERROR_ALLOCATION_FAILED;
    if (status == VA_STATUS_SUCCESS) {
        memcpy(dst, data, bytes); size_t n = bytes;
        if (!show) {
            unsigned slot = 0; while (!(refresh & (1u << slot))) slot++;
            dst[n++] = 0x88 | (profile << 3) | slot;
        }
        *size = n;
        if (layout) *layout = (struct vav4l2_packet_layout){.frame_size = {bytes, extra}};
    }
    free(data); return status;
}
