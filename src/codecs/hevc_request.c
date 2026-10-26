/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Slice parsing and controls adapted from Omacom libva-v4l2_request.
 * Copyright (C) 2026 Ondrej Jirman <megi@xff.cz> */
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "request_slices.h"
#include "request_bits.h"
#define HEVC_NAL_IDR_W_RADL 19
#define HEVC_NAL_IDR_N_LP 20
#define HEVC_NAL_BLA_W_LP 16
#define HEVC_NAL_RSV_IRAP_VCL23 23
struct hevc_parse {
    VAPictureParameterBufferHEVC va_pic;
    unsigned num_pic_total_curr, max_entry_point_offsets, num_entry_point_offsets;
    uint32_t *entry_point_offsets;
};
static unsigned int ceil_log2(unsigned int value)
{
    unsigned int bits = 0;

    for (value = value ? value - 1 : 0; value; value >>= 1)
        bits++;

    return bits;
}

/* 7.3.6.2 Reference picture list modification syntax */
static void ref_pic_lists_modification(struct hevc_parse *codec,
            struct vav4l2_bits *b, uint32_t num_ref_idx_l0_active_minus1,
            uint32_t num_ref_idx_l1_active_minus1, bool slice_type_b)
{
    /* list_entry_lX[i] is u(v) of Ceil(Log2(NumPicTotalCurr)) bits */
    unsigned int entry_bits = ceil_log2(codec->num_pic_total_curr);

    /* 7.4.7.1: at most 15 active references; anything else is corrupt */
    if (num_ref_idx_l0_active_minus1 > 14 || num_ref_idx_l1_active_minus1 > 14) {
        b->error = true;
        return;
    }

    if (vav4l2_bits_bit(b)) {   /* ref_pic_list_modification_flag_l0 */
        for (uint32_t i = 0; i <= num_ref_idx_l0_active_minus1; i++)
            vav4l2_bits_read(b, entry_bits);    /* list_entry_l0[i] */
    }
    /* flag_l1 is present in B slices whether or not flag_l0 was set */
    if (slice_type_b && vav4l2_bits_bit(b)) {   /* ref_pic_list_modification_flag_l1 */
        for (uint32_t i = 0; i <= num_ref_idx_l1_active_minus1; i++)
            vav4l2_bits_read(b, entry_bits);    /* list_entry_l1[i] */
    }
}

/* generalised 7.3.6.3 Weighted prediction parameters syntax */
static void pred_weight_table(struct vav4l2_bits *b,
            uint32_t num_ref_idx_active_minus1,
            uint32_t chroma_array_type)
{
    uint8_t luma_weight_flag[15];
    uint8_t chroma_weight_flag[15];

    /* 7.4.7.1: at most 15 active references; the value comes from the
     * bitstream and must not size or index anything past that */
    if (num_ref_idx_active_minus1 > 14) {
        b->error = true;
        return;
    }

    for (uint32_t i = 0; i <= num_ref_idx_active_minus1; i++)
        luma_weight_flag[i] = vav4l2_bits_bit(b);   /* luma_weight_lX_flag[i] */
    if(chroma_array_type != 0)
        for (uint32_t i = 0; i <= num_ref_idx_active_minus1; i++)
            chroma_weight_flag[i] = vav4l2_bits_bit(b); /* chroma_weight_lX_flag[i] */

    for (uint32_t i = 0; i <= num_ref_idx_active_minus1; i++) {
        if (luma_weight_flag[i]) {
            vav4l2_bits_se(b); /* delta_luma_weight_lX[i] */
            vav4l2_bits_se(b); /* luma_offset_lX[i] */
        }
        if (chroma_array_type != 0 && chroma_weight_flag[i]) {
            for(uint32_t j = 0; j < 2; j++) {
                vav4l2_bits_se(b); /* delta_chroma_weight_lX[i][j] */
                vav4l2_bits_se(b); /* delta_chroma_offset_lX[i][j] */
            }
        }
    }
}

/*
 * Parse the slice segment header far enough to measure the reference
 * picture set sections. The short term RPS length is cross-checked with
 * the st_rps_bits value provided through VA-API.
 */
