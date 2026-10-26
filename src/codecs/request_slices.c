/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Owned slice plans and shared Request submission. */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "request_slices.h"
struct vav4l2_slice_plan {
    uint8_t *data;
    struct vav4l2_request_slice *slices;
    unsigned count;
};
void vav4l2_request_slice_free(struct vav4l2_slice_plan *p)
{
    if (!p) return;
    for (unsigned i = 0; i < p->count; i++) free(p->slices[i].entries);
    free(p->slices); free(p->data); free(p);
}
VAStatus vav4l2_request_slice_plan(struct vav4l2_context *ctx, struct vav4l2_buffer **buffers, int count,
    const uint8_t *data, size_t size, const void *pic, const void *decode, struct vav4l2_slice_plan **result)
{
    if (!ctx || !buffers || count < 1 || count > 256 || !data || !size || size > UINT32_MAX || !pic || !decode || !result ||
        (ctx->codec != VAV4L2_CODEC_H264 && ctx->codec != VAV4L2_CODEC_HEVC)) return VA_STATUS_ERROR_INVALID_PARAMETER;
    _Static_assert(offsetof(VASliceParameterBufferH264, slice_data_flag) == offsetof(VASliceParameterBufferHEVC, slice_data_flag), "common VA slice prefix");
    struct vav4l2_slice_plan *p = calloc(1, sizeof(*p));
    if (!p) return VA_STATUS_ERROR_ALLOCATION_FAILED;
    p->data = malloc(size);
    if (!p->data) { free(p); return VA_STATUS_ERROR_ALLOCATION_FAILED; }
    memcpy(p->data, data, size);
    unsigned entries = 0;
    if (ctx->codec == VAV4L2_CODEC_HEVC) {
        struct v4l2_query_ext_ctrl q = {.id = V4L2_CID_STATELESS_HEVC_ENTRY_POINT_OFFSETS};
        if (vav4l2_xioctl(ctx->v4l2_fd, VIDIOC_QUERY_EXT_CTRL, &q) == 0 &&
            !(q.flags & (V4L2_CTRL_FLAG_DISABLED | V4L2_CTRL_FLAG_READ_ONLY)) && q.elem_size == sizeof(uint32_t))
            entries = (q.nr_of_dims == 1 ? q.dims[0] : q.elems) < size / 2 ? (q.nr_of_dims == 1 ? q.dims[0] : q.elems) : size / 2;
    }
    size_t offset = 0, length; const uint8_t *nal;
    VAStatus status = VA_STATUS_SUCCESS;
    for (int i = 0; i < count && status == VA_STATUS_SUCCESS; i++) {
        const struct vav4l2_buffer *b = buffers[i];
        if (!b || b->type != VASliceParameterBufferType) continue;
        size_t need = ctx->codec == VAV4L2_CODEC_H264 ? sizeof(VASliceParameterBufferH264) : sizeof(VASliceParameterBufferHEVC);
        unsigned n = b->num_elements ? b->num_elements : 1;
        if (!b->data || b->size < need || (b->allocated_elements && n > b->allocated_elements) || n > UINT32_MAX / b->size) {
            status = VA_STATUS_ERROR_INVALID_PARAMETER; break;
        }
        for (unsigned j = 0; j < n && status == VA_STATUS_SUCCESS; j++) {
            const uint8_t *va = (const uint8_t *)b->data + (size_t)j * b->size;
            uint32_t flag;
            memcpy(&flag, va + offsetof(VASliceParameterBufferH264, slice_data_flag), sizeof(flag));
            if (flag != VA_SLICE_DATA_FLAG_ALL && flag != VA_SLICE_DATA_FLAG_BEGIN) continue;
            if (p->count >= 65536 || vav4l2_annexb_next(p->data, size, &offset, &nal, &length) != 1 || length < 5 || length > UINT32_MAX / 8) {
                status = VA_STATUS_ERROR_INVALID_PARAMETER; break;
            }
            struct vav4l2_request_slice *grown = realloc(p->slices, (p->count + 1u) * sizeof(*grown));
            if (!grown) { status = VA_STATUS_ERROR_ALLOCATION_FAILED; break; }
            p->slices = grown;
            struct vav4l2_request_slice *s = &p->slices[p->count++]; memset(s, 0, sizeof(*s));
            unsigned prefix = nal[2] == 1 ? 3 : 4;
            s->offset = nal - p->data + (ctx->request_no_start_code ? prefix : 0);
            s->size = length - (ctx->request_no_start_code ? prefix : 0);
            if (ctx->codec == VAV4L2_CODEC_H264) {
                VASliceParameterBufferH264 value; memcpy(&value, va, sizeof(value));
                status = vav4l2_h264_request_slice(ctx, pic, &value, decode, nal + prefix, length - prefix, s);
            } else {
                VASliceParameterBufferHEVC value; memcpy(&value, va, sizeof(value));
                status = vav4l2_hevc_request_slice(ctx, pic, &value, decode, nal + prefix, length - prefix, entries, s);
            }
        }
    }
    if (status == VA_STATUS_SUCCESS && (!p->count || vav4l2_annexb_next(p->data, size, &offset, &nal, &length) != 0))
        status = VA_STATUS_ERROR_INVALID_PARAMETER;
    if (status != VA_STATUS_SUCCESS) { vav4l2_request_slice_free(p); return status; }
    *result = p; return VA_STATUS_SUCCESS;
}
/* Frame mode carries every slice in one OUTPUT buffer and one dynamic control
 * array. Slice mode keeps one slice per request and HOLD_CAPTURE semantics. */
