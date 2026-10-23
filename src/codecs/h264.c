/*
 * v4l2request — H.264 parameter translation
 *
 * Maps VA-API H.264 buffers to V4L2 stateless controls and submits
 * a decode request via the V4L2 Request API.
 *
 * Flow:
 *   1. Translate VAPictureParameterBufferH264 → SPS + PPS + decode_params
 *   2. Translate VAIQMatrixBufferH264 → scaling_matrix
 *   3. Translate VASliceParameterBufferH264 → slice_params
 *   4. Set all controls via VIDIOC_S_EXT_CTRLS (bound to request)
 *   5. Write slice data into output buffer (pre-mapped mmap)
 *   6. Queue output + capture buffers with request_fd
 *   7. Submit request via MEDIA_REQUEST_IOC_QUEUE
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <poll.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <va/va.h>
#include <linux/v4l2-controls.h>
#include <linux/videodev2.h>

#include "va_request.h"

/* Convert VA-API SPS fields to V4L2 SPS flags */
static uint32_t h264_sps_flags(const VAPictureParameterBufferH264 *pic)
{
    uint32_t flags = 0;

    if (pic->seq_fields.bits.frame_mbs_only_flag)
        flags |= V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY;
    if (pic->seq_fields.bits.mb_adaptive_frame_field_flag)
        flags |= V4L2_H264_SPS_FLAG_MB_ADAPTIVE_FRAME_FIELD;
    if (pic->seq_fields.bits.direct_8x8_inference_flag)
        flags |= V4L2_H264_SPS_FLAG_DIRECT_8X8_INFERENCE;
    if (pic->seq_fields.bits.delta_pic_order_always_zero_flag)
        flags |= V4L2_H264_SPS_FLAG_DELTA_PIC_ORDER_ALWAYS_ZERO;
    if (pic->seq_fields.bits.residual_colour_transform_flag)
        flags |= V4L2_H264_SPS_FLAG_SEPARATE_COLOUR_PLANE;
    if (pic->seq_fields.bits.gaps_in_frame_num_value_allowed_flag)
        flags |= V4L2_H264_SPS_FLAG_GAPS_IN_FRAME_NUM_VALUE_ALLOWED;

    return flags;
}

/* Convert VA-API PPS fields to V4L2 PPS flags */
static uint32_t h264_pps_flags(const VAPictureParameterBufferH264 *pic)
{
    uint32_t flags = 0;

    if (pic->pic_fields.bits.entropy_coding_mode_flag)
        flags |= V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE;
    if (pic->pic_fields.bits.pic_order_present_flag)
        flags |= V4L2_H264_PPS_FLAG_BOTTOM_FIELD_PIC_ORDER_IN_FRAME_PRESENT;
    if (pic->pic_fields.bits.weighted_pred_flag)
        flags |= V4L2_H264_PPS_FLAG_WEIGHTED_PRED;
    if (pic->pic_fields.bits.transform_8x8_mode_flag)
        flags |= V4L2_H264_PPS_FLAG_TRANSFORM_8X8_MODE;
    if (pic->pic_fields.bits.constrained_intra_pred_flag)
        flags |= V4L2_H264_PPS_FLAG_CONSTRAINED_INTRA_PRED;
    if (pic->pic_fields.bits.deblocking_filter_control_present_flag)
        flags |= V4L2_H264_PPS_FLAG_DEBLOCKING_FILTER_CONTROL_PRESENT;
    if (pic->pic_fields.bits.redundant_pic_cnt_present_flag)
        flags |= V4L2_H264_PPS_FLAG_REDUNDANT_PIC_CNT_PRESENT;

    return flags;
}

/*
 * Find the V4L2 timestamp for a VA-API reference picture.
 * Walk the driver's surface table looking for a surface whose
 * picture_id matches. Returns the surface's timestamp (which was
 * set during begin_picture when that picture was decoded).
 *
 * If the surface is not found (e.g., unused DPB slot), returns 0.
 */
static uint64_t h264_find_ref_timestamp(struct vareq_driver_data *dd,
                                         VASurfaceID picture_id)
{
    struct vareq_surface *surf;

    if (picture_id == VA_INVALID_SURFACE || picture_id == 0xFFFFFFFF)
        return 0;

    surf = vareq_surface_by_id(dd, picture_id);
    if (surf)
        return surf->timestamp;

    return 0;
}

/*
 * Translate VAPictureH264 reference frame info to V4L2 DPB entry
 */
static void h264_fill_dpb_entry(struct v4l2_h264_dpb_entry *dpb,
                                const VAPictureH264 *ref,
                                uint64_t ref_ts)
{
    dpb->top_field_order_cnt = ref->TopFieldOrderCnt;
    dpb->bottom_field_order_cnt = ref->BottomFieldOrderCnt;
    dpb->frame_num = ref->frame_idx;

    /* Map flags */
    dpb->flags = 0;
    if (ref->flags & (VA_PICTURE_H264_SHORT_TERM_REFERENCE | VA_PICTURE_H264_LONG_TERM_REFERENCE))
        dpb->flags |= V4L2_H264_DPB_ENTRY_FLAG_ACTIVE;
    if (ref->flags & VA_PICTURE_H264_LONG_TERM_REFERENCE)
        dpb->flags |= V4L2_H264_DPB_ENTRY_FLAG_LONG_TERM;
    if (ref->flags & (VA_PICTURE_H264_TOP_FIELD | VA_PICTURE_H264_BOTTOM_FIELD))
        dpb->flags |= V4L2_H264_DPB_ENTRY_FLAG_FIELD;

    dpb->reference_ts = ref_ts;
    dpb->pic_num = ref->frame_idx;
    dpb->fields = 0;
    if (ref->flags & VA_PICTURE_H264_TOP_FIELD)
        dpb->fields |= V4L2_H264_TOP_FIELD_REF;
    if (ref->flags & VA_PICTURE_H264_BOTTOM_FIELD)
        dpb->fields |= V4L2_H264_BOTTOM_FIELD_REF;
    if (!dpb->fields)
        dpb->fields = V4L2_H264_FRAME_REF;
}

/*
 * Translate VA-API picture parameters to V4L2 H.264 SPS
 */
void h264_fill_sps(struct v4l2_ctrl_h264_sps *sps,
                   const VAPictureParameterBufferH264 *pic,
                   VAProfile profile)
{
    memset(sps, 0, sizeof(*sps));

    sps->chroma_format_idc = pic->seq_fields.bits.chroma_format_idc;
    sps->bit_depth_luma_minus8 = pic->bit_depth_luma_minus8;
    sps->bit_depth_chroma_minus8 = pic->bit_depth_chroma_minus8;
    sps->log2_max_frame_num_minus4 = pic->seq_fields.bits.log2_max_frame_num_minus4;
    sps->pic_order_cnt_type = pic->seq_fields.bits.pic_order_cnt_type;
    sps->log2_max_pic_order_cnt_lsb_minus4 = pic->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4;
    sps->max_num_ref_frames = pic->num_ref_frames;
    sps->pic_width_in_mbs_minus1 = pic->picture_width_in_mbs_minus1;
    sps->pic_height_in_map_units_minus1 = (pic->picture_height_in_mbs_minus1 + 1) /
        (pic->seq_fields.bits.frame_mbs_only_flag ? 1 : 2) - 1;

    /* profile_idc / constraint_set are not in VA picture params; take them
     * from the VA config profile ffmpeg selected for this stream. */
    if (profile == VAProfileH264ConstrainedBaseline) {
        sps->profile_idc = 66;  /* Baseline */
        sps->constraint_set_flags = V4L2_H264_SPS_CONSTRAINT_SET0_FLAG |
                                    V4L2_H264_SPS_CONSTRAINT_SET1_FLAG;
    } else if (profile == VAProfileH264Main) {
        sps->profile_idc = 77;
        sps->constraint_set_flags = 0;
    } else if (profile == VAProfileH264High422 ||
               pic->seq_fields.bits.chroma_format_idc == 2) {
        sps->profile_idc = 122;
        sps->constraint_set_flags = 0;
    } else if (profile == VAProfileH264High10 ||
               pic->bit_depth_luma_minus8 >= 2) {
        sps->profile_idc = 110;
        sps->constraint_set_flags = 0;
    } else {
        sps->profile_idc = 100; /* High */
        sps->constraint_set_flags = 0;
    }
    sps->level_idc = 40;     /* Level 4.0 (covers 1080p) */

    sps->flags = h264_sps_flags(pic);
}