static void hevc_parse_slice_header(struct hevc_parse *codec,
                    const uint8_t *data, size_t size,
                    struct hevc_slice_info *info)
{
    const VAPictureParameterBufferHEVC *pic = &codec->va_pic;
    bool first_slice_in_pic, dependent_slice = false;
    bool slice_temporal_mvp_enabled_flag = false;
    bool slice_sao_luma_flag = false, slice_sao_chroma_flag = false;
    /* Absent syntax elements take their inferred values (7.4.7.1). */
    bool slice_deblocking_filter_disabled_flag =
        pic->slice_parsing_fields.bits.pps_disable_deblocking_filter_flag;
    bool collocated_from_l0_flag = true;
    uint32_t chroma_array_type, slice_type;
    uint32_t offset_len_minus1;
    uint32_t num_ref_idx_l0_active_minus1 = pic->num_ref_idx_l0_default_active_minus1;
    uint32_t num_ref_idx_l1_active_minus1 = pic->num_ref_idx_l1_default_active_minus1;
    bool idr, irap;
    struct vav4l2_bits b;
    size_t mark;

    memset(info, 0, sizeof(*info));
    if (!data || !size || pic->log2_max_pic_order_cnt_lsb_minus4 > 12)
        return;

    vav4l2_bits_init(&b, data, size, true);

    if (pic->pic_fields.bits.separate_colour_plane_flag)
        chroma_array_type = 0;
    else
        chroma_array_type = pic->pic_fields.bits.chroma_format_idc;

    bool forbidden = vav4l2_bits_bit(&b);
    info->nal_unit_type = vav4l2_bits_read(&b, 6);
    unsigned int layer_id = vav4l2_bits_read(&b, 6);
    info->temporal_id_plus1 = vav4l2_bits_read(&b, 3);
    if (forbidden || layer_id || !info->temporal_id_plus1 ||
        info->nal_unit_type > 31)
        return;

    idr = info->nal_unit_type == HEVC_NAL_IDR_W_RADL ||
          info->nal_unit_type == HEVC_NAL_IDR_N_LP;
    irap = info->nal_unit_type >= HEVC_NAL_BLA_W_LP &&
           info->nal_unit_type <= HEVC_NAL_RSV_IRAP_VCL23;

    first_slice_in_pic = vav4l2_bits_bit(&b);

    if (irap)
        info->no_output_of_prior_pics = vav4l2_bits_bit(&b);

    if (vav4l2_bits_ue(&b) > 63)        /* pic_parameter_set_id */
        return;

    if (!first_slice_in_pic) {
        unsigned int ctb_log2 =
            pic->log2_min_luma_coding_block_size_minus3 + 3 +
            pic->log2_diff_max_min_luma_coding_block_size;
        unsigned int ctb;

        /* 7.4.3.2.1: CtbLog2SizeY is 4..6 */
        if (ctb_log2 < 4 || ctb_log2 > 6) {
            info->valid = false;
            return;
        }
        ctb = 1u << ctb_log2;
        unsigned int pic_size_in_ctbs =
            ((pic->pic_width_in_luma_samples + ctb - 1) >> ctb_log2) *
            ((pic->pic_height_in_luma_samples + ctb - 1) >> ctb_log2);

        if (pic->slice_parsing_fields.bits.dependent_slice_segments_enabled_flag)
            dependent_slice = vav4l2_bits_bit(&b);

        vav4l2_bits_read(&b, ceil_log2(pic_size_in_ctbs));
    }

