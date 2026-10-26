/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Slice parsing and controls adapted from Omacom libva-v4l2_request.
 * Copyright (C) 2026 Ondrej Jirman <megi@xff.cz> */
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "request_slices.h"
#include "request_bits.h"
#define SLICE_P 0
#define SLICE_B 1
#define SLICE_I 2
#define SLICE_SP 3
#define SLICE_SI 4
static void h264_parse_slice_header(const VAPictureParameterBufferH264 *pic,
                    const VASliceParameterBufferH264 *va_slice,
                    const uint8_t *data, size_t size,
                    struct h264_slice_info *info)
{
    unsigned int chroma_array_type = pic->seq_fields.bits.chroma_format_idc;
    bool field_pic = pic->pic_fields.bits.field_pic_flag;
    struct vav4l2_bits b;
    uint32_t slice_type;
    size_t mark;

    memset(info, 0, sizeof(*info));
    if (va_slice->num_ref_idx_l0_active_minus1 > 31 ||
        va_slice->num_ref_idx_l1_active_minus1 > 31 ||
        pic->seq_fields.bits.log2_max_frame_num_minus4 > 12 ||
        pic->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 > 12 ||
        pic->seq_fields.bits.pic_order_cnt_type > 2)
        return;

    vav4l2_bits_init(&b, data, size, true);

    if (vav4l2_bits_bit(&b))            /* forbidden_zero_bit */
        return;
    info->nal_ref_idc = vav4l2_bits_read(&b, 2);
    info->nal_unit_type = vav4l2_bits_read(&b, 5);
    if (info->nal_unit_type != 1 && info->nal_unit_type != 5) {
        switch (info->nal_unit_type) {
        case 2: /* partition A */
        case 3: /* partition B */
        case 4: /* partition C */
        case 20: /* SVC/MVC extension */
        case 21: /* 3D-AVC extension */
            /* Recognised and unsupported: report it as such instead of
             * returning as if the buffer were malformed. Anything else in a
             * slice-data buffer is treated as invalid. */
            info->unsupported = true;
            info->unsupported_nal_unit_type = info->nal_unit_type;
            break;
        default:
            break;
        }
        return;
    }
    info->idr = info->nal_unit_type == 5;

    vav4l2_bits_ue(&b);             /* first_mb_in_slice */
    slice_type = vav4l2_bits_ue(&b);
    if (slice_type > 9)
        return;
    info->slice_type = slice_type % 5;
    info->pic_parameter_set_id = vav4l2_bits_ue(&b);
    if (info->pic_parameter_set_id > 255)
        return;

    vav4l2_bits_read(&b, pic->seq_fields.bits.log2_max_frame_num_minus4 + 4);

    if (!pic->seq_fields.bits.frame_mbs_only_flag) {
        if (vav4l2_bits_bit(&b))            /* field_pic_flag */
            vav4l2_bits_bit(&b);        /* bottom_field_flag */
    }

    if (info->idr)
        info->idr_pic_id = vav4l2_bits_ue(&b);

    mark = b.pos;
    if (pic->seq_fields.bits.pic_order_cnt_type == 0) {
        info->pic_order_cnt_lsb = vav4l2_bits_read(&b,
            pic->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 + 4);
        if (pic->pic_fields.bits.pic_order_present_flag && !field_pic)
            info->delta_pic_order_cnt_bottom = vav4l2_bits_se(&b);
    } else if (pic->seq_fields.bits.pic_order_cnt_type == 1 &&
           !pic->seq_fields.bits.delta_pic_order_always_zero_flag) {
        info->delta_pic_order_cnt0 = vav4l2_bits_se(&b);
        if (pic->pic_fields.bits.pic_order_present_flag && !field_pic)
            info->delta_pic_order_cnt1 = vav4l2_bits_se(&b);
    }
    info->pic_order_cnt_bit_size = b.pos - mark;

    if (pic->pic_fields.bits.redundant_pic_cnt_present_flag)
        info->redundant_pic_cnt = vav4l2_bits_ue(&b);

    if (info->slice_type == SLICE_B)
        vav4l2_bits_bit(&b);        /* direct_spatial_mv_pred_flag */

    if (info->slice_type == SLICE_P || info->slice_type == SLICE_SP ||
        info->slice_type == SLICE_B) {
        info->num_ref_idx_override = vav4l2_bits_bit(&b);
        if (info->num_ref_idx_override) {
            vav4l2_bits_ue(&b); /* num_ref_idx_l0_active_minus1 */
            if (info->slice_type == SLICE_B)
                vav4l2_bits_ue(&b);
        }
    }