/*
 * Translate VA-API picture parameters to V4L2 H.264 PPS
 */
void h264_fill_pps(struct v4l2_ctrl_h264_pps *pps,
                   const VAPictureParameterBufferH264 *pic,
                   const VASliceParameterBufferH264 *slice)
{
    memset(pps, 0, sizeof(*pps));

    pps->pic_parameter_set_id = 0;  /* Not in VA-API pic params; default from bitstream */
    pps->seq_parameter_set_id = 0;  /* Not in VA-API pic params; default from bitstream */
    pps->num_slice_groups_minus1 = 0;
    /* The kernel's slice parser falls back to these defaults when the slice
     * does not override them — feed the effective active counts. */
    pps->num_ref_idx_l0_default_active_minus1 =
        slice && (slice->slice_type % 5 == 0 || slice->slice_type % 5 == 1 || slice->slice_type % 5 == 3) ? slice->num_ref_idx_l0_active_minus1 : 0;
    pps->num_ref_idx_l1_default_active_minus1 =
        slice && slice->slice_type % 5 == 1 ? slice->num_ref_idx_l1_active_minus1 : 0;
    pps->weighted_bipred_idc = pic->pic_fields.bits.weighted_bipred_idc;
    /*
     * ffmpeg bakes the bit-depth QP offset (QpBdOffsetY = 6 * (depth-8))
     * into pic_init_qp/qs: it fills VA from pps->init_qp - 26 where
     * init_qp = se(v) + 26 + QpBdOffsetY. The V4L2 PPS control wants the
     * raw bitstream value — the VDPU381 applies the depth offset itself
     * (GStreamer sends the raw value and decodes bit-exact). Strip the
     * offset for 10-bit or dequantization runs 12 QP steps hot.
     */
    int qp_bd_offset = 6 * pic->bit_depth_luma_minus8;
    pps->pic_init_qp_minus26 = pic->pic_init_qp_minus26 - qp_bd_offset;
    pps->pic_init_qs_minus26 = pic->pic_init_qs_minus26 - qp_bd_offset;
    pps->chroma_qp_index_offset = pic->chroma_qp_index_offset;
    pps->second_chroma_qp_index_offset = pic->second_chroma_qp_index_offset;
    pps->flags = h264_pps_flags(pic);
}

/*
 * Translate VA-API picture parameters to V4L2 H.264 decode params
 */
void h264_fill_decode_params(struct v4l2_ctrl_h264_decode_params *dec,
                             const VAPictureParameterBufferH264 *pic,
                             struct vareq_driver_data *dd,
                             const VASliceParameterBufferH264 *slice)
{
    memset(dec, 0, sizeof(*dec));

    /* Fill DPB from reference frames, looking up their V4L2 timestamps */
    for (int i = 0; i < 16; i++) {
        const VAPictureH264 *ref = &pic->ReferenceFrames[i];
        if ((ref->flags & VA_PICTURE_H264_INVALID) || ref->picture_id == VA_INVALID_SURFACE ||
            !(ref->flags & (VA_PICTURE_H264_SHORT_TERM_REFERENCE | VA_PICTURE_H264_LONG_TERM_REFERENCE)))
            continue;
        uint64_t ref_ts = h264_find_ref_timestamp(dd, ref->picture_id);
        h264_fill_dpb_entry(&dec->dpb[i], ref, ref_ts);
        /* V4L2 wants long_term_frame_idx for long-term frame_num, not
         * the original slice FrameNum saved on the surface. */
        int32_t number = ref->frame_idx;
        if (!(ref->flags & VA_PICTURE_H264_LONG_TERM_REFERENCE) &&
            pic->seq_fields.bits.log2_max_frame_num_minus4 <= 12 && ref->frame_idx > pic->frame_num)
            number -= 1 << (pic->seq_fields.bits.log2_max_frame_num_minus4 + 4);
        if (pic->pic_fields.bits.field_pic_flag) {
            unsigned same = pic->CurrPic.flags & VA_PICTURE_H264_BOTTOM_FIELD ? VA_PICTURE_H264_BOTTOM_FIELD : VA_PICTURE_H264_TOP_FIELD;
            number = 2 * number + !!(ref->flags & same);
        }
        dec->dpb[i].pic_num = (uint32_t)number;
        /* VALID marks the slot as populated — the kernel skips entries
         * without it. ACTIVE marks it usable as a reference. */
        dec->dpb[i].flags |= V4L2_H264_DPB_ENTRY_FLAG_VALID;
    }

    /* Order DPB slots ascending by frame_num, mirroring reference userspace */
    for (int i = 1; i < 16; i++) {
        struct v4l2_h264_dpb_entry key = dec->dpb[i];
        int j = i - 1;
        while (j >= 0 && dec->dpb[j].frame_num > key.frame_num) {
            dec->dpb[j + 1] = dec->dpb[j];
            j--;
        }
        dec->dpb[j + 1] = key;
    }

    /* Current picture info */
    unsigned st = slice ? slice->slice_type % 5 : 2;
    dec->frame_num = pic->frame_num;
    dec->top_field_order_cnt = pic->CurrPic.TopFieldOrderCnt;
    dec->bottom_field_order_cnt = pic->CurrPic.BottomFieldOrderCnt;

    /* NAL identity and header bit sizes are filled from the actual RBSP. */

    /* Flags — the frame-based UAPI needs the picture type (PFRAME/BFRAME
     * configure the decoder mode). Slice type: 0=P, 1=B, 2=I (5-7 same). */
    dec->flags = 0;
    if (pic->pic_fields.bits.field_pic_flag)
        dec->flags |= V4L2_H264_DECODE_PARAM_FLAG_FIELD_PIC;
    if (pic->CurrPic.flags & VA_PICTURE_H264_BOTTOM_FIELD)
        dec->flags |= V4L2_H264_DECODE_PARAM_FLAG_BOTTOM_FIELD;
    if (st == 0)
        dec->flags |= V4L2_H264_DECODE_PARAM_FLAG_PFRAME;
    else if (st == 1)
        dec->flags |= V4L2_H264_DECODE_PARAM_FLAG_BFRAME;
}

struct h264_bits {
    const uint8_t *data;
    size_t size, offset, bits;
    unsigned byte, left, zeros;
    int error;
};