    if (!dependent_slice) {
        vav4l2_bits_skip(&b, pic->num_extra_slice_header_bits);
        slice_type = vav4l2_bits_ue(&b);
        if (slice_type > V4L2_HEVC_SLICE_TYPE_I)
            return;

        if (pic->slice_parsing_fields.bits.output_flag_present_flag)
            vav4l2_bits_bit(&b);

        if (pic->pic_fields.bits.separate_colour_plane_flag)
            vav4l2_bits_read(&b, 2);

        if (!idr) {

            vav4l2_bits_read(&b, pic->log2_max_pic_order_cnt_lsb_minus4 + 4);

            /*
             * short_term_ref_pic_set_sps_flag is NOT part of the st_ref_pic_set()
             * section, so measure after it: the hardware skips over the RPS using
             * short_term_ref_pic_set_size and accounts for this flag bit
             * separately. Including it made the value one bit too large and the
             * hardware started entropy decoding one bit early on every inter frame.
             */
            if (!vav4l2_bits_bit(&b)) { /* short_term_ref_pic_set_sps_flag */
                /*
                 * Inline st_ref_pic_set(): VA-API reports its exact bit size in
                 * st_rps_bits. Parse only to recover num_delta_pocs_of_ref_rps_idx
                 * (which VA-API does not carry) in the inter-prediction case, then
                 * resync to the known size so a heuristic mis-parse can never
                 * shift the rest of the slice header.
                 */
                mark = b.pos;

                if (pic->num_short_term_ref_pic_sets && vav4l2_bits_bit(&b)) {
                    /* inter_ref_pic_set_prediction_flag == 1 */
                    unsigned int count = 0;

                    vav4l2_bits_ue(&b);     /* delta_idx_minus1 */
                    vav4l2_bits_bit(&b);        /* delta_rps_sign */
                    vav4l2_bits_ue(&b);     /* abs_delta_rps_minus1 */

                    while (!b.error && b.pos - mark < pic->st_rps_bits) {
                        if (!vav4l2_bits_bit(&b) &&
                                b.pos - mark < pic->st_rps_bits)
                            vav4l2_bits_bit(&b);
                        count++;
                    }

                    if (count)
                        info->num_delta_pocs_of_ref_rps_idx = count - 1;
                }

                if (b.pos - mark < pic->st_rps_bits) {
                    /* st_rps_bits is client-supplied; never skip past the end
                     * of the slice data (a bogus value would otherwise spin
                     * the bit reader for billions of no-op iterations). */
                    size_t skip = pic->st_rps_bits - (b.pos - mark);
                    size_t avail = size * 8;

                    vav4l2_bits_skip(&b, skip < avail ? skip : avail);
                }

                if (b.pos - mark != pic->st_rps_bits) b.error = true;
                info->short_term_ref_pic_set_size = pic->st_rps_bits;
            } else {
                mark = b.pos;
                if (pic->num_short_term_ref_pic_sets > 1)
                    vav4l2_bits_read(&b,
                            ceil_log2(pic->num_short_term_ref_pic_sets));
                info->short_term_ref_pic_set_size = b.pos - mark;
            }

            mark = b.pos;
            if (pic->slice_parsing_fields.bits.long_term_ref_pics_present_flag) {
                uint32_t num_lt_sps = 0, num_lt_pics;

                if (pic->num_long_term_ref_pic_sps > 0)
                    num_lt_sps = vav4l2_bits_ue(&b);
                num_lt_pics = vav4l2_bits_ue(&b);
                if (num_lt_sps > pic->num_long_term_ref_pic_sps ||
                    num_lt_sps > 32 || num_lt_pics > 32 - num_lt_sps) {
                    b.error = true;
                    goto done;
                }

                for (uint32_t i = 0; i < num_lt_sps + num_lt_pics && !b.error; i++) {
                    if (i < num_lt_sps) {
                        if (pic->num_long_term_ref_pic_sps > 1)
                            vav4l2_bits_read(&b,
                                    ceil_log2(pic->num_long_term_ref_pic_sps));
                    } else {
                        vav4l2_bits_read(&b,
                                pic->log2_max_pic_order_cnt_lsb_minus4 + 4);
                        vav4l2_bits_bit(&b);    /* used_by_curr_pic_lt */
                    }
                    if (vav4l2_bits_bit(&b))        /* delta_poc_msb_present */
                        vav4l2_bits_ue(&b);
                }
            }
            info->long_term_ref_pic_set_size = b.pos - mark;

            if (pic->slice_parsing_fields.bits.sps_temporal_mvp_enabled_flag)
                slice_temporal_mvp_enabled_flag = vav4l2_bits_bit(&b);
        }

        if (pic->slice_parsing_fields.bits.sample_adaptive_offset_enabled_flag) {
            slice_sao_luma_flag = vav4l2_bits_bit(&b);
            if (chroma_array_type != 0)
                slice_sao_chroma_flag = vav4l2_bits_bit(&b);
        }
        if (slice_type == V4L2_HEVC_SLICE_TYPE_P
                || slice_type == V4L2_HEVC_SLICE_TYPE_B) {
            if(vav4l2_bits_bit(&b)) {   /* num_ref_idx_active_override_flag */
                num_ref_idx_l0_active_minus1 = vav4l2_bits_ue(&b);
                if (slice_type == V4L2_HEVC_SLICE_TYPE_B)
                    num_ref_idx_l1_active_minus1 = vav4l2_bits_ue(&b);
            }
            if (num_ref_idx_l0_active_minus1 > 14 || num_ref_idx_l1_active_minus1 > 14) {
                b.error = true;
                goto done;
            }
            if (pic->slice_parsing_fields.bits.lists_modification_present_flag &&
                    codec->num_pic_total_curr > 1) {
                ref_pic_lists_modification(codec, &b,
                        num_ref_idx_l0_active_minus1,
                        num_ref_idx_l1_active_minus1,
                        slice_type == V4L2_HEVC_SLICE_TYPE_B);
            }
            if (slice_type == V4L2_HEVC_SLICE_TYPE_B)
                vav4l2_bits_bit(&b);        /* mvd_l1_zero_flag */
            if (pic->slice_parsing_fields.bits.cabac_init_present_flag)
                vav4l2_bits_bit(&b);        /* cabac_init_flag */
            if (slice_temporal_mvp_enabled_flag) {
                if (slice_type == V4L2_HEVC_SLICE_TYPE_B)
                    collocated_from_l0_flag = vav4l2_bits_bit(&b);
                if ((collocated_from_l0_flag &&
                        num_ref_idx_l0_active_minus1 > 0)
                        || (!collocated_from_l0_flag &&
                        num_ref_idx_l1_active_minus1 > 0))
                    vav4l2_bits_ue(&b); /* collocated_ref_idx */
            }
            if ((pic->pic_fields.bits.weighted_pred_flag &&
                    slice_type == V4L2_HEVC_SLICE_TYPE_P) ||
                    (pic->pic_fields.bits.weighted_bipred_flag &&
                     slice_type == V4L2_HEVC_SLICE_TYPE_B)) {

                vav4l2_bits_ue(&b); /* luma_log2_weight_denom */
                if(chroma_array_type != 0)
                    vav4l2_bits_ue(&b); /* delta_chroma_log2_weight_denom */

                pred_weight_table(&b, num_ref_idx_l0_active_minus1,
                        chroma_array_type);
                if (slice_type == V4L2_HEVC_SLICE_TYPE_B)
                    pred_weight_table(&b, num_ref_idx_l1_active_minus1,
                            chroma_array_type);
            }
            vav4l2_bits_ue(&b); /* five_minus_max_num_merge_cand */
        }
        vav4l2_bits_se(&b); /* slice_qp_delta */
        if (pic->slice_parsing_fields.bits.
                pps_slice_chroma_qp_offsets_present_flag) {
            vav4l2_bits_se(&b); /* slice_cb_qp_offset */
            vav4l2_bits_se(&b); /* slice_cr_qp_offset */
        }

        if (pic->slice_parsing_fields.bits.
                deblocking_filter_override_enabled_flag) {
            if (vav4l2_bits_bit(&b)) {  /* deblocking_filter_override_flag */
                slice_deblocking_filter_disabled_flag = vav4l2_bits_bit(&b);
                if (!slice_deblocking_filter_disabled_flag) {
                    vav4l2_bits_se(&b); /* slice_beta_offset_div2 */
                    vav4l2_bits_se(&b); /* slice_tc_offset_div2 */
                }
            }
        }
        if (pic->pic_fields.bits.pps_loop_filter_across_slices_enabled_flag &&
                (slice_sao_luma_flag || slice_sao_chroma_flag ||
                 !slice_deblocking_filter_disabled_flag))
            vav4l2_bits_bit(&b);    /* slice_loop_filter_across_slices_enabled_flag */
    }