    /* ref_pic_list_modification() */
    if (info->slice_type != SLICE_I && info->slice_type != SLICE_SI) {
        if (vav4l2_bits_bit(&b)) {
            uint32_t idc;
            do {
                idc = vav4l2_bits_ue(&b);
                if (idc == 0 || idc == 1 || idc == 2)
                    vav4l2_bits_ue(&b);
            } while (idc != 3 && !b.error);
        }
    }
    if (info->slice_type == SLICE_B) {
        if (vav4l2_bits_bit(&b)) {
            uint32_t idc;
            do {
                idc = vav4l2_bits_ue(&b);
                if (idc == 0 || idc == 1 || idc == 2)
                    vav4l2_bits_ue(&b);
            } while (idc != 3 && !b.error);
        }
    }

    /* pred_weight_table() */
    if ((pic->pic_fields.bits.weighted_pred_flag &&
         (info->slice_type == SLICE_P || info->slice_type == SLICE_SP)) ||
        (pic->pic_fields.bits.weighted_bipred_idc == 1 &&
         info->slice_type == SLICE_B)) {
        unsigned int lists = info->slice_type == SLICE_B ? 2 : 1;

        vav4l2_bits_ue(&b);     /* luma_log2_weight_denom */
        if (chroma_array_type)
            vav4l2_bits_ue(&b); /* chroma_log2_weight_denom */

        for (unsigned int list = 0; list < lists; list++) {
            unsigned int count = list ?
                va_slice->num_ref_idx_l1_active_minus1 + 1 :
                va_slice->num_ref_idx_l0_active_minus1 + 1;

            for (unsigned int i = 0; i < count && !b.error; i++) {
                if (vav4l2_bits_bit(&b)) {
                    vav4l2_bits_se(&b);
                    vav4l2_bits_se(&b);
                }
                if (chroma_array_type && vav4l2_bits_bit(&b)) {
                    for (int j = 0; j < 4; j++)
                        vav4l2_bits_se(&b);
                }
            }
        }
    }

    /* dec_ref_pic_marking() */
    mark = b.pos;
    if (info->nal_ref_idc) {
        if (info->idr) {
            vav4l2_bits_bit(&b);    /* no_output_of_prior_pics */
            vav4l2_bits_bit(&b);    /* long_term_reference_flag */
        } else if (vav4l2_bits_bit(&b)) {
            uint32_t op;
            do {
                op = vav4l2_bits_ue(&b);
                if (op == 1 || op == 3)
                    vav4l2_bits_ue(&b);
                if (op == 2)
                    vav4l2_bits_ue(&b);
                if (op == 3 || op == 6)
                    vav4l2_bits_ue(&b);
                if (op == 4)
                    vav4l2_bits_ue(&b);
            } while (op != 0 && !b.error);
        }
    }
    info->dec_ref_pic_marking_bit_size = b.pos - mark;

    if (pic->pic_fields.bits.entropy_coding_mode_flag &&
        info->slice_type != SLICE_I && info->slice_type != SLICE_SI)
        if (vav4l2_bits_ue(&b) != va_slice->cabac_init_idc) b.error = true;

    if (vav4l2_bits_se(&b) != va_slice->slice_qp_delta) b.error = true;

    if (info->slice_type == SLICE_SP || info->slice_type == SLICE_SI) {
        if (info->slice_type == SLICE_SP)
            vav4l2_bits_bit(&b);    /* sp_for_switch_flag */
        vav4l2_bits_se(&b);     /* slice_qs_delta */
    }

    if (pic->pic_fields.bits.deblocking_filter_control_present_flag) {
        unsigned disable = vav4l2_bits_ue(&b);
        if (disable > 2 || disable != va_slice->disable_deblocking_filter_idc) b.error = true;
        if (disable != 1) {
            if (vav4l2_bits_se(&b) != va_slice->slice_alpha_c0_offset_div2) b.error = true;
            if (vav4l2_bits_se(&b) != va_slice->slice_beta_offset_div2) b.error = true;
        }
    }

    /* Offset in bits to slice_data() from the start of the slice NAL unit,
     * i.e. including the NAL header byte. cedrus (and the V4L2 interface)
     * count header_bit_size from bit 0 of the submitted slice buffer, which
     * begins at the NAL header byte, so this must not exclude it. */
    info->header_bit_size = b.pos;
    info->valid = !b.error;
}