static uint32_t h264_bits(struct h264_bits *r, unsigned count)
{
    uint32_t value = 0;
    if (count > 32) { r->error = 1; return 0; }
    for (unsigned i = 0; i < count && !r->error; i++) {
        if (!r->left) {
            if (r->offset == r->size) { r->error = 1; break; }
            unsigned byte = r->data[r->offset++];
            if (r->zeros == 2 && byte == 3) {
                if (r->offset == r->size || r->data[r->offset] > 3) { r->error = 1; break; }
                byte = r->data[r->offset++]; r->zeros = 0;
            }
            r->zeros = byte == 0 ? (r->zeros < 2 ? r->zeros + 1 : 2) : 0;
            r->byte = byte; r->left = 8;
        }
        value = (value << 1) | ((r->byte >> --r->left) & 1); r->bits++;
    }
    return value;
}

static uint32_t h264_ue(struct h264_bits *r)
{
    unsigned zeros = 0;
    while (!h264_bits(r, 1) && !r->error) {
        if (++zeros > 31) { r->error = 1; return 0; }
    }
    return ((UINT32_C(1) << zeros) - 1) + h264_bits(r, zeros);
}

static int32_t h264_se(struct h264_bits *r)
{
    uint32_t value = h264_ue(r);
    int64_t result = value & 1 ? ((int64_t)value + 1) / 2 : -(int64_t)(value / 2);
    if (result > INT32_MAX) { r->error = 1; return 0; }
    return (int32_t)result;
}

static void h264_list_modifications(struct h264_bits *r)
{
    if (!h264_bits(r, 1)) return;
    while (!r->error) {
        uint32_t op = h264_ue(r);
        if (op == 3) return;
        if (op > 2) { r->error = 1; return; }
        h264_ue(r);
    }
}

static void h264_weights(struct h264_bits *r, unsigned refs, int chroma)
{
    for (unsigned i = 0; i <= refs && !r->error; i++) {
        if (h264_bits(r, 1)) { h264_se(r); h264_se(r); }
        if (chroma && h264_bits(r, 1))
            for (unsigned j = 0; j < 2; j++) { h264_se(r); h264_se(r); }
    }
}

/* Read the first VCL header, including variable-length POC and MMCO syntax.
 * Never infer IDR from frame_num: a non-IDR intra picture may wrap to zero. */
static VAStatus h264_parse_header(const uint8_t *data, size_t size,
                                  const VAPictureParameterBufferH264 *pic,
                                  const VASliceParameterBufferH264 *slice,
                                  struct v4l2_ctrl_h264_pps *pps,
                                  struct v4l2_ctrl_h264_decode_params *dec, int *override, struct h264_bits *marking, int *mmco5)
{
    if (!data || !pic || !slice || !pps || !dec || size < 5) return VA_STATUS_ERROR_INVALID_PARAMETER;
    unsigned prefix = data[0] == 0 && data[1] == 0 && data[2] == 1 ? 3 :
        data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1 ? 4 : 0;
    if (!prefix || size <= prefix + 1) return VA_STATUS_ERROR_INVALID_PARAMETER;
    unsigned nal = data[prefix], type = nal & 31, idr = type == 5;
    if ((nal & 128) || (type != 1 && type != 5) || (idr && !(nal & 96))) return VA_STATUS_ERROR_INVALID_PARAMETER;
    size_t end = prefix + 1;
    while (end + 2 < size && !(data[end] == 0 && data[end + 1] == 0 &&
           (data[end + 2] == 1 || (end + 3 < size && data[end + 2] == 0 && data[end + 3] == 1)))) end++;
    if (end + 2 >= size) end = size;
    struct h264_bits r = {.data = data + prefix + 1, .size = end - prefix - 1};
    uint32_t first_mb = h264_ue(&r), slice_type = h264_ue(&r), pps_id = h264_ue(&r);
    if (slice_type > 9 || slice_type % 5 != slice->slice_type % 5 || first_mb != slice->first_mb_in_slice || pps_id > 255 ||
        pic->seq_fields.bits.log2_max_frame_num_minus4 > 12 ||
        (pic->seq_fields.bits.pic_order_cnt_type == 0 && pic->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 > 12) ||
        pic->seq_fields.bits.pic_order_cnt_type > 2) return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (pic->seq_fields.bits.residual_colour_transform_flag) h264_bits(&r, 2);
    uint32_t frame_num = h264_bits(&r, pic->seq_fields.bits.log2_max_frame_num_minus4 + 4);
    unsigned field = 0, bottom = 0, st = slice_type % 5;
    if (!pic->seq_fields.bits.frame_mbs_only_flag) { field = h264_bits(&r, 1); if (field) bottom = h264_bits(&r, 1); }
    if (frame_num != pic->frame_num || field != pic->pic_fields.bits.field_pic_flag ||
        (field && bottom != !!(pic->CurrPic.flags & VA_PICTURE_H264_BOTTOM_FIELD)) ||
        !!(nal & 96) != !!pic->pic_fields.bits.reference_pic_flag || (idr && st != 2 && st != 4))
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    struct v4l2_ctrl_h264_decode_params parsed = *dec;
    parsed.pic_order_cnt_lsb = 0; parsed.delta_pic_order_cnt_bottom = 0;
    parsed.delta_pic_order_cnt0 = parsed.delta_pic_order_cnt1 = 0;
    parsed.nal_ref_idc = (nal >> 5) & 3;
    uint32_t idr_pic_id = idr ? h264_ue(&r) : 0;
    if (idr_pic_id > 65535) return VA_STATUS_ERROR_INVALID_PARAMETER;
    parsed.idr_pic_id = idr_pic_id;
    size_t start = r.bits;
    if (pic->seq_fields.bits.pic_order_cnt_type == 0) {
        parsed.pic_order_cnt_lsb = h264_bits(&r, pic->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 + 4);
        if (pic->pic_fields.bits.pic_order_present_flag && !field) parsed.delta_pic_order_cnt_bottom = h264_se(&r);
    } else if (pic->seq_fields.bits.pic_order_cnt_type == 1 && !pic->seq_fields.bits.delta_pic_order_always_zero_flag) {
        parsed.delta_pic_order_cnt0 = h264_se(&r);
        if (pic->pic_fields.bits.pic_order_present_flag && !field) parsed.delta_pic_order_cnt1 = h264_se(&r);
    }
    parsed.pic_order_cnt_bit_size = r.bits - start;
    if (pic->pic_fields.bits.redundant_pic_cnt_present_flag) h264_ue(&r);
    if (st == 1 && h264_bits(&r, 1) != slice->direct_spatial_mv_pred_flag) return VA_STATUS_ERROR_INVALID_PARAMETER;
    unsigned l0 = st == 0 || st == 1 || st == 3 ? slice->num_ref_idx_l0_active_minus1 : 0;
    unsigned l1 = st == 1 ? slice->num_ref_idx_l1_active_minus1 : 0;
    int active_override = (st == 0 || st == 1 || st == 3) ? (int)h264_bits(&r, 1) : -1;
    if (active_override == 1) { l0 = h264_ue(&r); if (st == 1) l1 = h264_ue(&r); }
    if (l0 > 31 || l1 > 31 || ((st == 0 || st == 1 || st == 3) && l0 != slice->num_ref_idx_l0_active_minus1) ||
        (st == 1 && l1 != slice->num_ref_idx_l1_active_minus1)) return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (st != 2 && st != 4) { h264_list_modifications(&r); if (st == 1) h264_list_modifications(&r); }
    if ((pic->pic_fields.bits.weighted_pred_flag && (st == 0 || st == 3)) ||
        (pic->pic_fields.bits.weighted_bipred_idc == 1 && st == 1)) {
        int chroma = pic->seq_fields.bits.chroma_format_idc && !pic->seq_fields.bits.residual_colour_transform_flag;
        if (h264_ue(&r) > 7 || (chroma && h264_ue(&r) > 7)) r.error = 1;
        h264_weights(&r, l0, chroma); if (st == 1) h264_weights(&r, l1, chroma);
    }
    start = r.bits;
    struct h264_bits marking_start = r;
    int reset = 0;
    if (parsed.nal_ref_idc) {
        if (idr) h264_bits(&r, 2);
        else if (h264_bits(&r, 1)) while (!r.error) {
            unsigned op = h264_ue(&r);
            if (!op) break;
            if (op > 6) { r.error = 1; break; }
            if (op == 5) reset = 1;
            if (op == 1 || op == 3) h264_ue(&r);
            if (op == 2) h264_ue(&r);
            if (op == 3 || op == 6) h264_ue(&r);
            if (op == 4) h264_ue(&r);
        }
    }
    parsed.dec_ref_pic_marking_bit_size = r.bits - start;
    parsed.flags &= ~V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC;
    if (idr) parsed.flags |= V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC;
    if (r.error) return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (override) *override = active_override;
    if (marking) *marking = marking_start;
    if (mmco5) *mmco5 = reset;
    pps->pic_parameter_set_id = pps_id; *dec = parsed;
    return VA_STATUS_SUCCESS;
}