    if (pic->pic_fields.bits.tiles_enabled_flag ||
            pic->pic_fields.bits.entropy_coding_sync_enabled_flag) {
        info->num_entry_point_offsets = vav4l2_bits_ue(&b);

        if (codec->num_entry_point_offsets > codec->max_entry_point_offsets ||
            info->num_entry_point_offsets > codec->max_entry_point_offsets -
            codec->num_entry_point_offsets) {
            b.error = true;
            goto done;
        }
        if (info->num_entry_point_offsets) {
            offset_len_minus1 = vav4l2_bits_ue(&b);
            if (b.error || offset_len_minus1 > 31) {
                b.error = true;
                goto done;
            }
            unsigned int start = codec->num_entry_point_offsets;
            for (uint32_t i = 0; i < info->num_entry_point_offsets; i++) {
                uint32_t offset = vav4l2_bits_read(&b, offset_len_minus1 + 1);
                if (b.error || offset == UINT32_MAX) {
                    b.error = true;
                    goto done;
                }
                if (codec->entry_point_offsets)
                    codec->entry_point_offsets[start + i] = offset + 1;
            }
            codec->num_entry_point_offsets += info->num_entry_point_offsets;
        }
    }

    if (pic->slice_parsing_fields.bits.slice_segment_header_extension_present_flag) {
        unsigned bytes = vav4l2_bits_ue(&b);
        if (bytes > size) b.error = true;
        else vav4l2_bits_skip(&b, (size_t)bytes * 8);
    }
    if (!vav4l2_bits_bit(&b)) b.error = true;
    while (b.pos % 8 && !b.error) if (vav4l2_bits_bit(&b)) b.error = true;
    info->data_byte_offset = b.byte;

done:
    info->valid = !b.error;
}

