/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Normalization adapted from Radxa libva-v4l2 (MIT); see NOTICE. */
#include "bytestream.h"
#include "rbsp.h"
static void hevc_ptl(struct bits *b, unsigned profile, unsigned level, unsigned sublayers)
{
    put(b, 2, 0); put(b, 1, 0); put(b, 5, profile); put(b, 32, 1u << (31 - profile));
    put(b, 4, 9); put(b, 32, 0); put(b, 12, 0); put(b, 8, level);
    if (sublayers) put(b, 16, 0); /* Infer the general PTL for each sublayer. */
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
VAStatus vav4l2_hevc_headers(struct vav4l2_context *ctx, const struct vav4l2_collected *cb, const uint8_t *slices, size_t bytes,
    uint8_t *dst, size_t cap, size_t *used, int normalize)
{
    const VAPictureParameterBufferHEVC *p = cb->pic; const VAIQMatrixBufferHEVC *iq = cb->iq;
    if (!p || p->pic_fields.bits.chroma_format_idc != 1 || p->pic_fields.bits.separate_colour_plane_flag ||
        p->bit_depth_luma_minus8 != p->bit_depth_chroma_minus8 || (p->bit_depth_luma_minus8 != 0 && p->bit_depth_luma_minus8 != 2) ||
        p->sps_max_dec_pic_buffering_minus1 > 15 || p->log2_max_pic_order_cnt_lsb_minus4 > 12 ||
        p->num_short_term_ref_pic_sets > 64 || p->num_long_term_ref_pic_sps > 32 ||
        (p->CurrPic.flags & VA_PICTURE_HEVC_FIELD_PIC) ||
        !p->pic_width_in_luma_samples || !p->pic_height_in_luma_samples || p->pic_width_in_luma_samples > 4096 || p->pic_height_in_luma_samples > 4096)
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    /* These original SPS arrays are absent from the public VA decode ABI. */
    if (!normalize && (p->num_short_term_ref_pic_sets || p->num_long_term_ref_pic_sps || p->slice_parsing_fields.bits.output_flag_present_flag))
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
        if (length < prefix + 2 || (unit[prefix] & 1) || (unit[prefix + 1] >> 3) || (!normalize && unit[prefix + 1] != 1)) return VA_STATUS_ERROR_UNIMPLEMENTED;
    }
    unsigned w = p->pic_width_in_luma_samples, h = p->pic_height_in_luma_samples;
    if (w < (unsigned)ctx->width || h < (unsigned)ctx->height || (w - ctx->width) % 2 || (h - ctx->height) % 2)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    unsigned profile = p->bit_depth_luma_minus8 ? 2 : 1, level = 0;
    const unsigned levels[][2] = {{90,552960},{93,983040},{120,2228224},{150,8912896},{180,35651584}};
    uint64_t area = (uint64_t)w * h;
    for (unsigned i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
        uint64_t limit = levels[i][1];
        unsigned dpb = area <= limit / 4 ? 16 : area <= limit / 2 ? 12 : area <= 3 * limit / 4 ? 8 : 6;
        if (area <= limit && (uint64_t)w * w <= 8 * limit && (uint64_t)h * h <= 8 * limit &&
            p->sps_max_dec_pic_buffering_minus1 + 1u <= dpb) { level = levels[i][0]; break; }
    }
    if (!level) return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    unsigned reorder = p->pic_fields.bits.NoPicReorderingFlag ? 0 : p->sps_max_dec_pic_buffering_minus1;
    unsigned sublayers = normalize ? 6 : 0;
    struct bits b = {0};
    put(&b, 4, 0); put(&b, 1, 1); put(&b, 1, 1); put(&b, 6, 0); put(&b, 3, sublayers); put(&b, 1, 1); put(&b, 16, 0xffff);
    hevc_ptl(&b, profile, level, sublayers); put(&b, 1, 0); ue(&b, p->sps_max_dec_pic_buffering_minus1); ue(&b, reorder); ue(&b, 0);
    put(&b, 6, 0); ue(&b, 0); put(&b, 1, 0); put(&b, 1, 0);
    if (nal(&b, 32, 1, dst, cap, used) < 0) return VA_STATUS_ERROR_ALLOCATION_FAILED;
    b = (struct bits){0}; put(&b, 4, 0); put(&b, 3, sublayers); put(&b, 1, 1); hevc_ptl(&b, profile, level, sublayers);
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
    put(&b, 1, 0); put(&b, 1, normalize || p->slice_parsing_fields.bits.lists_modification_present_flag); ue(&b, p->log2_parallel_merge_level_minus2);
    put(&b, 1, p->slice_parsing_fields.bits.slice_segment_header_extension_present_flag); put(&b, 1, 0);
    return nal(&b, 34, 1, dst, cap, used) < 0 ? VA_STATUS_ERROR_ALLOCATION_FAILED : VA_STATUS_SUCCESS;
}
struct hevc_refs {
    unsigned index[3][15], count[3], list[2][15], active;
};
static VAStatus hevc_references(const VAPictureParameterBufferHEVC *p, struct hevc_refs *refs)
{
    memset(refs, 0, sizeof(*refs));
    const unsigned current = VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE | VA_PICTURE_HEVC_RPS_ST_CURR_AFTER | VA_PICTURE_HEVC_RPS_LT_CURR;
    for (unsigned i = 0; i < 15; i++) {
        const VAPictureHEVC *r = &p->ReferenceFrames[i];
        if (r->picture_id == VA_INVALID_SURFACE || (r->flags & VA_PICTURE_HEVC_INVALID)) continue;
        if (r->pic_order_cnt == p->CurrPic.pic_order_cnt || (r->flags & VA_PICTURE_HEVC_FIELD_PIC)) return VA_STATUS_ERROR_INVALID_PARAMETER;
        for (unsigned j = 0; j < i; j++) if (!(p->ReferenceFrames[j].flags & VA_PICTURE_HEVC_INVALID) &&
            p->ReferenceFrames[j].picture_id != VA_INVALID_SURFACE && (p->ReferenceFrames[j].pic_order_cnt == r->pic_order_cnt ||
            p->ReferenceFrames[j].picture_id == r->picture_id)) return VA_STATUS_ERROR_INVALID_PARAMETER;
        unsigned flags = r->flags & current;
        if (flags && (flags & (flags - 1))) return VA_STATUS_ERROR_INVALID_PARAMETER;
        unsigned group = r->flags & (VA_PICTURE_HEVC_LONG_TERM_REFERENCE | VA_PICTURE_HEVC_RPS_LT_CURR) ? 2 :
            r->pic_order_cnt < p->CurrPic.pic_order_cnt ? 0 : 1;
        if ((flags == VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE && group != 0) ||
            (flags == VA_PICTURE_HEVC_RPS_ST_CURR_AFTER && group != 1)) return VA_STATUS_ERROR_INVALID_PARAMETER;
        unsigned at = refs->count[group]++;
        while (at) {
            int previous = p->ReferenceFrames[refs->index[group][at - 1]].pic_order_cnt;
            if (group == 1 ? previous <= r->pic_order_cnt : previous >= r->pic_order_cnt) break;
            refs->index[group][at] = refs->index[group][at - 1]; at--;
        }
        refs->index[group][at] = i;
    }
    if (refs->count[0] + refs->count[1] + refs->count[2] > p->sps_max_dec_pic_buffering_minus1) return VA_STATUS_ERROR_INVALID_PARAMETER;
    for (unsigned l = 0; l < 2; l++) {
        unsigned n = 0;
        for (unsigned pass = 0; pass < 3; pass++) {
            unsigned group = pass < 2 ? pass ^ l : 2;
            for (unsigned j = 0; j < refs->count[group]; j++) {
                unsigned index = refs->index[group][j];
                if (p->ReferenceFrames[index].flags & current) refs->list[l][n++] = index;
            }
        }
        refs->active = n;
    }
    return refs->active <= 8 ? VA_STATUS_SUCCESS : VA_STATUS_ERROR_INVALID_PARAMETER;
}
static void hevc_write_rps(struct rbsp_writer *w, const struct hevc_refs *refs, const VAPictureParameterBufferHEVC *p)
{
    rbsp_put(w, 1, 0); /* SPS contains no reference-set arrays. */
    rbsp_write_ue(w, refs->count[0]); rbsp_write_ue(w, refs->count[1]);
    const unsigned used = VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE | VA_PICTURE_HEVC_RPS_ST_CURR_AFTER | VA_PICTURE_HEVC_RPS_LT_CURR;
    for (unsigned group = 0; group < 2; group++) {
        int64_t previous = p->CurrPic.pic_order_cnt;
        for (unsigned j = 0; j < refs->count[group]; j++) {
            const VAPictureHEVC *r = &p->ReferenceFrames[refs->index[group][j]];
            int64_t delta = group ? (int64_t)r->pic_order_cnt - previous : previous - r->pic_order_cnt;
            if (delta <= 0 || delta > 32768) { w->error = 1; return; }
            rbsp_write_ue(w, delta - 1); rbsp_put(w, 1, !!(r->flags & used)); previous = r->pic_order_cnt;
        }
    }
    if (p->slice_parsing_fields.bits.long_term_ref_pics_present_flag) {
        rbsp_write_ue(w, refs->count[2]);
        unsigned n = p->log2_max_pic_order_cnt_lsb_minus4 + 4, mask = (1u << n) - 1;
        int64_t msb = (int64_t)p->CurrPic.pic_order_cnt - ((uint32_t)p->CurrPic.pic_order_cnt & mask), previous = 0;
        for (unsigned j = 0; j < refs->count[2]; j++) {
            const VAPictureHEVC *r = &p->ReferenceFrames[refs->index[2][j]];
            unsigned lsb = (uint32_t)r->pic_order_cnt & mask;
            int64_t cycle = (msb - ((int64_t)r->pic_order_cnt - lsb)) / (1u << n);
            if (cycle < previous || cycle - previous >= UINT32_MAX) { w->error = 1; return; }
            rbsp_put(w, n, lsb); rbsp_put(w, 1, !!(r->flags & used)); rbsp_put(w, 1, 1);
            rbsp_write_ue(w, cycle - previous); previous = cycle;
        }
    } else if (refs->count[2]) w->error = 1;
}
static void hevc_copy_header(struct rbsp_writer *w, struct rbsp_reader *r, size_t *copied)
{
    size_t end = r->bit; r->bit = *copied; rbsp_copy(w, r, end); *copied = end;
}
static VAStatus hevc_rewrite_slice(const uint8_t *unit, size_t length, const VAPictureParameterBufferHEVC *p,
    const VASliceParameterBufferHEVC *slice, const struct hevc_refs *refs, uint8_t *scratch, size_t cap, uint8_t *dst, size_t *used)
{
    unsigned prefix = unit[2] == 1 ? 3 : 4;
    if (length <= prefix + 2 || slice->slice_data_byte_offset < 3) return VA_STATUS_ERROR_INVALID_PARAMETER;
    const uint8_t *header = unit + prefix; unsigned type = header[0] >> 1 & 63;
    struct rbsp_reader r; if (rbsp_open(&r, header + 2, length - prefix - 2) < 0) return VA_STATUS_ERROR_ALLOCATION_FAILED;
    struct rbsp_writer w = {.data = scratch, .capacity = cap};
    unsigned first = rbsp_get(&r, 1); rbsp_put(&w, 1, first);
    if (type >= 16 && type <= 23) { rbsp_get(&r, 1); rbsp_put(&w, 1, 0); }
    size_t copied = r.bit; unsigned id = rbsp_ue(&r), dependent = 0;
    if (id > 63) r.error = 1;
    if (!first) {
        if (p->slice_parsing_fields.bits.dependent_slice_segments_enabled_flag) dependent = rbsp_get(&r, 1);
        unsigned ctb = 1u << (p->log2_min_luma_coding_block_size_minus3 + 3 + p->log2_diff_max_min_luma_coding_block_size);
        unsigned count = ((p->pic_width_in_luma_samples + ctb - 1) / ctb) * ((p->pic_height_in_luma_samples + ctb - 1) / ctb);
        if (rbsp_get(&r, rbsp_ceil_log2(count)) != slice->slice_segment_address) r.error = 1;
    }
    if (dependent != slice->LongSliceFlags.fields.dependent_slice_segment_flag) r.error = 1;
    if (!dependent) {
        rbsp_skip(&r, p->num_extra_slice_header_bits);
        unsigned st = rbsp_ue(&r);
        if (st > 2 || st != slice->LongSliceFlags.fields.slice_type) r.error = 1;
        if (p->slice_parsing_fields.bits.output_flag_present_flag) {
            hevc_copy_header(&w, &r, &copied); rbsp_get(&r, 1); rbsp_put(&w, 1, 1); copied = r.bit;
        }
        if (type != 19 && type != 20) {
            rbsp_get(&r, p->log2_max_pic_order_cnt_lsb_minus4 + 4);
            hevc_copy_header(&w, &r, &copied);
            if (rbsp_get(&r, 1)) {
                if (!p->num_short_term_ref_pic_sets) r.error = 1;
                if (rbsp_get(&r, rbsp_ceil_log2(p->num_short_term_ref_pic_sets)) >= p->num_short_term_ref_pic_sets) r.error = 1;
            } else {
                size_t start = r.bit;
                if (p->num_short_term_ref_pic_sets && rbsp_get(&r, 1)) {
                    if (!p->st_rps_bits) r.error = 1;
                    r.bit = start; rbsp_skip(&r, p->st_rps_bits);
                } else {
                    unsigned neg = rbsp_ue(&r), pos = rbsp_ue(&r);
                    if (neg > 15 || pos > 15 || neg + pos > 15) r.error = 1;
                    for (unsigned i = 0; i < neg + pos && !r.error; i++) { rbsp_ue(&r); rbsp_get(&r, 1); }
                }
            }
            if (p->slice_parsing_fields.bits.long_term_ref_pics_present_flag) {
                unsigned ns = p->num_long_term_ref_pic_sps ? rbsp_ue(&r) : 0, np = rbsp_ue(&r);
                if (ns > p->num_long_term_ref_pic_sps || ns > 15 || np > 15 || ns + np > 15) r.error = 1;
                for (unsigned i = 0; i < ns + np && !r.error; i++) {
                    if (i < ns) { if (rbsp_get(&r, rbsp_ceil_log2(p->num_long_term_ref_pic_sps)) >= p->num_long_term_ref_pic_sps) r.error = 1; }
                    else { rbsp_get(&r, p->log2_max_pic_order_cnt_lsb_minus4 + 4); rbsp_get(&r, 1); }
                    if (rbsp_get(&r, 1)) rbsp_ue(&r);
                }
            }
            copied = r.bit; hevc_write_rps(&w, refs, p);
            if (p->slice_parsing_fields.bits.sps_temporal_mvp_enabled_flag) rbsp_get(&r, 1);
        }
        if (p->slice_parsing_fields.bits.sample_adaptive_offset_enabled_flag) { rbsp_get(&r, 1); rbsp_get(&r, 1); }
        if (st != 2) {
            unsigned counts[2] = {p->num_ref_idx_l0_default_active_minus1 + 1u, p->num_ref_idx_l1_default_active_minus1 + 1u};
            if (rbsp_get(&r, 1)) { counts[0] = rbsp_ue(&r) + 1; if (!st) counts[1] = rbsp_ue(&r) + 1; }
            hevc_copy_header(&w, &r, &copied);
            if (!refs->active) r.error = 1;
            for (unsigned l = 0; l < (st ? 1u : 2u); l++) {
                if (!counts[l] || counts[l] > 15 || counts[l] != (unsigned)(l ? slice->num_ref_idx_l1_active_minus1 : slice->num_ref_idx_l0_active_minus1) + 1) { r.error = 1; break; }
                if (p->slice_parsing_fields.bits.lists_modification_present_flag && refs->active > 1 && rbsp_get(&r, 1))
                    rbsp_skip(&r, counts[l] * rbsp_ceil_log2(refs->active));
                if (refs->active > 1) rbsp_put(&w, 1, 1);
                for (unsigned j = 0; j < counts[l]; j++) {
                    unsigned k = 0; while (k < refs->active && refs->list[l][k] != slice->RefPicList[l][j]) k++;
                    if (k == refs->active) r.error = 1;
                    if (refs->active > 1) rbsp_put(&w, rbsp_ceil_log2(refs->active), k);
                }
            }
            copied = r.bit;
        }
    }
    size_t data = ((size_t)slice->slice_data_byte_offset - 2) * 8, alignment = data, parsed = r.bit;
    if (data > r.size * 8 || data <= parsed) r.error = 1;
    if (!r.error) do {
        if (alignment <= parsed || data - alignment >= 8) { r.error = 1; break; }
        r.bit = --alignment;
    } while (!rbsp_get(&r, 1) && !r.error);
    r.bit = copied; rbsp_copy(&w, &r, alignment); rbsp_align(&w);
    r.bit = data; rbsp_copy(&w, &r, r.size * 8);
    VAStatus status = r.error || w.error ? VA_STATUS_ERROR_INVALID_PARAMETER :
        rbsp_nal(&w, header, 2, dst, cap, used) < 0 ? VA_STATUS_ERROR_ALLOCATION_FAILED : VA_STATUS_SUCCESS;
    free(r.data); return status;
}
VAStatus vav4l2_hevc_rewrite(const struct vav4l2_collected *cb, struct vav4l2_buffer **buffers, int count,
    const uint8_t *data, size_t bytes, uint8_t *dst, size_t cap, size_t *used)
{
    const VAPictureParameterBufferHEVC *p = cb->pic;
    struct hevc_refs refs; VAStatus status = hevc_references(p, &refs);
    if (status != VA_STATUS_SUCCESS) return status;
    uint8_t *scratch = malloc(cap ? cap : 1); if (!scratch) return VA_STATUS_ERROR_ALLOCATION_FAILED;
    size_t cursor = 0, length; const uint8_t *unit; *used = 0;
    for (int i = 0; i < count && status == VA_STATUS_SUCCESS; i++) {
        struct vav4l2_buffer *b = buffers[i];
        if (!b || b->type != VASliceParameterBufferType) continue;
        if (b->size < sizeof(VASliceParameterBufferHEVC)) { status = VA_STATUS_ERROR_INVALID_PARAMETER; break; }
        unsigned n = b->num_elements ? b->num_elements : 1;
        for (unsigned j = 0; j < n; j++) {
            VASliceParameterBufferHEVC slice; memcpy(&slice, (uint8_t *)b->data + (size_t)j * b->size, sizeof(slice));
            if (slice.slice_data_flag != VA_SLICE_DATA_FLAG_ALL && slice.slice_data_flag != VA_SLICE_DATA_FLAG_BEGIN) continue;
            if (vav4l2_annexb_next(data, bytes, &cursor, &unit, &length) != 1) { status = VA_STATUS_ERROR_INVALID_PARAMETER; break; }
            status = hevc_rewrite_slice(unit, length, p, &slice, &refs, scratch, cap, dst, used);
            if (status != VA_STATUS_SUCCESS) break;
        }
    }
    if (status == VA_STATUS_SUCCESS && vav4l2_annexb_next(data, bytes, &cursor, &unit, &length) != 0) status = VA_STATUS_ERROR_INVALID_PARAMETER;
    free(scratch); return status;
}