VAStatus vareq_h264_parse_header(const uint8_t *data, size_t size, const VAPictureParameterBufferH264 *pic,
    const VASliceParameterBufferH264 *slice, struct v4l2_ctrl_h264_pps *pps, struct v4l2_ctrl_h264_decode_params *dec)
{
    return h264_parse_header(data, size, pic, slice, pps, dec, NULL, NULL, NULL);
}

/* Validate every complete logical slice,not just the first NAL. Defaults
 * must come from a slice without an override when one is available. */
static VAStatus h264_validate_headers(const uint8_t *data, size_t size, const VAPictureParameterBufferH264 *pic,
    struct vareq_buffer **buffers, int count, struct v4l2_ctrl_h264_pps *pps, struct v4l2_ctrl_h264_decode_params *dec, unsigned *defaults, int *mmco5)
{
    if (!pic || !buffers || count <= 0 || !pps || !dec) return VA_STATUS_ERROR_INVALID_PARAMETER;
    struct v4l2_ctrl_h264_pps result_pps = *pps;
    struct v4l2_ctrl_h264_decode_params first = *dec;
    size_t offset = 0, length; const uint8_t *nal;
    struct h264_bits first_marking = {0};
    unsigned slices = 0, picture_type = 2, seen_types = 0; int default_l0 = -1, default_l1 = -1, all_type = -1;
    for (int i = 0; i < count; i++) {
        struct vareq_buffer *b = buffers[i];
        if (!b || b->type != VASliceParameterBufferType) continue;
        if (!b->data || b->size < sizeof(VASliceParameterBufferH264)) return VA_STATUS_ERROR_INVALID_PARAMETER;
        unsigned n = b->num_elements ? b->num_elements : 1;
        if ((b->allocated_elements && n > b->allocated_elements) || b->size > UINT32_MAX / n) return VA_STATUS_ERROR_INVALID_PARAMETER;
        for (unsigned j = 0; j < n; j++) {
            VASliceParameterBufferH264 value;
            memcpy(&value, (const uint8_t *)b->data + (size_t)j * b->size, sizeof(value));
            const VASliceParameterBufferH264 *slice = &value;
            if (slice->slice_data_flag != VA_SLICE_DATA_FLAG_ALL && slice->slice_data_flag != VA_SLICE_DATA_FLAG_BEGIN) continue;
            if (vareq_annexb_next(data, size, &offset, &nal, &length) != 1) return VA_STATUS_ERROR_INVALID_PARAMETER;
            struct v4l2_ctrl_h264_pps current_pps = result_pps;
            struct v4l2_ctrl_h264_decode_params current = *dec; int override; struct h264_bits marking;
            int reset;
            VAStatus status = h264_parse_header(nal, length, pic, slice, &current_pps, &current, &override, &marking, &reset);
            if (status != VA_STATUS_SUCCESS) return status;
            if (mmco5 && reset) *mmco5 = 1;
            if (slices && (current_pps.pic_parameter_set_id != result_pps.pic_parameter_set_id ||
                current.idr_pic_id != first.idr_pic_id || ((current.flags ^ first.flags) & V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC) ||
                current.pic_order_cnt_lsb != first.pic_order_cnt_lsb || current.delta_pic_order_cnt_bottom != first.delta_pic_order_cnt_bottom ||
                current.delta_pic_order_cnt0 != first.delta_pic_order_cnt0 || current.delta_pic_order_cnt1 != first.delta_pic_order_cnt1))
                return VA_STATUS_ERROR_INVALID_PARAMETER;
            /* Marking belongs to the picture: equal bit lengths alone do not
             * establish equal IDR flags or MMCO operations/arguments. Compare
             * decoded RBSP bits, independently of slice offsets and EPBs. */
            if (slices) {
                if (current.dec_ref_pic_marking_bit_size != first.dec_ref_pic_marking_bit_size) return VA_STATUS_ERROR_INVALID_PARAMETER;
                struct h264_bits previous = first_marking;
                for (unsigned left = current.dec_ref_pic_marking_bit_size; left;) {
                    unsigned n = left > 32 ? 32 : left;
                    if (h264_bits(&previous, n) != h264_bits(&marking, n) || previous.error || marking.error)
                        return VA_STATUS_ERROR_INVALID_PARAMETER;
                    left -= n;
                }
            }
            if (!slices) { result_pps = current_pps; first = current; first_marking = marking; }
            unsigned type = slice->slice_type % 5;
            if ((all_type >= 0 && type != (unsigned)all_type) || (slice->slice_type >= 5 && (seen_types & ~(1u << type)))) return VA_STATUS_ERROR_INVALID_PARAMETER;
            seen_types |= 1u << type;
            if (slice->slice_type >= 5) all_type = type;
            if (type == 1 || (type == 0 && picture_type != 1)) picture_type = type;
            if (override == 0) {
                if (default_l0 >= 0 && default_l0 != slice->num_ref_idx_l0_active_minus1) return VA_STATUS_ERROR_INVALID_PARAMETER;
                default_l0 = slice->num_ref_idx_l0_active_minus1;
                if (type == 1) {
                    if (default_l1 >= 0 && default_l1 != slice->num_ref_idx_l1_active_minus1) return VA_STATUS_ERROR_INVALID_PARAMETER;
                    default_l1 = slice->num_ref_idx_l1_active_minus1;
                }
            }
            slices++;
        }
    }
    if (!slices || vareq_annexb_next(data, size, &offset, &nal, &length) != 0) return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (default_l0 >= 0) result_pps.num_ref_idx_l0_default_active_minus1 = default_l0;
    if (default_l1 >= 0) result_pps.num_ref_idx_l1_default_active_minus1 = default_l1;
    first.flags &= ~(V4L2_H264_DECODE_PARAM_FLAG_PFRAME | V4L2_H264_DECODE_PARAM_FLAG_BFRAME);
    if (picture_type == 0) first.flags |= V4L2_H264_DECODE_PARAM_FLAG_PFRAME;
    if (picture_type == 1) first.flags |= V4L2_H264_DECODE_PARAM_FLAG_BFRAME;
    if (defaults) *defaults = (default_l0 >= 0) | ((default_l1 >= 0) << 1);
    *pps = result_pps; *dec = first;
    return VA_STATUS_SUCCESS;
}