static VAStatus hevc_frame_submit(struct vav4l2_context *ctx, const struct vav4l2_slice_plan *p,
    struct v4l2_ext_control *base, unsigned count)
{
    struct v4l2_query_ext_ctrl q = {.id = V4L2_CID_STATELESS_HEVC_SLICE_PARAMS};
    if (vav4l2_xioctl(ctx->v4l2_fd, VIDIOC_QUERY_EXT_CTRL, &q) < 0 || q.nr_of_dims != 1 ||
        (q.flags & (V4L2_CTRL_FLAG_DISABLED | V4L2_CTRL_FLAG_READ_ONLY)) || !(q.flags & V4L2_CTRL_FLAG_DYNAMIC_ARRAY) || q.elem_size != sizeof(struct v4l2_ctrl_hevc_slice_params) ||
        p->count > q.dims[0]) return VA_STATUS_ERROR_UNIMPLEMENTED;
    size_t bytes = 0; unsigned entries = 0;
    for (unsigned i = 0; i < p->count; i++) {
        if (p->slices[i].size > ctx->output_buf_size - bytes || p->slices[i].num_entries > UINT32_MAX - entries)
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        bytes += p->slices[i].size; entries += p->slices[i].num_entries;
    }
    if (entries) {
        q = (struct v4l2_query_ext_ctrl){.id = V4L2_CID_STATELESS_HEVC_ENTRY_POINT_OFFSETS};
        if (vav4l2_xioctl(ctx->v4l2_fd, VIDIOC_QUERY_EXT_CTRL, &q) < 0 || q.elem_size != sizeof(uint32_t) ||
            q.nr_of_dims != 1 || (q.flags & (V4L2_CTRL_FLAG_DISABLED | V4L2_CTRL_FLAG_READ_ONLY)) ||
            !(q.flags & V4L2_CTRL_FLAG_DYNAMIC_ARRAY) || entries > q.dims[0]) return VA_STATUS_ERROR_UNIMPLEMENTED;
    }
    struct v4l2_ctrl_hevc_slice_params *slices = calloc(p->count, sizeof(*slices));
    uint32_t *offsets = entries ? calloc(entries, sizeof(*offsets)) : NULL;
    if (!slices || (entries && !offsets)) { free(slices); free(offsets); return VA_STATUS_ERROR_ALLOCATION_FAILED; }
    unsigned entry = 0;
    for (unsigned i = 0; i < p->count; i++) {
        slices[i] = p->slices[i].hevc;
        if (p->slices[i].num_entries) memcpy(offsets + entry, p->slices[i].entries, p->slices[i].num_entries * sizeof(*offsets));
        entry += p->slices[i].num_entries;
    }
    struct v4l2_ext_control controls[6]; memcpy(controls, base, count * sizeof(*base)); unsigned n = count;
    controls[n++] = (struct v4l2_ext_control){.id = V4L2_CID_STATELESS_HEVC_SLICE_PARAMS, .ptr = slices, .size = p->count * sizeof(*slices)};
    if (entries) controls[n++] = (struct v4l2_ext_control){.id = V4L2_CID_STATELESS_HEVC_ENTRY_POINT_OFFSETS, .ptr = offsets, .size = entries * sizeof(*offsets)};
    struct v4l2_ext_controls batch = {.count = n, .controls = controls};
    VAStatus status = VA_STATUS_SUCCESS;
    if (!ctx->n_free_out || vav4l2_codec_controls(ctx, ctx->request_fd, &batch) < 0) status = VA_STATUS_ERROR_OPERATION_FAILED;
    else {
        int index = ctx->free_out_bufs[--ctx->n_free_out]; size_t used = 0;
        if (index < 0 || index >= ctx->output_bufs_allocd || !ctx->output_buf_ptr[index]) status = VA_STATUS_ERROR_OPERATION_FAILED;
        else {
            for (unsigned i = 0; i < p->count; i++) {
                memcpy((uint8_t *)ctx->output_buf_ptr[index] + used, p->data + p->slices[i].offset, p->slices[i].size); used += p->slices[i].size;
            }
            int cap = vav4l2_decode_submit(ctx, index, used, ctx->current_surface ? ctx->current_surface->timestamp : 0);
            if (cap < 0) status = cap == -2 ? VA_STATUS_ERROR_DECODING_ERROR : VA_STATUS_ERROR_OPERATION_FAILED;
            else if (ctx->current_surface) {
                if (vav4l2_surface_pull_capture(ctx, ctx->current_surface, cap) < 0) { vav4l2_cap_pool_push(ctx, cap); status = VA_STATUS_ERROR_OPERATION_FAILED; }
            } else vav4l2_cap_pool_push(ctx, cap);
        }
    }
    free(slices); free(offsets); return status;
}
VAStatus vav4l2_request_slice_submit(struct vav4l2_context *ctx, struct vav4l2_slice_plan *p,
    struct v4l2_ext_control *base, unsigned count)
{
    if (!p || !p->count || count > 4) return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (ctx->codec == VAV4L2_CODEC_HEVC && !ctx->request_slice_based) return hevc_frame_submit(ctx, p, base, count);
    if (p->count > 1 && !vav4l2_h264_field_supported(ctx)) return VA_STATUS_ERROR_UNIMPLEMENTED;
    for (unsigned i = 0; i < p->count; i++) if (p->slices[i].size > ctx->output_buf_size)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    int capture = -1; VAStatus status = VA_STATUS_SUCCESS;
    for (unsigned i = 0; i < p->count; i++) {
        struct vav4l2_request_slice *s = &p->slices[i];
        struct v4l2_ext_control controls[7]; memcpy(controls, base, count * sizeof(*base)); unsigned n = count;
        struct v4l2_ctrl_h264_decode_params hd;
        struct v4l2_ctrl_hevc_decode_params ed;
        if (ctx->codec == VAV4L2_CODEC_H264) {
            for (unsigned j = 0; j < count; j++) if (controls[j].id == V4L2_CID_STATELESS_H264_DECODE_PARAMS) {
                hd = *(const struct v4l2_ctrl_h264_decode_params *)controls[j].ptr;
                hd.pic_order_cnt_bit_size = s->h264_info.pic_order_cnt_bit_size;
                hd.dec_ref_pic_marking_bit_size = s->h264_info.dec_ref_pic_marking_bit_size;
                controls[j].ptr = &hd;
            }
            controls[n++] = (struct v4l2_ext_control){.id = V4L2_CID_STATELESS_H264_SLICE_PARAMS, .ptr = &s->h264, .size = sizeof(s->h264)};
            /* Sending weights is harmless only when the UAPI requires them. */
            const struct v4l2_ctrl_h264_pps *pps = NULL;
            for (unsigned j = 0; j < count; j++) if (controls[j].id == V4L2_CID_STATELESS_H264_PPS) pps = controls[j].ptr;
            if (pps && V4L2_H264_CTRL_PRED_WEIGHTS_REQUIRED(pps, &s->h264))
                controls[n++] = (struct v4l2_ext_control){.id = V4L2_CID_STATELESS_H264_PRED_WEIGHTS, .ptr = &s->weights, .size = sizeof(s->weights)};
        } else {
            for (unsigned j = 0; j < count; j++) if (controls[j].id == V4L2_CID_STATELESS_HEVC_DECODE_PARAMS) {
                ed = *(const struct v4l2_ctrl_hevc_decode_params *)controls[j].ptr;
                ed.short_term_ref_pic_set_size = s->hevc_info.short_term_ref_pic_set_size;
                ed.long_term_ref_pic_set_size = s->hevc_info.long_term_ref_pic_set_size;
                ed.num_delta_pocs_of_ref_rps_idx = s->hevc_info.num_delta_pocs_of_ref_rps_idx;
                controls[j].ptr = &ed;
            }
            controls[n++] = (struct v4l2_ext_control){.id = V4L2_CID_STATELESS_HEVC_SLICE_PARAMS, .ptr = &s->hevc, .size = sizeof(s->hevc)};
            if (s->num_entries) controls[n++] = (struct v4l2_ext_control){.id = V4L2_CID_STATELESS_HEVC_ENTRY_POINT_OFFSETS,
                .ptr = s->entries, .size = s->num_entries * sizeof(*s->entries)};
        }
        struct v4l2_ext_controls batch = {.count = n, .controls = controls};
        if (!ctx->n_free_out || vav4l2_codec_controls(ctx, ctx->request_fd, &batch) < 0) { status = VA_STATUS_ERROR_OPERATION_FAILED; break; }
        int index = ctx->free_out_bufs[--ctx->n_free_out];
        if (!ctx->output_buf_ptr[index]) { vav4l2_out_pool_push(ctx, index); status = VA_STATUS_ERROR_OPERATION_FAILED; break; }
        memcpy(ctx->output_buf_ptr[index], p->data + s->offset, s->size);
        int hold = i + 1 < p->count;
        int done = vav4l2_decode_submit_field(ctx, index, s->size,
            ctx->current_surface ? ctx->current_surface->timestamp : 0, capture, hold);
        if (done < 0) { if (done == -2 && p->count > 1) vav4l2_decode_reset(ctx); capture = -1; status = done == -2 ? VA_STATUS_ERROR_DECODING_ERROR : VA_STATUS_ERROR_OPERATION_FAILED; break; }
        capture = done;
    }
    if (status != VA_STATUS_SUCCESS) { if (capture >= 0) vav4l2_decode_reset(ctx); return status; }
    if (ctx->current_surface && vav4l2_surface_pull_capture(ctx, ctx->current_surface, capture) < 0) {
        if (!vav4l2_surface_capture_matches(ctx, ctx->current_surface) || ctx->current_surface->buf_index != capture)
            vav4l2_cap_pool_push(ctx, capture);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    if (!ctx->current_surface) vav4l2_cap_pool_push(ctx, capture);
    return VA_STATUS_SUCCESS;
}

VAStatus vav4l2_request_slice_decode(struct vav4l2_context *ctx, struct vav4l2_slice_plan *plan,
    void *sps, void *pps, void *dec, void *sm, int matrix, unsigned width, unsigned height, uint32_t format)
{
    int h264 = ctx->codec == VAV4L2_CODEC_H264;
    struct v4l2_ext_control controls[] = {
        {.id = h264 ? V4L2_CID_STATELESS_H264_SPS : V4L2_CID_STATELESS_HEVC_SPS, .ptr = sps,
         .size = h264 ? sizeof(struct v4l2_ctrl_h264_sps) : sizeof(struct v4l2_ctrl_hevc_sps)},
        {.id = h264 ? V4L2_CID_STATELESS_H264_PPS : V4L2_CID_STATELESS_HEVC_PPS, .ptr = pps,
         .size = h264 ? sizeof(struct v4l2_ctrl_h264_pps) : sizeof(struct v4l2_ctrl_hevc_pps)},
        {.id = h264 ? V4L2_CID_STATELESS_H264_DECODE_PARAMS : V4L2_CID_STATELESS_HEVC_DECODE_PARAMS, .ptr = dec,
         .size = h264 ? sizeof(struct v4l2_ctrl_h264_decode_params) : sizeof(struct v4l2_ctrl_hevc_decode_params)},
        {.id = h264 ? V4L2_CID_STATELESS_H264_SCALING_MATRIX : V4L2_CID_STATELESS_HEVC_SCALING_MATRIX, .ptr = sm,
         .size = h264 ? sizeof(struct v4l2_ctrl_h264_scaling_matrix) : sizeof(struct v4l2_ctrl_hevc_scaling_matrix)},
    };
    struct v4l2_ext_controls global = {.count = 1, .controls = controls};
    if (!ctx->g_ctrl_valid || memcmp(ctx->g_ctrl_payload, sps, controls[0].size)) {
        if (vav4l2_codec_controls(ctx, -1, &global) < 0) return VA_STATUS_ERROR_OPERATION_FAILED;
        memcpy(ctx->g_ctrl_payload, sps, controls[0].size); ctx->g_ctrl_valid = 1;
    }
    if (vav4l2_ensure_capture(ctx, width, height, format) < 0) return VA_STATUS_ERROR_OPERATION_FAILED;
    if (!ctx->streamed) {
        if (vav4l2_streamon(ctx->v4l2_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) < 0 ||
            vav4l2_streamon(ctx->v4l2_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) < 0) return VA_STATUS_ERROR_OPERATION_FAILED;
        ctx->streamed = 1;
    }
    return vav4l2_request_slice_submit(ctx, plan, controls, matrix ? 4 : 3);
}
