/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Normalization adapted from Radxa libva-v4l2 (MIT); see NOTICE. */
#include "bytestream.h"
#include "rbsp.h"
static void h264_scaling(struct bits *b, const uint8_t *matrix, unsigned side)
{
    int last = 8;
    for (unsigned j = 0; j < side * side; j++) {
        int value = matrix ? matrix[scan(j, side, 0)] : 16;
        if (!value) { b->error = 1; return; }
        int delta = value - last; if (delta > 127) delta -= 256; if (delta < -128) delta += 256;
        se(b, delta); last = value;
    }
}
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
VAStatus vav4l2_h264_headers(struct vav4l2_context *ctx, const struct vav4l2_collected *cb, struct vav4l2_buffer **buffers,
    int count, const uint8_t *slices, size_t bytes, uint8_t *dst, size_t cap, size_t *used, struct vav4l2_packet_state *next)
{
    const VAPictureParameterBufferH264 *p = cb->pic;
    const VASliceParameterBufferH264 *slice = cb->n_slice_params ? cb->slice_params[0] : NULL;
    const VAIQMatrixBufferH264 *iq = cb->iq;
    if (!p || !slice || p->seq_fields.bits.log2_max_frame_num_minus4 > 12 || p->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 > 12 ||
        p->num_ref_frames > 16 || p->num_slice_groups_minus1 || p->seq_fields.bits.chroma_format_idc != 1 ||
        p->bit_depth_luma_minus8 || p->bit_depth_chroma_minus8 || p->picture_width_in_mbs_minus1 >= 256 || p->picture_height_in_mbs_minus1 >= 256)
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    if (p->pic_fields.bits.field_pic_flag) return VA_STATUS_ERROR_UNIMPLEMENTED;
    if (p->seq_fields.bits.pic_order_cnt_type > 2 || (!p->seq_fields.bits.frame_mbs_only_flag && ((p->picture_height_in_mbs_minus1 + 1) & 1)))
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    for (int i = 0; i < count; i++) if (buffers[i] && buffers[i]->type == VASliceParameterBufferType) {
        struct vav4l2_buffer *buffer = buffers[i];
        unsigned n = buffer->num_elements ? buffer->num_elements : 1;
        if (buffer->size < sizeof(VASliceParameterBufferH264)) return VA_STATUS_ERROR_INVALID_PARAMETER;
        for (unsigned j = 0; j < n; j++) {
            VASliceParameterBufferH264 value; memcpy(&value, (const uint8_t *)buffer->data + (size_t)j * buffer->size, sizeof(value));
            if ((value.slice_data_flag == VA_SLICE_DATA_FLAG_ALL || value.slice_data_flag == VA_SLICE_DATA_FLAG_BEGIN) && value.slice_type % 5 > 2)
                return VA_STATUS_ERROR_UNIMPLEMENTED;
        }
    }
    struct v4l2_ctrl_h264_sps sps; struct v4l2_ctrl_h264_pps pps; struct v4l2_ctrl_h264_decode_params dec = {0};
    h264_fill_sps(&sps, p, ctx->profile); h264_fill_pps(&pps, p, slice);
    int reset = 0;
    VAStatus status = vav4l2_h264_validate_headers_reset(slices, bytes, p, buffers, count, &pps, &dec, &reset);
    if (status != VA_STATUS_SUCCESS) return status;
    if (p->seq_fields.bits.pic_order_cnt_type == 1) {
        /* Refuse ambiguous wraps rather than silently changing display order.
         * MMCO5 changes the original type-1 POC origin and needs its own proof. */
        if (reset) return VA_STATUS_ERROR_UNIMPLEMENTED;
        int idr = !!(dec.flags & V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC);
        if (!idr && !ctx->packet_state.h264_valid) return VA_STATUS_ERROR_DECODING_ERROR;
        unsigned previous = idr ? 0 : ctx->packet_state.h264_lsb;
        int64_t msb = idr ? 0 : ctx->packet_state.h264_msb;
        unsigned lsb = (uint32_t)p->CurrPic.TopFieldOrderCnt & 65535;
        if (lsb < previous && previous - lsb >= 32768) msb += 65536;
        else if (lsb > previous && lsb - previous > 32768) msb -= 65536;
        if (msb + lsb != p->CurrPic.TopFieldOrderCnt || (idr && p->CurrPic.TopFieldOrderCnt)) return VA_STATUS_ERROR_UNIMPLEMENTED;
        if (p->pic_fields.bits.reference_pic_flag) { next->h264_msb = msb; next->h264_lsb = lsb; next->h264_valid = 1; }
    } else if (p->pic_fields.bits.reference_pic_flag) {
        next->h264_valid = 0; /* A later POC1 sequence must start at an IDR. */
    }
    unsigned w = (p->picture_width_in_mbs_minus1 + 1u) * 16, h = (p->picture_height_in_mbs_minus1 + 1u) * 16;
    unsigned crop_y = p->seq_fields.bits.frame_mbs_only_flag ? 2 : 4;
    if (w < (unsigned)ctx->width || h < (unsigned)ctx->height || (w - ctx->width) % 2 || (h - ctx->height) % crop_y)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    unsigned level = sps.level_idc;
    if (!level) return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    struct bits b = {0};
    put(&b, 8, sps.profile_idc); put(&b, 8, ctx->profile == VAProfileH264ConstrainedBaseline ? 0xc0 : 0); put(&b, 8, level); ue(&b, 0);
    if (sps.profile_idc >= 100) { ue(&b, 1); ue(&b, 0); ue(&b, 0); put(&b, 1, !!(sps.flags & V4L2_H264_SPS_FLAG_QPPRIME_Y_ZERO_TRANSFORM_BYPASS)); put(&b, 1, 0); }
    int normalize = sps.pic_order_cnt_type == 1;
    ue(&b, sps.log2_max_frame_num_minus4); ue(&b, normalize ? 0 : sps.pic_order_cnt_type);
    if (normalize || !sps.pic_order_cnt_type) ue(&b, normalize ? 12 : sps.log2_max_pic_order_cnt_lsb_minus4);
    ue(&b, p->num_ref_frames); put(&b, 1, !!(sps.flags & V4L2_H264_SPS_FLAG_GAPS_IN_FRAME_NUM_VALUE_ALLOWED));
    ue(&b, sps.pic_width_in_mbs_minus1); ue(&b, sps.pic_height_in_map_units_minus1);
    put(&b, 1, p->seq_fields.bits.frame_mbs_only_flag);
    if (!p->seq_fields.bits.frame_mbs_only_flag) put(&b, 1, p->seq_fields.bits.mb_adaptive_frame_field_flag);
    put(&b, 1, p->seq_fields.bits.direct_8x8_inference_flag); put(&b, 1, w != (unsigned)ctx->width || h != (unsigned)ctx->height);
    if (w != (unsigned)ctx->width || h != (unsigned)ctx->height) { ue(&b, 0); ue(&b, (w - ctx->width) / 2); ue(&b, 0); ue(&b, (h - ctx->height) / crop_y); }
    put(&b, 1, 0); /* VUI does not alter reconstruction; display geometry is explicit. */
    if (nal(&b, 7, 0, dst, cap, used) < 0) return VA_STATUS_ERROR_ALLOCATION_FAILED;
    b = (struct bits){0};
    ue(&b, pps.pic_parameter_set_id); ue(&b, 0);
    put(&b, 1, p->pic_fields.bits.entropy_coding_mode_flag); put(&b, 1, normalize || p->pic_fields.bits.pic_order_present_flag); ue(&b, 0);
    ue(&b, pps.num_ref_idx_l0_default_active_minus1); ue(&b, pps.num_ref_idx_l1_default_active_minus1);
    put(&b, 1, p->pic_fields.bits.weighted_pred_flag); put(&b, 2, p->pic_fields.bits.weighted_bipred_idc);
    se(&b, pps.pic_init_qp_minus26); se(&b, pps.pic_init_qs_minus26); se(&b, pps.chroma_qp_index_offset);
    put(&b, 1, p->pic_fields.bits.deblocking_filter_control_present_flag); put(&b, 1, p->pic_fields.bits.constrained_intra_pred_flag);
    put(&b, 1, p->pic_fields.bits.redundant_pic_cnt_present_flag);
    put(&b, 1, p->pic_fields.bits.transform_8x8_mode_flag); put(&b, 1, 1);
    for (unsigned i = 0; i < 6; i++) { put(&b, 1, 1); h264_scaling(&b, iq ? iq->ScalingList4x4[i] : NULL, 4); }
    if (p->pic_fields.bits.transform_8x8_mode_flag) for (unsigned i = 0; i < 2; i++) { put(&b, 1, 1); h264_scaling(&b, iq ? iq->ScalingList8x8[i] : NULL, 8); }
    se(&b, pps.second_chroma_qp_index_offset);
    return nal(&b, 8, 0, dst, cap, used) < 0 ? VA_STATUS_ERROR_ALLOCATION_FAILED : VA_STATUS_SUCCESS;
}
#pragma GCC diagnostic pop
/* VA supplies resolved POCs, but omits type-1 SPS offset cycles. Express
 * those POCs as type 0 and retain the entropy-coded macroblock data. */