VAStatus vav4l2_h264_request_slice(struct vav4l2_context *ctx, const VAPictureParameterBufferH264 *pic,
    const VASliceParameterBufferH264 *va, const struct v4l2_ctrl_h264_decode_params *dec,
    const uint8_t *data, size_t size, struct vav4l2_request_slice *out)
{
    if (va->slice_type % 5 > 2 || pic->seq_fields.bits.residual_colour_transform_flag)
        return VA_STATUS_ERROR_UNIMPLEMENTED;
    h264_parse_slice_header(pic, va, data, size, &out->h264_info);
    const struct h264_slice_info *info = &out->h264_info;
    if (info->unsupported) return VA_STATUS_ERROR_UNIMPLEMENTED;
    if (!info->valid || info->redundant_pic_cnt > 127 || info->header_bit_size >= size * 8)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    struct v4l2_ctrl_h264_slice_params *s = &out->h264;
    *s = (struct v4l2_ctrl_h264_slice_params){.header_bit_size = info->header_bit_size,
        .first_mb_in_slice = va->first_mb_in_slice, .slice_type = va->slice_type % 5,
        .redundant_pic_cnt = info->redundant_pic_cnt, .cabac_init_idc = va->cabac_init_idc,
        .slice_qp_delta = va->slice_qp_delta, .disable_deblocking_filter_idc = va->disable_deblocking_filter_idc,
        .slice_alpha_c0_offset_div2 = va->slice_alpha_c0_offset_div2, .slice_beta_offset_div2 = va->slice_beta_offset_div2,
        .num_ref_idx_l0_active_minus1 = va->num_ref_idx_l0_active_minus1,
        .num_ref_idx_l1_active_minus1 = va->num_ref_idx_l1_active_minus1};
    if (s->slice_type == 1 && va->direct_spatial_mv_pred_flag) s->flags |= V4L2_H264_SLICE_FLAG_DIRECT_SPATIAL_MV_PRED;
    const VAPictureH264 *lists[] = {va->RefPicList0, va->RefPicList1};
    struct v4l2_h264_reference *refs[] = {s->ref_pic_list0, s->ref_pic_list1};
    unsigned counts[] = {s->num_ref_idx_l0_active_minus1 + 1u, s->num_ref_idx_l1_active_minus1 + 1u};
    for (unsigned l = 0; l < (s->slice_type == 1 ? 2u : s->slice_type == 0 ? 1u : 0u); l++) {
        if (counts[l] > 32) return VA_STATUS_ERROR_INVALID_PARAMETER;
        for (unsigned j = 0; j < counts[l]; j++) {
            const VAPictureH264 *ref = &lists[l][j];
            VAStatus status = vav4l2_validate_reference(ctx, ref->picture_id);
            if (status != VA_STATUS_SUCCESS || (ref->flags & VA_PICTURE_H264_INVALID)) return VA_STATUS_ERROR_INVALID_SURFACE;
            int found = -1;
            uint64_t ts = vav4l2_surface_ts(ctx->driver_data, ref->picture_id);
            for (unsigned k = 0; k < 16; k++) if ((dec->dpb[k].flags & V4L2_H264_DPB_ENTRY_FLAG_VALID) &&
                dec->dpb[k].reference_ts == ts && dec->dpb[k].frame_num == ref->frame_idx) { found = k; break; }
            if (found < 0) return VA_STATUS_ERROR_INVALID_SURFACE;
            unsigned fields = ref->flags & (VA_PICTURE_H264_TOP_FIELD | VA_PICTURE_H264_BOTTOM_FIELD);
            refs[l][j] = (struct v4l2_h264_reference){.index = found, .fields = fields == VA_PICTURE_H264_TOP_FIELD ?
                V4L2_H264_TOP_FIELD_REF : fields == VA_PICTURE_H264_BOTTOM_FIELD ? V4L2_H264_BOTTOM_FIELD_REF : V4L2_H264_FRAME_REF};
        }
    }
    struct v4l2_ctrl_h264_pred_weights *w = &out->weights;
    w->luma_log2_weight_denom = va->luma_log2_weight_denom; w->chroma_log2_weight_denom = va->chroma_log2_weight_denom;
    if (w->luma_log2_weight_denom > 7 || w->chroma_log2_weight_denom > 7) return VA_STATUS_ERROR_INVALID_PARAMETER;
    memcpy(w->weight_factors[0].luma_weight, va->luma_weight_l0, sizeof(va->luma_weight_l0));
    memcpy(w->weight_factors[0].luma_offset, va->luma_offset_l0, sizeof(va->luma_offset_l0));
    memcpy(w->weight_factors[0].chroma_weight, va->chroma_weight_l0, sizeof(va->chroma_weight_l0));
    memcpy(w->weight_factors[0].chroma_offset, va->chroma_offset_l0, sizeof(va->chroma_offset_l0));
    memcpy(w->weight_factors[1].luma_weight, va->luma_weight_l1, sizeof(va->luma_weight_l1));
    memcpy(w->weight_factors[1].luma_offset, va->luma_offset_l1, sizeof(va->luma_offset_l1));
    memcpy(w->weight_factors[1].chroma_weight, va->chroma_weight_l1, sizeof(va->chroma_weight_l1));
    memcpy(w->weight_factors[1].chroma_offset, va->chroma_offset_l1, sizeof(va->chroma_offset_l1));
    return VA_STATUS_SUCCESS;
}