VAStatus vareq_h264_validate_headers(const uint8_t *data, size_t size, const VAPictureParameterBufferH264 *pic,
    struct vareq_buffer **buffers, int count, struct v4l2_ctrl_h264_pps *pps, struct v4l2_ctrl_h264_decode_params *dec)
{
    return h264_validate_headers(data, size, pic, buffers, count, pps, dec, NULL, NULL);
}

/*
 * Translate VA-API IQ matrix to V4L2 H.264 scaling matrix
 */
void h264_fill_scaling_matrix(struct v4l2_ctrl_h264_scaling_matrix *sm,
                              const VAIQMatrixBufferH264 *iq)
{
    memset(sm, 16, sizeof(*sm));

    /* 4x4 scaling lists: 6 lists, 16 elements each */
    memcpy(sm->scaling_list_4x4, iq->ScalingList4x4, sizeof(sm->scaling_list_4x4));
    /* VA-API exposes only two 8x8 lists; kernel has six — copy the two and
     * default the rest flat (they are only consulted when the bitstream
     * enables them). */

    /* 8x8 scaling lists: 6 lists, 64 elements each */
    /* VA-API only has 2 8x8 lists, V4L2 expects 6 */
    memcpy(sm->scaling_list_8x8[0], iq->ScalingList8x8[0], 64);
    memcpy(sm->scaling_list_8x8[1], iq->ScalingList8x8[1], 64);
    /* Remaining lists retain the flat value 16. */
}

struct vareq_h264_field {
    VASurfaceID surface;
    uint64_t timestamp;
    VAPictureParameterBufferH264 pic;
    struct v4l2_ctrl_h264_sps sps;
    struct v4l2_ctrl_h264_pps pps;
    struct v4l2_ctrl_h264_decode_params dec;
    struct v4l2_ctrl_h264_scaling_matrix sm;
    unsigned defaults;
    int matrix;
    size_t size;
    uint8_t data[];
};

int vareq_h264_field_pending(const struct vareq_context *ctx, VASurfaceID surface)
{
    return ctx && ctx->h264_field && ctx->h264_field->surface == surface;
}

void vareq_h264_discard_field(struct vareq_context *ctx, int failed)
{
    if (!ctx || !ctx->h264_field) return;
    struct vareq_surface *s = vareq_surface_by_id(ctx->driver_data, ctx->h264_field->surface);
    if (failed && s && s->status == VASurfaceRendering) s->status = VASurfaceSkipped;
    free(ctx->h264_field); ctx->h264_field = NULL;
}

/* Stage only owned bytes and translated controls. VA buffers may be destroyed
 * as soon as the first EndPicture returns. No request is queued for a half frame. */