VAStatus vav4l2_hevc_request_slice(struct vav4l2_context *ctx, const VAPictureParameterBufferHEVC *pic,
    const VASliceParameterBufferHEVC *va, const struct v4l2_ctrl_hevc_decode_params *dec,
    const uint8_t *data, size_t size, unsigned entry_capacity, struct vav4l2_request_slice *out)
{
    struct hevc_parse parser = {.va_pic = *pic, .num_pic_total_curr = dec->num_poc_st_curr_before +
        dec->num_poc_st_curr_after + dec->num_poc_lt_curr, .max_entry_point_offsets = entry_capacity};
    if (entry_capacity) {
        out->entries = calloc(entry_capacity, sizeof(*out->entries));
        if (!out->entries) return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    parser.entry_point_offsets = out->entries;
    hevc_parse_slice_header(&parser, data, size, &out->hevc_info);
    const struct hevc_slice_info *info = &out->hevc_info;
    if (!info->valid || info->num_delta_pocs_of_ref_rps_idx > 15 || info->data_byte_offset >= size || va->slice_data_byte_offset != info->data_byte_offset)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    out->num_entries = parser.num_entry_point_offsets;
    size_t left = size - info->data_byte_offset;
    for (unsigned j = 0; j < out->num_entries; j++) {
        if (out->entries[j] >= left) return VA_STATUS_ERROR_INVALID_PARAMETER;
        left -= out->entries[j];
    }
    struct v4l2_ctrl_hevc_slice_params *s = &out->hevc;
    *s = (struct v4l2_ctrl_hevc_slice_params){.bit_size = out->size * 8, .data_byte_offset = info->data_byte_offset,
        .num_entry_point_offsets = out->num_entries, .nal_unit_type = info->nal_unit_type,
        .nuh_temporal_id_plus1 = info->temporal_id_plus1, .slice_type = va->LongSliceFlags.fields.slice_type,
        .colour_plane_id = va->LongSliceFlags.fields.color_plane_id, .slice_pic_order_cnt = pic->CurrPic.pic_order_cnt,
        .num_ref_idx_l0_active_minus1 = va->num_ref_idx_l0_active_minus1, .num_ref_idx_l1_active_minus1 = va->num_ref_idx_l1_active_minus1,
        .collocated_ref_idx = va->LongSliceFlags.fields.slice_temporal_mvp_enabled_flag ? va->collocated_ref_idx : 0,
        .five_minus_max_num_merge_cand = va->five_minus_max_num_merge_cand, .slice_qp_delta = va->slice_qp_delta,
        .slice_cb_qp_offset = va->slice_cb_qp_offset, .slice_cr_qp_offset = va->slice_cr_qp_offset,
        .slice_beta_offset_div2 = va->slice_beta_offset_div2, .slice_tc_offset_div2 = va->slice_tc_offset_div2,
        .slice_segment_addr = va->slice_segment_address, .short_term_ref_pic_set_size = info->short_term_ref_pic_set_size,
        .long_term_ref_pic_set_size = info->long_term_ref_pic_set_size};
#define FLAG(field, bit) if (va->LongSliceFlags.fields.field) s->flags |= bit
    FLAG(slice_sao_luma_flag, V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_SAO_LUMA);
    FLAG(slice_sao_chroma_flag, V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_SAO_CHROMA);
    FLAG(slice_temporal_mvp_enabled_flag, V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_TEMPORAL_MVP_ENABLED);
    FLAG(mvd_l1_zero_flag, V4L2_HEVC_SLICE_PARAMS_FLAG_MVD_L1_ZERO);
    FLAG(cabac_init_flag, V4L2_HEVC_SLICE_PARAMS_FLAG_CABAC_INIT);
    FLAG(collocated_from_l0_flag, V4L2_HEVC_SLICE_PARAMS_FLAG_COLLOCATED_FROM_L0);
    FLAG(slice_deblocking_filter_disabled_flag, V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_DEBLOCKING_FILTER_DISABLED);
    FLAG(slice_loop_filter_across_slices_enabled_flag, V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_LOOP_FILTER_ACROSS_SLICES_ENABLED);
    FLAG(dependent_slice_segment_flag, V4L2_HEVC_SLICE_PARAMS_FLAG_DEPENDENT_SLICE_SEGMENT);
#undef FLAG
    if (s->slice_type > 2) return VA_STATUS_ERROR_INVALID_PARAMETER;
    unsigned counts[] = {s->num_ref_idx_l0_active_minus1 + 1u, s->num_ref_idx_l1_active_minus1 + 1u};
    uint8_t *refs[] = {s->ref_idx_l0, s->ref_idx_l1};
    for (unsigned l = 0; l < (s->slice_type == 0 ? 2u : s->slice_type == 1 ? 1u : 0u); l++) {
        if (counts[l] > 15) return VA_STATUS_ERROR_INVALID_PARAMETER;
        for (unsigned j = 0; j < counts[l]; j++) {
            unsigned index = va->RefPicList[l][j];
            if (index >= 15 || (pic->ReferenceFrames[index].flags & VA_PICTURE_HEVC_INVALID)) return VA_STATUS_ERROR_INVALID_SURFACE;
            VAStatus status = vav4l2_validate_reference(ctx, pic->ReferenceFrames[index].picture_id);
            if (status != VA_STATUS_SUCCESS) return status;
            uint64_t ts = vav4l2_surface_ts(ctx->driver_data, pic->ReferenceFrames[index].picture_id);
            int found = -1;
            for (unsigned k = 0; k < dec->num_active_dpb_entries; k++) if (dec->dpb[k].timestamp == ts) { found = k; break; }
            if (found < 0) return VA_STATUS_ERROR_INVALID_SURFACE;
            refs[l][j] = found;
        }
    }
    if (va->LongSliceFlags.fields.slice_temporal_mvp_enabled_flag) {
        unsigned list = s->slice_type == 1 || va->LongSliceFlags.fields.collocated_from_l0_flag ? 0 : 1;
        if (s->slice_type == 2 || va->collocated_ref_idx >= counts[list]) return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    struct v4l2_hevc_pred_weight_table *w = &s->pred_weight_table;
    w->luma_log2_weight_denom = va->luma_log2_weight_denom;
    w->delta_chroma_log2_weight_denom = va->delta_chroma_log2_weight_denom;
#define COPY(field) memcpy(w->field, va->field, sizeof(w->field))
    COPY(delta_luma_weight_l0); COPY(luma_offset_l0); COPY(delta_luma_weight_l1); COPY(luma_offset_l1);
    COPY(delta_chroma_weight_l0); COPY(delta_chroma_weight_l1);
#undef COPY
    memcpy(w->chroma_offset_l0, va->ChromaOffsetL0, sizeof(w->chroma_offset_l0));
    memcpy(w->chroma_offset_l1, va->ChromaOffsetL1, sizeof(w->chroma_offset_l1));
    return VA_STATUS_SUCCESS;
}
