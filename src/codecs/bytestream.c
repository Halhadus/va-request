/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Assemble complete bytestream packets before changing OUTPUT storage. */
#include "bytestream.h"
VAStatus vav4l2_stateful_packet_prepare(struct vav4l2_context *ctx, struct vav4l2_buffer **buffers, int count, uint8_t *dst, size_t capacity, size_t *size, struct vav4l2_packet_state *prepared, struct vav4l2_packet_layout *layout)
{
    if (!ctx || !dst || !size || capacity > UINT32_MAX || capacity > SIZE_MAX / 8 || !vav4l2_packet_buffers_valid(ctx->codec, buffers, count)) return VA_STATUS_ERROR_INVALID_PARAMETER;
    struct vav4l2_packet_state next = ctx->packet_state;
    if (ctx->codec == VAV4L2_CODEC_AV1) {
        VAStatus status = vav4l2_av1_packet_prepare(ctx, buffers, count, dst, capacity, size, prepared ? &next.av1 : NULL);
        if (status == VA_STATUS_SUCCESS) {
            if (prepared) *prepared = next;
            if (layout) *layout = (struct vav4l2_packet_layout){.frame_size = {*size, 0}};
        }
        return status;
    }
    if (ctx->codec == VAV4L2_CODEC_VP9) {
        VAStatus status = vav4l2_vp9_packet(ctx, buffers, count, dst, capacity, size, layout);
        if (status == VA_STATUS_SUCCESS && prepared) *prepared = next;
        return status;
    }
    struct vav4l2_collected cb; vav4l2_collect_decode_buffers(buffers, count, &cb);
    size_t bytes = 0, used = 0;
    VAStatus status = vav4l2_packetize_slices(ctx->codec, buffers, count, NULL, UINT32_MAX, &bytes);
    if (status != VA_STATUS_SUCCESS || !bytes) return status == VA_STATUS_SUCCESS ? VA_STATUS_ERROR_INVALID_PARAMETER : status;
    if (bytes > capacity) return VA_STATUS_ERROR_ALLOCATION_FAILED;
    uint8_t *slices = malloc(bytes); if (!slices) return VA_STATUS_ERROR_ALLOCATION_FAILED;
    status = vav4l2_packetize_slices(ctx->codec, buffers, count, slices, bytes, &bytes);
    uint8_t headers[8192];
    int normalize = ctx->codec == VAV4L2_CODEC_HEVC && cb.n_slice_params &&
        ((const VASliceParameterBufferHEVC *)cb.slice_params[0])->slice_data_byte_offset >= 3;
    if (status == VA_STATUS_SUCCESS && ctx->codec == VAV4L2_CODEC_H264)
        status = vav4l2_h264_headers(ctx, &cb, buffers, count, slices, bytes, headers, sizeof(headers), &used, &next);
    else if (status == VA_STATUS_SUCCESS && ctx->codec == VAV4L2_CODEC_HEVC)
        status = vav4l2_hevc_headers(ctx, &cb, slices, bytes, headers, sizeof(headers), &used, normalize);
    else if (status == VA_STATUS_SUCCESS) status = VA_STATUS_ERROR_UNIMPLEMENTED;
    if (status == VA_STATUS_SUCCESS && ctx->codec == VAV4L2_CODEC_HEVC) {
        const uint8_t *unit; size_t cursor = 0, length;
        if (vav4l2_annexb_next(slices, bytes, &cursor, &unit, &length) != 1) status = VA_STATUS_ERROR_INVALID_PARAMETER;
        else {
            unsigned prefix = unit[2] == 1 ? 3 : 4, type = unit[prefix] >> 1 & 63;
            if ((!next.hevc_started && (type < 16 || type > 21)) || (next.hevc_no_rasl && (type == 8 || type == 9)))
                status = VA_STATUS_ERROR_DECODING_ERROR;
            if (type >= 16 && type <= 21) next.hevc_no_rasl = !next.hevc_started || type <= 20;
            next.hevc_started = 1;
        }
    }
    uint8_t *packet = status == VA_STATUS_SUCCESS ? malloc(capacity ? capacity : 1) : NULL;
    if (status == VA_STATUS_SUCCESS && !packet) status = VA_STATUS_ERROR_ALLOCATION_FAILED;
    size_t payload = 0;
    if (status == VA_STATUS_SUCCESS && used > capacity) status = VA_STATUS_ERROR_ALLOCATION_FAILED;
    if (status == VA_STATUS_SUCCESS) {
        memcpy(packet, headers, used);
        if (ctx->codec == VAV4L2_CODEC_H264)
            status = vav4l2_h264_rewrite(&cb, buffers, count, slices, bytes, packet + used, capacity - used, &payload);
        else if (normalize)
            status = vav4l2_hevc_rewrite(&cb, buffers, count, slices, bytes, packet + used, capacity - used, &payload);
        else if (bytes > capacity - used) status = VA_STATUS_ERROR_ALLOCATION_FAILED;
        else { memcpy(packet + used, slices, bytes); payload = bytes; }
    }
    if (status == VA_STATUS_SUCCESS) {
        memcpy(dst, packet, used + payload); *size = used + payload; if (prepared) *prepared = next;
        if (layout) *layout = (struct vav4l2_packet_layout){.frame_size = {*size, 0}};
    }
    free(packet); free(slices); return status;
}
VAStatus vav4l2_stateful_packet(struct vav4l2_context *ctx, struct vav4l2_buffer **buffers, int count,
    uint8_t *dst, size_t capacity, size_t *size)
{
    return vav4l2_stateful_packet_prepare(ctx, buffers, count, dst, capacity, size, NULL, NULL);
}
void vav4l2_packet_state_release(struct vav4l2_packet_state *state)
{
    vav4l2_av1_packet_free(state->av1); memset(state, 0, sizeof(*state));
}
void vav4l2_packet_state_commit(struct vav4l2_context *ctx, struct vav4l2_packet_state *prepared)
{
    vav4l2_packet_state_release(&ctx->packet_state); ctx->packet_state = *prepared; prepared->av1 = NULL;
}