static VAStatus h264_pair_fields(struct vareq_context *ctx, VAPictureParameterBufferH264 *picture,
    struct v4l2_ctrl_h264_sps *sps, struct v4l2_ctrl_h264_pps *pps, struct v4l2_ctrl_h264_decode_params *dec,
    const struct v4l2_ctrl_h264_scaling_matrix *sm, int matrix, unsigned defaults, int mmco5,
    const uint8_t *data, size_t *size, uint8_t **paired, struct vareq_h264_field **retained)
{
    if (!ctx->current_surface || picture->CurrPic.picture_id != ctx->current_surface_id)
        return VA_STATUS_ERROR_INVALID_SURFACE;
    unsigned fields = picture->CurrPic.flags & (VA_PICTURE_H264_TOP_FIELD | VA_PICTURE_H264_BOTTOM_FIELD);
    if (fields != VA_PICTURE_H264_TOP_FIELD && fields != VA_PICTURE_H264_BOTTOM_FIELD)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    struct vareq_h264_field *first = ctx->h264_field;
    if (!first) {
        if (*size > UINT32_MAX || *size > SIZE_MAX - sizeof(*first)) return VA_STATUS_ERROR_ALLOCATION_FAILED;
        first = malloc(sizeof(*first) + *size);
        if (!first) return VA_STATUS_ERROR_ALLOCATION_FAILED;
        *first = (struct vareq_h264_field){.surface = ctx->current_surface_id, .timestamp = ctx->current_surface->timestamp,
            .pic = *picture, .sps = *sps, .pps = *pps, .dec = *dec, .sm = *sm, .defaults = defaults, .matrix = matrix, .size = *size};
        memcpy(first->data, data, *size); ctx->h264_field = first;
        vareq_decode_trace(ctx, "h264-field-hold", 0, fields);
        return VA_STATUS_SUCCESS;
    }
    unsigned first_fields = first->pic.CurrPic.flags & (VA_PICTURE_H264_TOP_FIELD | VA_PICTURE_H264_BOTTOM_FIELD);
    if (first->surface != ctx->current_surface_id || first->timestamp != ctx->current_surface->timestamp)
        return VA_STATUS_ERROR_INVALID_SURFACE;
    if (fields == first_fields || first->pic.frame_num != picture->frame_num)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (memcmp(sps, &first->sps, sizeof(*sps)) || matrix != first->matrix || (matrix && memcmp(sm, &first->sm, sizeof(*sm))))
        return VA_STATUS_ERROR_UNIMPLEMENTED;
    struct v4l2_ctrl_h264_pps common = first->pps, second = *pps;
    unsigned a[] = {first->pps.num_ref_idx_l0_default_active_minus1, first->pps.num_ref_idx_l1_default_active_minus1};
    unsigned b[] = {pps->num_ref_idx_l0_default_active_minus1, pps->num_ref_idx_l1_default_active_minus1};
    for (unsigned i = 0; i < 2; i++) {
        if ((first->defaults & defaults & (1u << i)) && a[i] != b[i]) return VA_STATUS_ERROR_INVALID_PARAMETER;
        if (defaults & (1u << i)) a[i] = b[i];
    }
    common.num_ref_idx_l0_default_active_minus1 = second.num_ref_idx_l0_default_active_minus1 = a[0];
    common.num_ref_idx_l1_default_active_minus1 = second.num_ref_idx_l1_default_active_minus1 = a[1];
    if (memcmp(&common, &second, sizeof(common))) return VA_STATUS_ERROR_UNIMPLEMENTED;
    /* A complementary pair has matching reference status. Its second
     * reference field is neither IDR nor an MMCO5 picture (H.264 3.36). */
    if (first->pic.pic_fields.bits.reference_pic_flag != picture->pic_fields.bits.reference_pic_flag ||
        (dec->flags & V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC) || mmco5)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    for (unsigned i = 0; i < 16; i++) {
        const VAPictureH264 *ref = &first->pic.ReferenceFrames[i];
        if ((ref->flags & VA_PICTURE_H264_INVALID) || !(ref->flags & (VA_PICTURE_H264_SHORT_TERM_REFERENCE | VA_PICTURE_H264_LONG_TERM_REFERENCE))) continue;
        VAStatus status = vareq_validate_reference(ctx, ref->picture_id);
        if (status != VA_STATUS_SUCCESS) return status;
        uint64_t timestamp = h264_find_ref_timestamp(ctx->driver_data, ref->picture_id); int found = 0;
        for (unsigned j = 0; j < 16; j++)
            if ((first->dec.dpb[j].flags & V4L2_H264_DPB_ENTRY_FLAG_VALID) && first->dec.dpb[j].reference_ts == timestamp) found = 1;
        if (!found) return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    for (unsigned i = 0; i < 16; i++) {
        const VAPictureH264 *ref = &picture->ReferenceFrames[i];
        unsigned active = ref->flags & (VA_PICTURE_H264_SHORT_TERM_REFERENCE | VA_PICTURE_H264_LONG_TERM_REFERENCE);
        if ((ref->flags & VA_PICTURE_H264_INVALID) || !active) continue;
        unsigned referenced = ref->flags & (VA_PICTURE_H264_TOP_FIELD | VA_PICTURE_H264_BOTTOM_FIELD);
        if (ref->picture_id == first->surface) {
            if (!first->pic.pic_fields.bits.reference_pic_flag ||
                (active == VA_PICTURE_H264_SHORT_TERM_REFERENCE && ref->frame_idx != first->pic.frame_num) || referenced != first_fields ||
                (first_fields == VA_PICTURE_H264_TOP_FIELD ? ref->TopFieldOrderCnt != first->pic.CurrPic.TopFieldOrderCnt :
                 ref->BottomFieldOrderCnt != first->pic.CurrPic.BottomFieldOrderCnt)) return VA_STATUS_ERROR_INVALID_PARAMETER;
            continue; /* The first request writes this field into the held CAPTURE. */
        }
        int found = 0;
        for (unsigned j = 0; j < 16; j++) {
            const VAPictureH264 *old = &first->pic.ReferenceFrames[j];
            if ((old->flags & VA_PICTURE_H264_INVALID) || old->picture_id != ref->picture_id || old->frame_idx != ref->frame_idx ||
                (old->flags & (VA_PICTURE_H264_SHORT_TERM_REFERENCE | VA_PICTURE_H264_LONG_TERM_REFERENCE)) != active) continue;
            unsigned available = old->flags & (VA_PICTURE_H264_TOP_FIELD | VA_PICTURE_H264_BOTTOM_FIELD);
            if (!available) available = VA_PICTURE_H264_TOP_FIELD | VA_PICTURE_H264_BOTTOM_FIELD;
            if (!referenced) referenced = VA_PICTURE_H264_TOP_FIELD | VA_PICTURE_H264_BOTTOM_FIELD;
            if ((referenced & ~available) || ((referenced & VA_PICTURE_H264_TOP_FIELD) && ref->TopFieldOrderCnt != old->TopFieldOrderCnt) ||
                ((referenced & VA_PICTURE_H264_BOTTOM_FIELD) && ref->BottomFieldOrderCnt != old->BottomFieldOrderCnt)) continue;
            found = 1; break;
        }
        if (!found) return VA_STATUS_ERROR_UNIMPLEMENTED; /* Conservative subset until external-DPB transitions have target vectors. */
    }
    if (*size > UINT32_MAX - first->size) return VA_STATUS_ERROR_ALLOCATION_FAILED;
    *paired = malloc(first->size + *size);
    if (!*paired) return VA_STATUS_ERROR_ALLOCATION_FAILED;
    memcpy(*paired, first->data, first->size); memcpy(*paired + first->size, data, *size); *size += first->size;
    int32_t top = fields == VA_PICTURE_H264_TOP_FIELD ? picture->CurrPic.TopFieldOrderCnt : first->pic.CurrPic.TopFieldOrderCnt;
    int32_t bottom = fields == VA_PICTURE_H264_BOTTOM_FIELD ? picture->CurrPic.BottomFieldOrderCnt : first->pic.CurrPic.BottomFieldOrderCnt;
    uint32_t types = first->dec.flags | dec->flags;
    *picture = first->pic; picture->CurrPic.TopFieldOrderCnt = top; picture->CurrPic.BottomFieldOrderCnt = bottom;
    *sps = first->sps; *pps = common; *dec = first->dec; dec->top_field_order_cnt = top; dec->bottom_field_order_cnt = bottom;
    dec->flags &= ~(V4L2_H264_DECODE_PARAM_FLAG_PFRAME | V4L2_H264_DECODE_PARAM_FLAG_BFRAME);
    if (types & V4L2_H264_DECODE_PARAM_FLAG_BFRAME) dec->flags |= V4L2_H264_DECODE_PARAM_FLAG_BFRAME;
    else if (types & V4L2_H264_DECODE_PARAM_FLAG_PFRAME) dec->flags |= V4L2_H264_DECODE_PARAM_FLAG_PFRAME;
    *retained = first; ctx->h264_field = NULL;
    vareq_decode_trace(ctx, "h264-field-pair", 0, *size);
    return VA_STATUS_SUCCESS;
}

static VAStatus h264_submit_picture(struct vareq_context *ctx, const VAPictureParameterBufferH264 *pic_param,
    struct v4l2_ctrl_h264_sps *sps, struct v4l2_ctrl_h264_pps *pps, struct v4l2_ctrl_h264_decode_params *dec,
    struct v4l2_ctrl_h264_scaling_matrix *sm, int matrix, const uint8_t *paired, size_t total,
    struct vareq_buffer **buffers, int num_buffers, int hold, int *field_capture);

/*
 * Main H.264 decode pipeline
 *
 * Called from vareq_end_picture when all VA-API buffers have been collected.
 * Translates parameters, submits to V4L2, waits for result.
 *
 * This is the critical path: if this function works, H.264 decoding works.
 */
VAStatus vareq_h264_translate(struct vareq_context *ctx,
                               struct vareq_buffer **buffers,
                               int num_buffers)
{
    /* Parse collected buffers */
    VAPictureParameterBufferH264 *pic_param = NULL;
    VAIQMatrixBufferH264 *iq_matrix = NULL;
    VASliceParameterBufferH264 *slice_param = NULL;
    /* A picture may consist of many slices — one VASliceDataBuffer each */

    struct vareq_collected cb;
    int n_slice_data = 0;

    vareq_collect_decode_buffers(buffers, num_buffers, &cb);
    pic_param = cb.pic;
    iq_matrix = cb.iq;
    slice_param = cb.n_slice_params ? cb.slice_params[0] : NULL;
    n_slice_data = cb.n_slice_datas;

    if (!pic_param) {
        fprintf(stderr, "v4l2request: H.264 decode missing picture params\n");
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    if (n_slice_data == 0) {
        fprintf(stderr, "v4l2request: H.264 decode missing slice data\n");
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    if (pic_param->seq_fields.bits.log2_max_frame_num_minus4 > 12 ||
        pic_param->seq_fields.bits.pic_order_cnt_type > 2 ||
        (pic_param->seq_fields.bits.pic_order_cnt_type == 0 && pic_param->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 > 12) ||
        pic_param->frame_num >= (1u << (pic_param->seq_fields.bits.log2_max_frame_num_minus4 + 4)) ||
        (!pic_param->seq_fields.bits.frame_mbs_only_flag && ((pic_param->picture_height_in_mbs_minus1 + 1) & 1)) ||
        (pic_param->seq_fields.bits.frame_mbs_only_flag && pic_param->pic_fields.bits.field_pic_flag))
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    /* Standard VA decode buffers omit the POC1 SPS cycle offsets. */
    if (pic_param->seq_fields.bits.pic_order_cnt_type == 1) {
        fprintf(stderr, "v4l2request: H.264 POC1 requires original SPS cycle metadata\n");
        return VA_STATUS_ERROR_UNIMPLEMENTED;
    }
    if (pic_param->pic_fields.bits.field_pic_flag && !ctx->current_surface) return VA_STATUS_ERROR_INVALID_SURFACE;
    if (pic_param->pic_fields.bits.field_pic_flag && !vareq_h264_field_supported(ctx)) return VA_STATUS_ERROR_UNIMPLEMENTED;
    if (!pic_param->pic_fields.bits.field_pic_flag && ctx->h264_field) vareq_h264_discard_field(ctx, 0);

    for (unsigned i = 0; i < 16; i++) {
        const VAPictureH264 *ref = &pic_param->ReferenceFrames[i];
        if (ref->flags & VA_PICTURE_H264_INVALID) continue;
        unsigned active = ref->flags & (VA_PICTURE_H264_SHORT_TERM_REFERENCE | VA_PICTURE_H264_LONG_TERM_REFERENCE);
        if (!active) continue;
        if (active == (VA_PICTURE_H264_SHORT_TERM_REFERENCE | VA_PICTURE_H264_LONG_TERM_REFERENCE))
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        if (ref->frame_idx > UINT16_MAX || (active == VA_PICTURE_H264_SHORT_TERM_REFERENCE &&
            ref->frame_idx >= (1u << (pic_param->seq_fields.bits.log2_max_frame_num_minus4 + 4))))
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        if (pic_param->pic_fields.bits.field_pic_flag && vareq_h264_field_pending(ctx, ref->picture_id)) continue;
        VAStatus status = vareq_validate_reference(ctx, ref->picture_id);
        if (status != VA_STATUS_SUCCESS) return status;
    }

    size_t total = 0;
    VAStatus packet_status = vareq_packetize_slices(VAREQ_CODEC_H264, buffers, num_buffers, NULL, UINT32_MAX, &total);
    if (packet_status != VA_STATUS_SUCCESS) return packet_status;

    /*
     * Step 1: Translate VA-API parameters to V4L2 controls
     */
    struct v4l2_ctrl_h264_sps sps;
    struct v4l2_ctrl_h264_pps pps;
    struct v4l2_ctrl_h264_decode_params dec;
    struct v4l2_ctrl_h264_scaling_matrix sm;
    /*
     * Note: VDPU381 (kernel 7.0+ refactored rkvdec) uses the frame-based
     * UAPI — V4L2_CID_STATELESS_H264_SLICE_PARAMS is not registered and the
     * kernel parses the slice header itself from the Annex B bitstream.
     */

    h264_fill_sps(&sps, pic_param, ctx->profile);
    h264_fill_pps(&pps, pic_param, slice_param);
    h264_fill_decode_params(&dec, pic_param, ctx->driver_data, slice_param);
    if (ctx->n_free_out == 0) return VA_STATUS_ERROR_OPERATION_FAILED;
    int out_buf_idx = ctx->free_out_bufs[ctx->n_free_out - 1];
    if (out_buf_idx < 0 || out_buf_idx >= VAREQ_NUM_OUTPUT_BUFS || !ctx->output_buf_ptr[out_buf_idx])
        return VA_STATUS_ERROR_OPERATION_FAILED;
    uint8_t *header_data = ctx->output_buf_ptr[out_buf_idx];
    int temporary = total > ctx->output_buf_size;
    if (temporary) {
        header_data = malloc(total); /* a larger SPS may resize OUTPUT only after its header is known */
        if (!header_data) return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    packet_status = vareq_packetize_slices(VAREQ_CODEC_H264, buffers, num_buffers,
        header_data, temporary ? total : ctx->output_buf_size, &total);
    unsigned defaults = 0;
    int mmco5 = 0;
    if (packet_status == VA_STATUS_SUCCESS)
        packet_status = h264_validate_headers(header_data, total, pic_param, buffers, num_buffers, &pps, &dec, &defaults, &mmco5);
    if (packet_status != VA_STATUS_SUCCESS) { if (temporary) free(header_data); return packet_status; }

    if (iq_matrix) {
        h264_fill_scaling_matrix(&sm, iq_matrix);
        pps.flags |= V4L2_H264_PPS_FLAG_SCALING_MATRIX_PRESENT;
    }
    else
        memset(&sm, 0, sizeof(sm));

    uint8_t *paired = NULL;
    struct vareq_h264_field *first = NULL;
    struct v4l2_ctrl_h264_decode_params second_dec = dec;
    VAPictureParameterBufferH264 paired_picture = *pic_param;
    if (pic_param->pic_fields.bits.field_pic_flag)
        packet_status = h264_pair_fields(ctx, &paired_picture, &sps, &pps, &dec, &sm, iq_matrix != NULL,
            defaults, mmco5, header_data, &total, &paired, &first);
    if (temporary) free(header_data);
    if (packet_status != VA_STATUS_SUCCESS) return packet_status;
    if (pic_param->pic_fields.bits.field_pic_flag && ctx->h264_field) return VA_STATUS_SUCCESS;
    if (first) {
        /* VDPU381 completes one coded field per request. HOLD keeps its
         * CAPTURE queued for the opposite field, as used by upstream GStreamer.
         * Keep both owned payloads: the first request overwrites OUTPUT. */
        int capture = -1;
        second_dec.top_field_order_cnt = dec.top_field_order_cnt;
        second_dec.bottom_field_order_cnt = dec.bottom_field_order_cnt;
        packet_status = h264_submit_picture(ctx, &first->pic, &first->sps, &pps, &first->dec, &first->sm,
            first->matrix, paired, first->size, buffers, num_buffers, 1, &capture);
        if (packet_status == VA_STATUS_SUCCESS)
            packet_status = h264_submit_picture(ctx, pic_param, &sps, &pps, &second_dec, &sm,
                iq_matrix != NULL, paired + first->size, total - first->size, buffers, num_buffers, 0, &capture);
        if (packet_status != VA_STATUS_SUCCESS && capture >= 0) vareq_decode_reset(ctx);
        free(first);
    } else packet_status = h264_submit_picture(ctx, pic_param, &sps, &pps, &dec, &sm,
        iq_matrix != NULL, NULL, total, buffers, num_buffers, 0, NULL);
    free(paired);
    return packet_status;
}

static VAStatus h264_submit_picture(struct vareq_context *ctx, const VAPictureParameterBufferH264 *pic_param,
    struct v4l2_ctrl_h264_sps *sps, struct v4l2_ctrl_h264_pps *pps, struct v4l2_ctrl_h264_decode_params *dec,
    struct v4l2_ctrl_h264_scaling_matrix *sm, int matrix, const uint8_t *paired, size_t total,
    struct vareq_buffer **buffers, int num_buffers, int hold, int *field_capture)
{
    int out_buf_idx;
    VAStatus packet_status;

    /*
     * Step 2: Set V4L2 ext controls bound to the request
     *
     * We set SPS, PPS, decode_params, scaling_matrix, and slice_params
     * as separate VIDIOC_S_EXT_CTRLS calls, each bound to the same request_fd.
     *
     * Note: V4L2 stateless controls are set via ext_controls, not regular
     * v4l2_control. Each control class (H264_SPS, H264_PPS, etc.) is set
     * independently.
     */

    int request_fd = ctx->request_fd;
    int v4l2_fd = ctx->v4l2_fd;

    if (request_fd < 0) {
        fprintf(stderr, "v4l2request: H.264 no request fd available\n");
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    /* rkvdec only accepts NV15/NV16/NV20 after a matching SPS. HEVC
     * programs SPS as a global control before STREAMON; High10 must
     * do the same. Request-scoped SPS still goes on the picture. */
    {
        int w = (pic_param->picture_width_in_mbs_minus1 + 1) * 16;
        int h = (pic_param->picture_height_in_mbs_minus1 + 1) * 16;
        uint32_t cap = vareq_capture_fourcc_from_sps(
            pic_param->bit_depth_luma_minus8,
            pic_param->seq_fields.bits.chroma_format_idc);
        struct v4l2_ext_control g_sps_ctrl = { 0 };
        struct v4l2_ext_controls g_sps_ctrls = { 0 };

        g_sps_ctrl.id = V4L2_CID_STATELESS_H264_SPS;
        g_sps_ctrl.p_h264_sps = sps;
        g_sps_ctrl.size = sizeof(*sps);
        g_sps_ctrls.controls = &g_sps_ctrl;
        g_sps_ctrls.count = 1;
        /* Sequence-level control: resubmit only when the payload changed. */
        _Static_assert(sizeof(*sps) <= sizeof(ctx->g_ctrl_payload), "grow cache");
        if (!ctx->g_ctrl_valid ||
            memcmp(ctx->g_ctrl_payload, sps, sizeof(*sps)) != 0) {
            if (vareq_set_global_controls(v4l2_fd, &g_sps_ctrls) < 0) {
                fprintf(stderr,
                        "v4l2request: H.264 global SPS not accepted\n");
                if (ctx->capture_bufs_allocd <= 0)
                    return VA_STATUS_ERROR_OPERATION_FAILED;
                ctx->g_ctrl_valid = 0;
            } else {
                memcpy(ctx->g_ctrl_payload, sps, sizeof(*sps));
                ctx->g_ctrl_valid = 1;
            }
        }

        if (vareq_ensure_capture(ctx, w, h, cap) < 0) {
            fprintf(stderr, "v4l2request: H.264 capture reconfig failed\n");
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        if (!ctx->streamed) {
            if (vareq_streamon(v4l2_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) < 0 ||
                vareq_streamon(v4l2_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) < 0) {
                fprintf(stderr, "v4l2request: H.264 STREAMON failed\n");
                return VA_STATUS_ERROR_OPERATION_FAILED;
            }
            ctx->streamed = 1;
        }
    }

    /* Request-scoped picture controls in ONE ioctl. */
    struct v4l2_ext_control pic_ctrls[3] = { { 0 }, { 0 }, { 0 } };
    struct v4l2_ext_controls batch = { 0 };

    pic_ctrls[0].id = V4L2_CID_STATELESS_H264_SPS;
    pic_ctrls[0].p_h264_sps = sps;
    pic_ctrls[0].size = sizeof(*sps);
    pic_ctrls[1].id = V4L2_CID_STATELESS_H264_PPS;
    pic_ctrls[1].p_h264_pps = pps;
    pic_ctrls[1].size = sizeof(*pps);
    pic_ctrls[2].id = V4L2_CID_STATELESS_H264_DECODE_PARAMS;
    pic_ctrls[2].p_h264_decode_params = dec;
    pic_ctrls[2].size = sizeof(*dec);
    batch.controls = pic_ctrls;
    batch.count = 3;

    if (vareq_set_request_controls(request_fd, v4l2_fd, &batch) < 0) {
        fprintf(stderr, "v4l2request: failed to set H.264 picture controls\n");
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    /* Set H.264 scaling matrix (if provided) */
    if (matrix) {
        struct v4l2_ext_control sm_ctrl = { 0 };
        sm_ctrl.id = V4L2_CID_STATELESS_H264_SCALING_MATRIX;
        sm_ctrl.p_h264_scaling_matrix = sm;
        sm_ctrl.size = sizeof(*sm);

        struct v4l2_ext_controls sm_ctrls = { 0 };
        sm_ctrls.controls = &sm_ctrl;
        sm_ctrls.count = 1;

        if (vareq_set_request_controls(request_fd, v4l2_fd, &sm_ctrls) < 0) {
            fprintf(stderr, "v4l2request: failed to set H.264 scaling matrix\n");
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
    }

    /*
     * VDPU381 frame-based UAPI: no slice-params control to set here —
     * the kernel parses the slice header from the bitstream we queue below.
     */

    /*
     * Step 3: write the bitstream, submit the request, and wait for it.
     *
     * VA-API clients call vaSyncSurface after vaEndPicture anyway, so we run
     * the pipeline synchronously with a single request in flight: every
     * buffer we queue is dequeued again before returning. This keeps buffer
     * ownership unambiguous — the free pools in the context never lie.
     */
    if (ctx->n_free_out == 0) {
        fprintf(stderr, "v4l2request: no free output buffer\n");
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    out_buf_idx = ctx->free_out_bufs[--ctx->n_free_out];
    if (out_buf_idx < 0 || out_buf_idx >= VAREQ_NUM_OUTPUT_BUFS || !ctx->output_buf_ptr[out_buf_idx]) {
        vareq_out_pool_push(ctx, out_buf_idx);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    uint64_t timestamp = ctx->current_surface ? ctx->current_surface->timestamp : 0;

    /* SPS/capture negotiation may have remapped OUTPUT after header parsing.
     * Submit from the current mapping and pool, never the earlier scratch view. */
    if (paired) {
        packet_status = total <= ctx->output_buf_size ? VA_STATUS_SUCCESS : VA_STATUS_ERROR_OPERATION_FAILED;
        if (packet_status == VA_STATUS_SUCCESS) memcpy(ctx->output_buf_ptr[out_buf_idx], paired, total);
    } else packet_status = vareq_packetize_slices(VAREQ_CODEC_H264, buffers, num_buffers,
        ctx->output_buf_ptr[out_buf_idx], ctx->output_buf_size, &total);
    if (packet_status != VA_STATUS_SUCCESS) {
        vareq_out_pool_push(ctx, out_buf_idx);
        return packet_status;
    }

    /*
     * Submit synchronously. On failure decode_submit resets both queues so
     * nothing is left queued in the kernel — do NOT push out_buf_idx back.
     */
    int done_cap = field_capture ? vareq_decode_submit_field(ctx, out_buf_idx, (uint32_t)total, timestamp, *field_capture, hold) :
        vareq_decode_submit(ctx, out_buf_idx, (uint32_t)total, timestamp);
    if (done_cap == -2)
        return VA_STATUS_ERROR_DECODING_ERROR;
    if (done_cap < 0)
        return VA_STATUS_ERROR_OPERATION_FAILED;
    if (hold) { *field_capture = done_cap; return VA_STATUS_SUCCESS; }

    /* Attach the decoded frame to the target surface */
    struct vareq_surface *surf = ctx->current_surface;
    if (surf) {
        if (vareq_surface_pull_capture(ctx, surf, done_cap) < 0) {
            fprintf(stderr, "v4l2request: H.264 pull capture failed\n");
            if (!vareq_surface_capture_matches(ctx, surf) || surf->buf_index != done_cap) vareq_cap_pool_push(ctx, done_cap);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        surf->h264_frame_num = pic_param->frame_num;
        surf->h264_frame_num_valid = 1;
    } else {
        vareq_cap_pool_push(ctx, done_cap);
    }

    return VA_STATUS_SUCCESS;
}

const int vareq_has_h264 = 1;