static VAStatus rewrite_poc(const uint8_t *unit, size_t length, const VAPictureParameterBufferH264 *p,
    const VASliceParameterBufferH264 *slice, uint8_t *scratch, size_t cap, uint8_t *dst, size_t *used)
{
    unsigned prefix = unit[2] == 1 ? 3 : 4;
    if (length <= prefix + 1) return VA_STATUS_ERROR_INVALID_PARAMETER;
    struct rbsp_reader r;
    if (rbsp_open(&r, unit + prefix + 1, length - prefix - 1) < 0) return VA_STATUS_ERROR_ALLOCATION_FAILED;
    struct rbsp_writer w = {.data = scratch, .capacity = cap};
    rbsp_ue(&r); rbsp_ue(&r); rbsp_ue(&r);
    rbsp_get(&r, p->seq_fields.bits.log2_max_frame_num_minus4 + 4);
    if (!p->seq_fields.bits.frame_mbs_only_flag && rbsp_get(&r, 1)) { r.error = 1; }
    if ((unit[prefix] & 31) == 5) rbsp_ue(&r);
    size_t start = r.bit;
    if (!p->seq_fields.bits.delta_pic_order_always_zero_flag) {
        rbsp_ue(&r); if (p->pic_fields.bits.pic_order_present_flag) rbsp_ue(&r);
    }
    size_t end = r.bit; r.bit = 0; rbsp_copy(&w, &r, start);
    rbsp_put(&w, 16, (uint32_t)p->CurrPic.TopFieldOrderCnt & 65535);
    rbsp_write_se(&w, (int64_t)p->CurrPic.BottomFieldOrderCnt - p->CurrPic.TopFieldOrderCnt);
    r.bit = end;
    if (p->pic_fields.bits.entropy_coding_mode_flag) {
        if (slice->slice_data_bit_offset < 8) r.error = 1;
        size_t header = slice->slice_data_bit_offset >= 8 ? slice->slice_data_bit_offset - 8 : 0;
        rbsp_copy(&w, &r, header);
        while (w.bit % 8 && !w.error) rbsp_put(&w, 1, 1);
        r.bit = (header + 7) & ~(size_t)7;
        rbsp_copy(&w, &r, r.size * 8);
    } else {
        size_t stop = r.size * 8;
        do {
            if (stop <= end) { r.error = 1; break; }
            r.bit = --stop;
        } while (!rbsp_get(&r, 1) && !r.error);
        r.bit = end; rbsp_copy(&w, &r, stop); rbsp_align(&w);
    }
    VAStatus status = r.error || w.error ? VA_STATUS_ERROR_INVALID_PARAMETER :
        rbsp_nal(&w, unit + prefix, 1, dst, cap, used) < 0 ? VA_STATUS_ERROR_ALLOCATION_FAILED : VA_STATUS_SUCCESS;
    free(r.data); return status;
}
VAStatus vav4l2_h264_rewrite(const struct vav4l2_collected *cb, struct vav4l2_buffer **buffers, int count,
    const uint8_t *data, size_t bytes, uint8_t *dst, size_t cap, size_t *used)
{
    const VAPictureParameterBufferH264 *p = cb->pic;
    if (!p->seq_fields.bits.pic_order_cnt_type || p->seq_fields.bits.pic_order_cnt_type == 2) {
        if (bytes > cap) return VA_STATUS_ERROR_ALLOCATION_FAILED;
        memcpy(dst, data, bytes); *used = bytes; return VA_STATUS_SUCCESS;
    }
    uint8_t *scratch = malloc(cap ? cap : 1); if (!scratch) return VA_STATUS_ERROR_ALLOCATION_FAILED;
    size_t cursor = 0, length; const uint8_t *unit; VAStatus status = VA_STATUS_SUCCESS;
    *used = 0;
    for (int i = 0; i < count && status == VA_STATUS_SUCCESS; i++) {
        struct vav4l2_buffer *b = buffers[i];
        if (!b || b->type != VASliceParameterBufferType) continue;
        unsigned n = b->num_elements ? b->num_elements : 1;
        for (unsigned j = 0; j < n; j++) {
            VASliceParameterBufferH264 slice; memcpy(&slice, (uint8_t *)b->data + (size_t)j * b->size, sizeof(slice));
            if (slice.slice_data_flag != VA_SLICE_DATA_FLAG_ALL && slice.slice_data_flag != VA_SLICE_DATA_FLAG_BEGIN) continue;
            if (vav4l2_annexb_next(data, bytes, &cursor, &unit, &length) != 1) { status = VA_STATUS_ERROR_INVALID_PARAMETER; break; }
            status = rewrite_poc(unit, length, p, &slice, scratch, cap, dst, used);
            if (status != VA_STATUS_SUCCESS) break;
        }
    }
    if (status == VA_STATUS_SUCCESS && vav4l2_annexb_next(data, bytes, &cursor, &unit, &length) != 0) status = VA_STATUS_ERROR_INVALID_PARAMETER;
    free(scratch); return status;
}
