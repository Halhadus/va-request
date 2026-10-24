/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Reconstruct parameter sets from public VA syntax; retain coded slice bytes. */
#include <stdlib.h>
#include <string.h>
#include "va_v4l2.h"

struct bits { uint8_t data[8192]; size_t bit; int error; };
static void put(struct bits *b, unsigned n, uint32_t v)
{
    if (n > 32 || b->bit + n > sizeof(b->data) * 8) { b->error = 1; return; }
    for (unsigned i = n; i; i--, b->bit++) b->data[b->bit / 8] |= ((v >> (i - 1)) & 1) << (7 - b->bit % 8);
}
static void ue(struct bits *b, uint32_t v)
{
    if (v == UINT32_MAX) { b->error = 1; return; }
    unsigned n = 0; uint32_t code = v + 1;
    for (uint32_t x = code; x >>= 1;) n++;
    put(b, n, 0); put(b, n + 1, code);
}
static void se(struct bits *b, int v) { ue(b, v > 0 ? 2u * (unsigned)v - 1 : 2u * (unsigned)(-(int64_t)v)); }
static int nal(struct bits *b, unsigned type, int hevc, uint8_t *dst, size_t cap, size_t *used)
{
    put(b, 1, 1); while (b->bit % 8) put(b, 1, 0);
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
static unsigned scan(unsigned index, unsigned side, int diagonal)
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
static VAStatus h264_headers(struct vav4l2_context *ctx, const struct vav4l2_collected *cb, struct vav4l2_buffer **buffers,
    int count, const uint8_t *slices, size_t bytes, uint8_t *dst, size_t cap, size_t *used)
{
    const VAPictureParameterBufferH264 *p = cb->pic;
    const VASliceParameterBufferH264 *slice = cb->n_slice_params ? cb->slice_params[0] : NULL;
    const VAIQMatrixBufferH264 *iq = cb->iq;
    if (!p || !slice || p->seq_fields.bits.log2_max_frame_num_minus4 > 12 || p->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 > 12 ||
        p->num_ref_frames > 16 || p->num_slice_groups_minus1 || p->seq_fields.bits.chroma_format_idc != 1 ||
        p->bit_depth_luma_minus8 || p->bit_depth_chroma_minus8 || p->picture_width_in_mbs_minus1 >= 256 || p->picture_height_in_mbs_minus1 >= 256)
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    if (p->seq_fields.bits.pic_order_cnt_type == 1 || p->pic_fields.bits.field_pic_flag) return VA_STATUS_ERROR_UNIMPLEMENTED;
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
    VAStatus status = vav4l2_h264_validate_headers(slices, bytes, p, buffers, count, &pps, &dec);
    if (status != VA_STATUS_SUCCESS) return status;
    unsigned w = (p->picture_width_in_mbs_minus1 + 1u) * 16, h = (p->picture_height_in_mbs_minus1 + 1u) * 16;
    unsigned crop_y = p->seq_fields.bits.frame_mbs_only_flag ? 2 : 4;
    if (w < (unsigned)ctx->width || h < (unsigned)ctx->height || (w - ctx->width) % 2 || (h - ctx->height) % crop_y)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    const unsigned levels[][3] = {{30,1620,8100},{31,3600,18000},{32,5120,20480},{40,8192,32768},{42,8704,34816},{50,22080,110400},{51,36864,184320}};
    unsigned area = w / 16 * (h / 16), refs = p->num_ref_frames ? p->num_ref_frames : 1, level = 0;
    for (unsigned i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) if (area <= levels[i][1] && (uint64_t)(w / 16) * (w / 16) <= 8u * levels[i][1] &&
        (uint64_t)(h / 16) * (h / 16) <= 8u * levels[i][1] && (uint64_t)area * refs <= levels[i][2]) { level = levels[i][0]; break; }
    if (!level) return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    struct bits b = {0};
    put(&b, 8, sps.profile_idc); put(&b, 8, ctx->profile == VAProfileH264ConstrainedBaseline ? 0xc0 : 0); put(&b, 8, level); ue(&b, 0);
    if (sps.profile_idc >= 100) { ue(&b, 1); ue(&b, 0); ue(&b, 0); put(&b, 1, !!(sps.flags & V4L2_H264_SPS_FLAG_QPPRIME_Y_ZERO_TRANSFORM_BYPASS)); put(&b, 1, 0); }
    ue(&b, sps.log2_max_frame_num_minus4); ue(&b, sps.pic_order_cnt_type);
    if (!sps.pic_order_cnt_type) ue(&b, sps.log2_max_pic_order_cnt_lsb_minus4);
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
    put(&b, 1, p->pic_fields.bits.entropy_coding_mode_flag); put(&b, 1, p->pic_fields.bits.pic_order_present_flag); ue(&b, 0);
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
static void hevc_ptl(struct bits *b, unsigned profile, unsigned level)
{
    put(b, 2, 0); put(b, 1, 0); put(b, 5, profile); put(b, 32, 1u << (31 - profile));
    put(b, 4, 9); put(b, 32, 0); put(b, 12, 0); put(b, 8, level);
}
static void hevc_scaling(struct bits *b, const VAIQMatrixBufferHEVC *iq)
{
    for (unsigned size = 0; size < 4; size++) for (unsigned id = 0; id < 6; id += size == 3 ? 3 : 1) {
        const uint8_t *m = size == 0 ? iq->ScalingList4x4[id] : size == 1 ? iq->ScalingList8x8[id] :
            size == 2 ? iq->ScalingList16x16[id] : iq->ScalingList32x32[id / 3];
        int last = 8; put(b, 1, 1);
        if (size > 1) { last = size == 2 ? iq->ScalingListDC16x16[id] : iq->ScalingListDC32x32[id / 3]; se(b, last - 8); }
        unsigned side = size ? 8 : 4;
        for (unsigned j = 0; j < side * side; j++) { int v = m[scan(j, side, 1)], delta = v - last; if (!v) b->error = 1; if (delta > 127) delta -= 256; if (delta < -128) delta += 256; se(b, delta); last = v; }
    }
}
static VAStatus hevc_headers(struct vav4l2_context *ctx, const struct vav4l2_collected *cb, const uint8_t *slices, size_t bytes,
    uint8_t *dst, size_t cap, size_t *used)
{
    const VAPictureParameterBufferHEVC *p = cb->pic; const VAIQMatrixBufferHEVC *iq = cb->iq;
    if (!p || p->pic_fields.bits.chroma_format_idc != 1 || p->pic_fields.bits.separate_colour_plane_flag ||
        p->bit_depth_luma_minus8 != p->bit_depth_chroma_minus8 || (p->bit_depth_luma_minus8 != 0 && p->bit_depth_luma_minus8 != 2) ||
        p->sps_max_dec_pic_buffering_minus1 > 15 || p->log2_max_pic_order_cnt_lsb_minus4 > 12 ||
        !p->pic_width_in_luma_samples || !p->pic_height_in_luma_samples || p->pic_width_in_luma_samples > 4096 || p->pic_height_in_luma_samples > 4096)
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    /* These original SPS arrays are absent from the public VA decode ABI. */
    if (p->num_short_term_ref_pic_sets || p->num_long_term_ref_pic_sps || p->slice_parsing_fields.bits.output_flag_present_flag)
        return VA_STATUS_ERROR_UNIMPLEMENTED;
    unsigned log2_cb = p->log2_min_luma_coding_block_size_minus3 + 3u + p->log2_diff_max_min_luma_coding_block_size;
    unsigned log2_tb = p->log2_min_transform_block_size_minus2 + 2u + p->log2_diff_max_min_transform_block_size;
    if (log2_cb < 4 || log2_cb > 6 || log2_tb > 5 || log2_tb > log2_cb || p->num_extra_slice_header_bits > 7 ||
        p->num_ref_idx_l0_default_active_minus1 > 14 || p->num_ref_idx_l1_default_active_minus1 > 14 ||
        p->log2_parallel_merge_level_minus2 + 2u > log2_cb) return VA_STATUS_ERROR_INVALID_PARAMETER;
    struct v4l2_ctrl_hevc_pps q; struct v4l2_ctrl_hevc_decode_params dec = {0};
    VAStatus status = vav4l2_hevc_fill_pps(&q, p);
    if (status == VA_STATUS_SUCCESS) status = vav4l2_hevc_parse_headers(slices, bytes, p, &q, &dec);
    if (status != VA_STATUS_SUCCESS) return status;
    size_t offset = 0, length; const uint8_t *unit;
    while (vav4l2_annexb_next(slices, bytes, &offset, &unit, &length) == 1) {
        unsigned prefix = unit[2] == 1 ? 3 : 4;
        if (length < prefix + 2 || (unit[prefix] & 1) || unit[prefix + 1] != 1) return VA_STATUS_ERROR_UNIMPLEMENTED;
    }
    unsigned w = p->pic_width_in_luma_samples, h = p->pic_height_in_luma_samples;
    if (w < (unsigned)ctx->width || h < (unsigned)ctx->height || (w - ctx->width) % 2 || (h - ctx->height) % 2)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    unsigned profile = p->bit_depth_luma_minus8 ? 2 : 1, level = (uint64_t)w * h <= 552960 ? 93 : (uint64_t)w * h <= 2228224 ? 123 : 153;
    unsigned reorder = p->pic_fields.bits.NoPicReorderingFlag ? 0 : p->sps_max_dec_pic_buffering_minus1;
    struct bits b = {0};
    put(&b, 4, 0); put(&b, 1, 1); put(&b, 1, 1); put(&b, 6, 0); put(&b, 3, 0); put(&b, 1, 1); put(&b, 16, 0xffff);
    hevc_ptl(&b, profile, level); put(&b, 1, 0); ue(&b, p->sps_max_dec_pic_buffering_minus1); ue(&b, reorder); ue(&b, 0);
    put(&b, 6, 0); ue(&b, 0); put(&b, 1, 0); put(&b, 1, 0);
    if (nal(&b, 32, 1, dst, cap, used) < 0) return VA_STATUS_ERROR_ALLOCATION_FAILED;
    b = (struct bits){0}; put(&b, 4, 0); put(&b, 3, 0); put(&b, 1, 1); hevc_ptl(&b, profile, level);
    ue(&b, 0); ue(&b, 1); ue(&b, w); ue(&b, h); put(&b, 1, w != (unsigned)ctx->width || h != (unsigned)ctx->height);
    if (w != (unsigned)ctx->width || h != (unsigned)ctx->height) { ue(&b, 0); ue(&b, (w - ctx->width) / 2); ue(&b, 0); ue(&b, (h - ctx->height) / 2); }
    ue(&b, p->bit_depth_luma_minus8); ue(&b, p->bit_depth_chroma_minus8); ue(&b, p->log2_max_pic_order_cnt_lsb_minus4);
    put(&b, 1, 0); ue(&b, p->sps_max_dec_pic_buffering_minus1); ue(&b, reorder); ue(&b, 0);
    ue(&b, p->log2_min_luma_coding_block_size_minus3); ue(&b, p->log2_diff_max_min_luma_coding_block_size);
    ue(&b, p->log2_min_transform_block_size_minus2); ue(&b, p->log2_diff_max_min_transform_block_size);
    ue(&b, p->max_transform_hierarchy_depth_inter); ue(&b, p->max_transform_hierarchy_depth_intra);
    put(&b, 1, p->pic_fields.bits.scaling_list_enabled_flag);
    if (p->pic_fields.bits.scaling_list_enabled_flag) { put(&b, 1, !!iq); if (iq) hevc_scaling(&b, iq); }
    put(&b, 1, p->pic_fields.bits.amp_enabled_flag); put(&b, 1, p->slice_parsing_fields.bits.sample_adaptive_offset_enabled_flag);
    put(&b, 1, p->pic_fields.bits.pcm_enabled_flag);
    if (p->pic_fields.bits.pcm_enabled_flag) {
        put(&b, 4, p->pcm_sample_bit_depth_luma_minus1); put(&b, 4, p->pcm_sample_bit_depth_chroma_minus1);
        ue(&b, p->log2_min_pcm_luma_coding_block_size_minus3); ue(&b, p->log2_diff_max_min_pcm_luma_coding_block_size);
        put(&b, 1, p->pic_fields.bits.pcm_loop_filter_disabled_flag);
    }
    ue(&b, 0); put(&b, 1, p->slice_parsing_fields.bits.long_term_ref_pics_present_flag);
    if (p->slice_parsing_fields.bits.long_term_ref_pics_present_flag) ue(&b, 0);
    put(&b, 1, p->slice_parsing_fields.bits.sps_temporal_mvp_enabled_flag); put(&b, 1, p->pic_fields.bits.strong_intra_smoothing_enabled_flag);
    put(&b, 1, 0); put(&b, 1, 0);
    if (nal(&b, 33, 1, dst, cap, used) < 0) return VA_STATUS_ERROR_ALLOCATION_FAILED;
    b = (struct bits){0}; ue(&b, q.pic_parameter_set_id); ue(&b, 0);
    put(&b, 1, p->slice_parsing_fields.bits.dependent_slice_segments_enabled_flag); put(&b, 1, p->slice_parsing_fields.bits.output_flag_present_flag);
    put(&b, 3, p->num_extra_slice_header_bits); put(&b, 1, p->pic_fields.bits.sign_data_hiding_enabled_flag); put(&b, 1, p->slice_parsing_fields.bits.cabac_init_present_flag);
    ue(&b, p->num_ref_idx_l0_default_active_minus1); ue(&b, p->num_ref_idx_l1_default_active_minus1); se(&b, p->init_qp_minus26);
    put(&b, 1, p->pic_fields.bits.constrained_intra_pred_flag); put(&b, 1, p->pic_fields.bits.transform_skip_enabled_flag); put(&b, 1, p->pic_fields.bits.cu_qp_delta_enabled_flag);
    if (p->pic_fields.bits.cu_qp_delta_enabled_flag) ue(&b, p->diff_cu_qp_delta_depth);
    se(&b, p->pps_cb_qp_offset); se(&b, p->pps_cr_qp_offset); put(&b, 1, p->slice_parsing_fields.bits.pps_slice_chroma_qp_offsets_present_flag);
    put(&b, 1, p->pic_fields.bits.weighted_pred_flag); put(&b, 1, p->pic_fields.bits.weighted_bipred_flag); put(&b, 1, p->pic_fields.bits.transquant_bypass_enabled_flag);
    put(&b, 1, p->pic_fields.bits.tiles_enabled_flag); put(&b, 1, p->pic_fields.bits.entropy_coding_sync_enabled_flag);
    if (p->pic_fields.bits.tiles_enabled_flag) {
        ue(&b, q.num_tile_columns_minus1); ue(&b, q.num_tile_rows_minus1); int uniform = !!(q.flags & V4L2_HEVC_PPS_FLAG_UNIFORM_SPACING); put(&b, 1, uniform);
        if (!uniform) { for (unsigned i = 0; i < q.num_tile_columns_minus1; i++) ue(&b, p->column_width_minus1[i]); for (unsigned i = 0; i < q.num_tile_rows_minus1; i++) ue(&b, p->row_height_minus1[i]); }
        put(&b, 1, p->pic_fields.bits.loop_filter_across_tiles_enabled_flag);
    }
    put(&b, 1, p->pic_fields.bits.pps_loop_filter_across_slices_enabled_flag); put(&b, 1, 1);
    put(&b, 1, p->slice_parsing_fields.bits.deblocking_filter_override_enabled_flag); put(&b, 1, p->slice_parsing_fields.bits.pps_disable_deblocking_filter_flag);
    if (!p->slice_parsing_fields.bits.pps_disable_deblocking_filter_flag) { se(&b, p->pps_beta_offset_div2); se(&b, p->pps_tc_offset_div2); }
    put(&b, 1, 0); put(&b, 1, p->slice_parsing_fields.bits.lists_modification_present_flag); ue(&b, p->log2_parallel_merge_level_minus2);
    put(&b, 1, p->slice_parsing_fields.bits.slice_segment_header_extension_present_flag); put(&b, 1, 0);
    return nal(&b, 34, 1, dst, cap, used) < 0 ? VA_STATUS_ERROR_ALLOCATION_FAILED : VA_STATUS_SUCCESS;
}
VAStatus vav4l2_stateful_packet(struct vav4l2_context *ctx, struct vav4l2_buffer **buffers, int count, uint8_t *dst, size_t capacity, size_t *size)
{
    if (!ctx || !dst || !size) return VA_STATUS_ERROR_INVALID_PARAMETER;
    struct vav4l2_collected cb; vav4l2_collect_decode_buffers(buffers, count, &cb);
    size_t bytes = 0, used = 0;
    VAStatus status = vav4l2_packetize_slices(ctx->codec, buffers, count, NULL, UINT32_MAX, &bytes);
    if (status != VA_STATUS_SUCCESS || !bytes) return status == VA_STATUS_SUCCESS ? VA_STATUS_ERROR_INVALID_PARAMETER : status;
    if (bytes > capacity) return VA_STATUS_ERROR_ALLOCATION_FAILED;
    uint8_t *slices = malloc(bytes); if (!slices) return VA_STATUS_ERROR_ALLOCATION_FAILED;
    status = vav4l2_packetize_slices(ctx->codec, buffers, count, slices, bytes, &bytes);
    uint8_t headers[8192];
    if (status == VA_STATUS_SUCCESS && ctx->codec == VAV4L2_CODEC_H264) status = h264_headers(ctx, &cb, buffers, count, slices, bytes, headers, sizeof(headers), &used);
    else if (status == VA_STATUS_SUCCESS && ctx->codec == VAV4L2_CODEC_HEVC) status = hevc_headers(ctx, &cb, slices, bytes, headers, sizeof(headers), &used);
    else if (status == VA_STATUS_SUCCESS) status = VA_STATUS_ERROR_UNIMPLEMENTED;
    if (status == VA_STATUS_SUCCESS && used > capacity - bytes) status = VA_STATUS_ERROR_ALLOCATION_FAILED;
    if (status == VA_STATUS_SUCCESS) { memcpy(dst, headers, used); memcpy(dst + used, slices, bytes); *size = used + bytes; }
    free(slices); return status;
}
