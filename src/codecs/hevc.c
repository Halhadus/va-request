/*
 * v4l2request — HEVC parameter translation
 *
 * Maps VA-API HEVC buffers to V4L2 stateless controls and submits
 * a decode request. VA provides the current DPB/RPS, but not the complete
 * original SPS arrays for HEVC_EXT_SPS_ST_RPS / HEVC_EXT_SPS_LT_RPS.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <poll.h>
#include <unistd.h>

#include <va/va.h>
#include <va/va_dec_hevc.h>
#include <linux/v4l2-controls.h>
#include <linux/videodev2.h>

#include "va_request.h"

static void hevc_fill_sps(struct v4l2_ctrl_hevc_sps *sps,
                          const VAPictureParameterBufferHEVC *pic)
{
    memset(sps, 0, sizeof(*sps));

    sps->pic_width_in_luma_samples = pic->pic_width_in_luma_samples;
    sps->pic_height_in_luma_samples = pic->pic_height_in_luma_samples;
    sps->bit_depth_luma_minus8 = pic->bit_depth_luma_minus8;
    sps->bit_depth_chroma_minus8 = pic->bit_depth_chroma_minus8;
    sps->log2_max_pic_order_cnt_lsb_minus4 = pic->log2_max_pic_order_cnt_lsb_minus4;
    sps->sps_max_dec_pic_buffering_minus1 = pic->sps_max_dec_pic_buffering_minus1;
    sps->log2_min_luma_coding_block_size_minus3 = pic->log2_min_luma_coding_block_size_minus3;
    sps->log2_diff_max_min_luma_coding_block_size = pic->log2_diff_max_min_luma_coding_block_size;
    sps->log2_min_luma_transform_block_size_minus2 = pic->log2_min_transform_block_size_minus2;
    sps->log2_diff_max_min_luma_transform_block_size = pic->log2_diff_max_min_transform_block_size;
    sps->max_transform_hierarchy_depth_inter = pic->max_transform_hierarchy_depth_inter;
    sps->max_transform_hierarchy_depth_intra = pic->max_transform_hierarchy_depth_intra;
    /* VA-API has no sub-layer ordering info; a value consistent with the
     * DPB size is all the kernel needs. */
    sps->sps_max_num_reorder_pics = pic->pic_fields.bits.NoPicReorderingFlag ? 0 : pic->sps_max_dec_pic_buffering_minus1;
    sps->num_short_term_ref_pic_sets = pic->num_short_term_ref_pic_sets;
    sps->num_long_term_ref_pics_sps = pic->num_long_term_ref_pic_sps;
    sps->chroma_format_idc = pic->pic_fields.bits.chroma_format_idc;

    if (pic->pic_fields.bits.pcm_enabled_flag) {
        sps->pcm_sample_bit_depth_luma_minus1 = pic->pcm_sample_bit_depth_luma_minus1;
        sps->pcm_sample_bit_depth_chroma_minus1 = pic->pcm_sample_bit_depth_chroma_minus1;
        sps->log2_min_pcm_luma_coding_block_size_minus3 = pic->log2_min_pcm_luma_coding_block_size_minus3;
        sps->log2_diff_max_min_pcm_luma_coding_block_size = pic->log2_diff_max_min_pcm_luma_coding_block_size;
    }
    /* PCM disabled: keep the PCM fields zeroed — VA hands back invalid
     * marker values (0xff / -3) for them in that case. */

    sps->flags = pic->pic_fields.bits.separate_colour_plane_flag ? V4L2_HEVC_SPS_FLAG_SEPARATE_COLOUR_PLANE : 0;
    if (pic->pic_fields.bits.pcm_enabled_flag)
        sps->flags |= V4L2_HEVC_SPS_FLAG_PCM_ENABLED;
    if (pic->pic_fields.bits.pcm_loop_filter_disabled_flag)
        sps->flags |= V4L2_HEVC_SPS_FLAG_PCM_LOOP_FILTER_DISABLED;
    if (pic->pic_fields.bits.scaling_list_enabled_flag)
        sps->flags |= V4L2_HEVC_SPS_FLAG_SCALING_LIST_ENABLED;
    if (pic->pic_fields.bits.amp_enabled_flag)
        sps->flags |= V4L2_HEVC_SPS_FLAG_AMP_ENABLED;
    if (pic->pic_fields.bits.strong_intra_smoothing_enabled_flag)
        sps->flags |= V4L2_HEVC_SPS_FLAG_STRONG_INTRA_SMOOTHING_ENABLED;
    if (pic->slice_parsing_fields.bits.sps_temporal_mvp_enabled_flag)
        sps->flags |= V4L2_HEVC_SPS_FLAG_SPS_TEMPORAL_MVP_ENABLED;
    if (pic->slice_parsing_fields.bits.long_term_ref_pics_present_flag)
        sps->flags |= V4L2_HEVC_SPS_FLAG_LONG_TERM_REF_PICS_PRESENT;
    if (pic->slice_parsing_fields.bits.sample_adaptive_offset_enabled_flag)
        sps->flags |= V4L2_HEVC_SPS_FLAG_SAMPLE_ADAPTIVE_OFFSET;
}

VAStatus vareq_hevc_fill_pps(struct v4l2_ctrl_hevc_pps *pps,
                             const VAPictureParameterBufferHEVC *pic)
{
    if (!pps || !pic) return VA_STATUS_ERROR_INVALID_PARAMETER;
    memset(pps, 0, sizeof(*pps));

    pps->num_extra_slice_header_bits = pic->num_extra_slice_header_bits;
    pps->num_ref_idx_l0_default_active_minus1 = pic->num_ref_idx_l0_default_active_minus1;
    pps->num_ref_idx_l1_default_active_minus1 = pic->num_ref_idx_l1_default_active_minus1;
    pps->init_qp_minus26 = pic->init_qp_minus26;
    pps->diff_cu_qp_delta_depth = pic->diff_cu_qp_delta_depth;
    pps->pps_cb_qp_offset = pic->pps_cb_qp_offset;
    pps->pps_cr_qp_offset = pic->pps_cr_qp_offset;
    pps->log2_parallel_merge_level_minus2 = pic->log2_parallel_merge_level_minus2;
    pps->pps_beta_offset_div2 = pic->pps_beta_offset_div2;
    pps->pps_tc_offset_div2 = pic->pps_tc_offset_div2;

    pps->flags = 0;
    if (pic->pic_fields.bits.sign_data_hiding_enabled_flag)
        pps->flags |= V4L2_HEVC_PPS_FLAG_SIGN_DATA_HIDING_ENABLED;
    if (pic->pic_fields.bits.constrained_intra_pred_flag)
        pps->flags |= V4L2_HEVC_PPS_FLAG_CONSTRAINED_INTRA_PRED;
    if (pic->pic_fields.bits.transform_skip_enabled_flag)
        pps->flags |= V4L2_HEVC_PPS_FLAG_TRANSFORM_SKIP_ENABLED;
    if (pic->pic_fields.bits.cu_qp_delta_enabled_flag)
        pps->flags |= V4L2_HEVC_PPS_FLAG_CU_QP_DELTA_ENABLED;
    if (pic->pic_fields.bits.weighted_pred_flag)
        pps->flags |= V4L2_HEVC_PPS_FLAG_WEIGHTED_PRED;
    if (pic->pic_fields.bits.weighted_bipred_flag)
        pps->flags |= V4L2_HEVC_PPS_FLAG_WEIGHTED_BIPRED;
    if (pic->pic_fields.bits.transquant_bypass_enabled_flag)
        pps->flags |= V4L2_HEVC_PPS_FLAG_TRANSQUANT_BYPASS_ENABLED;
    if (pic->pic_fields.bits.tiles_enabled_flag)
        pps->flags |= V4L2_HEVC_PPS_FLAG_TILES_ENABLED;
    if (pic->pic_fields.bits.entropy_coding_sync_enabled_flag)
        pps->flags |= V4L2_HEVC_PPS_FLAG_ENTROPY_CODING_SYNC_ENABLED;
    if (pic->pic_fields.bits.loop_filter_across_tiles_enabled_flag)
        pps->flags |= V4L2_HEVC_PPS_FLAG_LOOP_FILTER_ACROSS_TILES_ENABLED;
    if (pic->pic_fields.bits.pps_loop_filter_across_slices_enabled_flag)
        pps->flags |= V4L2_HEVC_PPS_FLAG_PPS_LOOP_FILTER_ACROSS_SLICES_ENABLED;
    if (pic->slice_parsing_fields.bits.cabac_init_present_flag)
        pps->flags |= V4L2_HEVC_PPS_FLAG_CABAC_INIT_PRESENT;
    if (pic->slice_parsing_fields.bits.output_flag_present_flag)
        pps->flags |= V4L2_HEVC_PPS_FLAG_OUTPUT_FLAG_PRESENT;
    if (pic->slice_parsing_fields.bits.pps_slice_chroma_qp_offsets_present_flag)
        pps->flags |= V4L2_HEVC_PPS_FLAG_PPS_SLICE_CHROMA_QP_OFFSETS_PRESENT;
    if (pic->slice_parsing_fields.bits.dependent_slice_segments_enabled_flag)
        pps->flags |= V4L2_HEVC_PPS_FLAG_DEPENDENT_SLICE_SEGMENT_ENABLED;
    if (pic->slice_parsing_fields.bits.deblocking_filter_override_enabled_flag)
        pps->flags |= V4L2_HEVC_PPS_FLAG_DEBLOCKING_FILTER_OVERRIDE_ENABLED;
    if (pic->slice_parsing_fields.bits.pps_disable_deblocking_filter_flag)
        pps->flags |= V4L2_HEVC_PPS_FLAG_PPS_DISABLE_DEBLOCKING_FILTER;
    if (pic->slice_parsing_fields.bits.lists_modification_present_flag)
        pps->flags |= V4L2_HEVC_PPS_FLAG_LISTS_MODIFICATION_PRESENT;
    if (pic->slice_parsing_fields.bits.slice_segment_header_extension_present_flag)
        pps->flags |= V4L2_HEVC_PPS_FLAG_SLICE_SEGMENT_HEADER_EXTENSION_PRESENT;

    if (pic->pic_fields.bits.tiles_enabled_flag) {
        unsigned log2_ctb = pic->log2_min_luma_coding_block_size_minus3 + 3 + pic->log2_diff_max_min_luma_coding_block_size;
        if (log2_ctb < 4 || log2_ctb > 6 || pic->num_tile_columns_minus1 > 19 || pic->num_tile_rows_minus1 > 21 ||
            !pic->pic_width_in_luma_samples || !pic->pic_height_in_luma_samples) return VA_STATUS_ERROR_INVALID_PARAMETER;
        pps->num_tile_columns_minus1 = pic->num_tile_columns_minus1;
        pps->num_tile_rows_minus1 = pic->num_tile_rows_minus1;
        unsigned lengths[] = {(pic->pic_width_in_luma_samples + (1u << log2_ctb) - 1) >> log2_ctb,
            (pic->pic_height_in_luma_samples + (1u << log2_ctb) - 1) >> log2_ctb};
        unsigned counts[] = {pic->num_tile_columns_minus1 + 1u, pic->num_tile_rows_minus1 + 1u};
        const uint16_t *input[] = {pic->column_width_minus1, pic->row_height_minus1};
        uint8_t *output[] = {pps->column_width_minus1, pps->row_height_minus1};
        int uniform = 1;
        for (unsigned axis = 0; axis < 2; axis++) {
            unsigned used = 0;
            if (counts[axis] > lengths[axis]) return VA_STATUS_ERROR_INVALID_PARAMETER;
            for (unsigned i = 0; i < counts[axis]; i++) {
                unsigned width = i + 1 < counts[axis] ? input[axis][i] + 1u : lengths[axis] - used;
                if (!width || width > lengths[axis] - used) return VA_STATUS_ERROR_INVALID_PARAMETER;
                if (width > 256) return VA_STATUS_ERROR_UNIMPLEMENTED;
                output[axis][i] = width - 1;
                if (width != (i + 1) * lengths[axis] / counts[axis] - i * lengths[axis] / counts[axis]) uniform = 0;
                used += width;
            }
        }
        if (uniform) pps->flags |= V4L2_HEVC_PPS_FLAG_UNIFORM_SPACING;
    } else pps->flags |= V4L2_HEVC_PPS_FLAG_UNIFORM_SPACING; /* preserve the verified untiled kernel convention */
    return VA_STATUS_SUCCESS;
}

/*
 * Find the V4L2 timestamp assigned to a VA-API HEVC reference picture.
 */
static uint64_t hevc_find_ref_timestamp(struct vareq_driver_data *dd,
                                        VASurfaceID picture_id)
{
    struct vareq_surface *surf;

    if (!dd || picture_id == VA_INVALID_SURFACE)
        return 0;
    surf = vareq_surface_by_id(dd, picture_id);
    if (surf)
        return surf->timestamp;
    return 0;
}

void vareq_hevc_fill_decode_params(struct v4l2_ctrl_hevc_decode_params *dec,
                                    const VAPictureParameterBufferHEVC *pic,
                                    struct vareq_driver_data *dd)
{
    memset(dec, 0, sizeof(*dec));

    dec->pic_order_cnt_val = pic->CurrPic.pic_order_cnt;
    dec->short_term_ref_pic_set_size = pic->st_rps_bits;

    dec->flags = 0;
    if (pic->slice_parsing_fields.bits.IdrPicFlag)
        dec->flags |= V4L2_HEVC_DECODE_PARAM_FLAG_IDR_PIC;
    if (pic->slice_parsing_fields.bits.RapPicFlag)
        dec->flags |= V4L2_HEVC_DECODE_PARAM_FLAG_IRAP_PIC;

    /* VA explicitly marks current reference sets. Unmarked DPB entries
     * are retained for future pictures and must not enter the current RPS. */
    int nact = 0, nb = 0, na = 0, nl = 0;

    for (int i = 0; i < 15; i++) {
        const VAPictureHEVC *ref = &pic->ReferenceFrames[i];
        if ((ref->flags & VA_PICTURE_HEVC_INVALID) || ref->picture_id == VA_INVALID_SURFACE)
            continue;

        dec->dpb[nact].pic_order_cnt_val = ref->pic_order_cnt;
        dec->dpb[nact].timestamp = hevc_find_ref_timestamp(dd, ref->picture_id);
        if (ref->flags & (VA_PICTURE_HEVC_LONG_TERM_REFERENCE | VA_PICTURE_HEVC_RPS_LT_CURR))
            dec->dpb[nact].flags |= V4L2_HEVC_DPB_ENTRY_LONG_TERM_REFERENCE;
        dec->dpb[nact].field_pic = !!(ref->flags & VA_PICTURE_HEVC_FIELD_PIC);
        if (ref->flags & VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE)
            dec->poc_st_curr_before[nb++] = nact;
        else if (ref->flags & VA_PICTURE_HEVC_RPS_ST_CURR_AFTER)
            dec->poc_st_curr_after[na++] = nact;
        else if (ref->flags & VA_PICTURE_HEVC_RPS_LT_CURR)
            dec->poc_lt_curr[nl++] = nact;
        nact++;
    }

    dec->num_active_dpb_entries = nact;
    dec->num_poc_st_curr_before = nb;
    dec->num_poc_st_curr_after = na;
    dec->num_poc_lt_curr = nl;
    /* PocStCurrBefore is decreasing, PocStCurrAfter increasing POC order.
     * VA's ReferenceFrames array is a DPB, not an ordered reference list. */
    for (unsigned list = 0; list < 2; list++) {
        uint8_t *indices = list ? dec->poc_st_curr_after : dec->poc_st_curr_before;
        unsigned count = list ? na : nb;
        for (unsigned i = 1; i < count; i++) {
            uint8_t index = indices[i]; unsigned j = i;
            while (j && (list ? dec->dpb[indices[j - 1]].pic_order_cnt_val > dec->dpb[index].pic_order_cnt_val :
                               dec->dpb[indices[j - 1]].pic_order_cnt_val < dec->dpb[index].pic_order_cnt_val)) {
                indices[j] = indices[j - 1]; j--;
            }
            indices[j] = index;
        }
    }
}

/* PPS identity precedes all SPS-dependent slice syntax. Its ue(v) is at
 * most13 bits,so no emulation-prevention byte can occur in this prefix. */
VAStatus vareq_hevc_parse_headers(const uint8_t *data, size_t size, const VAPictureParameterBufferHEVC *pic,
    struct v4l2_ctrl_hevc_pps *pps, struct v4l2_ctrl_hevc_decode_params *dec)
{
    if (!pic || !pps) return VA_STATUS_ERROR_INVALID_PARAMETER;
    const uint8_t *nal; size_t offset = 0, length; unsigned slices = 0, pps_id = 0, identity = 0, no_output = 0;
    int next;
    while ((next = vareq_annexb_next(data, size, &offset, &nal, &length)) == 1) {
        unsigned prefix = nal[2] == 1 ? 3 : 4;
        if (length < prefix + 3) return VA_STATUS_ERROR_INVALID_PARAMETER;
        nal += prefix; length -= prefix;
        unsigned type = (nal[0] >> 1) & 63, temporal = nal[1] & 7, layer = ((nal[0] & 1) << 5) | (nal[1] >> 3);
        unsigned irap = type >= 16 && type <= 21, idr = type == 19 || type == 20;
        if ((nal[0] & 128) || !temporal || layer || (irap && temporal != 1) || (type > 9 && !irap) ||
            irap != pic->slice_parsing_fields.bits.RapPicFlag || idr != pic->slice_parsing_fields.bits.IdrPicFlag ||
            (slices && identity != ((type << 3) | temporal)) || !!(nal[2] & 128) != !slices)
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        unsigned current_no_output = irap && (nal[2] & 64);
        if (slices && current_no_output != no_output) return VA_STATUS_ERROR_INVALID_PARAMETER;
        unsigned bit = irap ? 2 : 1, zeros = 0;
        while (2 + bit / 8 < length && !(nal[2 + bit / 8] & (128u >> (bit % 8)))) { bit++; if (++zeros > 6) return VA_STATUS_ERROR_INVALID_PARAMETER; }
        if (2 + bit / 8 >= length) return VA_STATUS_ERROR_INVALID_PARAMETER;
        bit++; unsigned suffix = 0;
        for (unsigned i = 0; i < zeros; i++, bit++) {
            if (2 + bit / 8 >= length) return VA_STATUS_ERROR_INVALID_PARAMETER;
            suffix = (suffix << 1) | !!(nal[2 + bit / 8] & (128u >> (bit % 8)));
        }
        unsigned current = (1u << zeros) - 1 + suffix;
        if (current > 63 || (slices && current != pps_id)) return VA_STATUS_ERROR_INVALID_PARAMETER;
        pps_id = current; identity = (type << 3) | temporal; no_output = current_no_output; slices++;
    }
    if (next < 0 || !slices) return VA_STATUS_ERROR_INVALID_PARAMETER;
    pps->pic_parameter_set_id = pps_id;
    if (dec) {
        dec->flags &= ~V4L2_HEVC_DECODE_PARAM_FLAG_NO_OUTPUT_OF_PRIOR;
        if (no_output) dec->flags |= V4L2_HEVC_DECODE_PARAM_FLAG_NO_OUTPUT_OF_PRIOR;
    }
    return VA_STATUS_SUCCESS;
}

/*
 * HEVC decode pipeline — translate and submit
 */
VAStatus vareq_hevc_translate(struct vareq_context *ctx,
                               struct vareq_buffer **buffers,
                               int num_buffers)
{
    VAPictureParameterBufferHEVC *pic_param = NULL;
    VAQMatrixBufferHEVC *qm = NULL;

    struct vareq_collected cb;
    int n_slice_data = 0;

    vareq_collect_decode_buffers(buffers, num_buffers, &cb);
    pic_param = cb.pic;
    qm = cb.iq;
    n_slice_data = cb.n_slice_datas;

    if (!pic_param) {
        fprintf(stderr, "v4l2request: HEVC decode missing picture params\n");
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    if (n_slice_data == 0) {
        fprintf(stderr, "v4l2request: HEVC decode missing slice data\n");
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    if (pic_param->st_rps_bits > UINT16_MAX || pic_param->num_short_term_ref_pic_sets > 64 ||
        pic_param->num_long_term_ref_pic_sps > 32 || pic_param->sps_max_dec_pic_buffering_minus1 > 15)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    unsigned total_current = 0;
    for (unsigned i = 0; i < 15; i++) {
        const VAPictureHEVC *ref = &pic_param->ReferenceFrames[i];
        if (ref->flags & VA_PICTURE_HEVC_INVALID) continue;
        unsigned current = ref->flags & (VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE | VA_PICTURE_HEVC_RPS_ST_CURR_AFTER | VA_PICTURE_HEVC_RPS_LT_CURR);
        if (current & (current - 1)) return VA_STATUS_ERROR_INVALID_PARAMETER;
        for (unsigned j = 0; j < i; j++)
            if (!(pic_param->ReferenceFrames[j].flags & VA_PICTURE_HEVC_INVALID) &&
                pic_param->ReferenceFrames[j].picture_id == ref->picture_id) return VA_STATUS_ERROR_INVALID_PARAMETER;
        /* Every populated entry is sent to the kernel DPB, including ST/LT
         * references kept for future pictures. Validate its retained storage;
         * an unresolved timestamp zero must not alias the valid first frame. */
        VAStatus status = vareq_validate_reference(ctx, ref->picture_id);
        if (status != VA_STATUS_SUCCESS) return status;
        if (!current) continue;
        if (++total_current > 8 ||
            ((current & (VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE | VA_PICTURE_HEVC_RPS_ST_CURR_AFTER)) &&
             (ref->flags & VA_PICTURE_HEVC_LONG_TERM_REFERENCE)) ||
            (current == VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE && ref->pic_order_cnt >= pic_param->CurrPic.pic_order_cnt) ||
            (current == VA_PICTURE_HEVC_RPS_ST_CURR_AFTER && ref->pic_order_cnt <= pic_param->CurrPic.pic_order_cnt))
            return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    size_t total = 0;
    VAStatus packet_status = vareq_packetize_slices(VAREQ_CODEC_HEVC, buffers, num_buffers, NULL, UINT32_MAX, &total);
    if (packet_status != VA_STATUS_SUCCESS) return packet_status;

    struct v4l2_ctrl_hevc_sps sps;
    struct v4l2_ctrl_hevc_pps pps;
    struct v4l2_ctrl_hevc_decode_params dec;

    hevc_fill_sps(&sps, pic_param);
    packet_status = vareq_hevc_fill_pps(&pps, pic_param);
    if (packet_status != VA_STATUS_SUCCESS) return packet_status;
    vareq_hevc_fill_decode_params(&dec, pic_param, ctx->driver_data);
    if (ctx->n_free_out <= 0) return VA_STATUS_ERROR_OPERATION_FAILED;
    int header_index = ctx->free_out_bufs[ctx->n_free_out - 1];
    if (header_index < 0 || header_index >= VAREQ_NUM_OUTPUT_BUFS || !ctx->output_buf_ptr[header_index]) return VA_STATUS_ERROR_OPERATION_FAILED;
    int temporary = total > ctx->output_buf_size;
    uint8_t *header_data = temporary ? malloc(total) : ctx->output_buf_ptr[header_index];
    if (!header_data) return VA_STATUS_ERROR_ALLOCATION_FAILED;
    packet_status = vareq_packetize_slices(VAREQ_CODEC_HEVC, buffers, num_buffers, header_data,
        temporary ? total : ctx->output_buf_size, &total);
    if (packet_status == VA_STATUS_SUCCESS) packet_status = vareq_hevc_parse_headers(header_data, total, pic_param, &pps, &dec);
    if (temporary) free(header_data);
    if (packet_status != VA_STATUS_SUCCESS) return packet_status;
    vareq_decode_trace(ctx, "hevc-header", 0, pps.pic_parameter_set_id);

    int request_fd = ctx->request_fd;
    int v4l2_fd = ctx->v4l2_fd;

    if (request_fd < 0) {
        fprintf(stderr, "v4l2request: HEVC no request fd available\n");
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    /* Set HEVC SPS — as a GLOBAL control: the rkvdec kernel rejects the
     * request-scoped variant of this control with EINVAL. */
    struct v4l2_ext_control sps_ctrl = { 0 };
    sps_ctrl.id = V4L2_CID_STATELESS_HEVC_SPS;
    sps_ctrl.p_hevc_sps = &sps;
    sps_ctrl.size = sizeof(sps);

    struct v4l2_ext_controls sps_ctrls = { 0 };
    sps_ctrls.controls = &sps_ctrl;
    sps_ctrls.count = 1;

    /* Sequence-level control: resubmit only when the payload changed. */
    _Static_assert(sizeof(sps) <= sizeof(ctx->g_ctrl_payload), "grow cache");
    if (!ctx->g_ctrl_valid ||
        memcmp(ctx->g_ctrl_payload, &sps, sizeof(sps)) != 0) {
        if (vareq_set_global_controls(v4l2_fd, &sps_ctrls) < 0) {
            fprintf(stderr, "v4l2request: failed to set HEVC SPS\n");
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        memcpy(ctx->g_ctrl_payload, &sps, sizeof(sps));
        ctx->g_ctrl_valid = 1;
    }

    {
        uint32_t cap = vareq_capture_fourcc_from_sps(
            pic_param->bit_depth_luma_minus8,
            pic_param->pic_fields.bits.chroma_format_idc);
        if (vareq_ensure_capture(ctx,
                                  pic_param->pic_width_in_luma_samples,
                                  pic_param->pic_height_in_luma_samples,
                                  cap) < 0) {
            fprintf(stderr, "v4l2request: HEVC capture reconfig failed\n");
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
    }

    /* The kernel accepts STREAMON only after it has the SPS */
    if (!ctx->streamed) {
        if (vareq_streamon(v4l2_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) < 0 ||
            vareq_streamon(v4l2_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) < 0) {
            fprintf(stderr, "v4l2request: HEVC STREAMON failed\n");
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        ctx->streamed = 1;
    }

    /* Set HEVC PPS */
    struct v4l2_ext_control pps_ctrl = { 0 };
    pps_ctrl.id = V4L2_CID_STATELESS_HEVC_PPS;
    pps_ctrl.p_hevc_pps = &pps;
    pps_ctrl.size = sizeof(pps);

    struct v4l2_ext_controls pps_ctrls = { 0 };
    pps_ctrls.controls = &pps_ctrl;
    pps_ctrls.count = 1;

    if (vareq_set_request_controls(request_fd, v4l2_fd, &pps_ctrls) < 0) {
        fprintf(stderr, "v4l2request: failed to set HEVC PPS\n");
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    /* Set HEVC scaling matrix — the kernel expects it every frame; flat 16s
     * are the H.265 default when the stream carries no custom lists. */
    struct v4l2_ctrl_hevc_scaling_matrix sm;

    /* Streams with scaling_list_enabled_flag deliver VAQMatrixBufferHEVC;
     * the VA and V4L2 layouts are identical. Flat 16 is only the fallback
     * when the buffer is absent. */
    if (qm)
        memcpy(&sm, qm, sizeof(sm));
    else
        memset(&sm, 16, sizeof(sm));

    struct v4l2_ext_control sm_ctrl = { 0 };
    sm_ctrl.id = V4L2_CID_STATELESS_HEVC_SCALING_MATRIX;
    sm_ctrl.p_hevc_scaling_matrix = &sm;
    sm_ctrl.size = sizeof(sm);

    struct v4l2_ext_controls sm_ctrls = { 0 };
    sm_ctrls.controls = &sm_ctrl;
    sm_ctrls.count = 1;

    if (vareq_set_request_controls(request_fd, v4l2_fd, &sm_ctrls) < 0) {
        fprintf(stderr, "v4l2request: failed to set HEVC scaling matrix\n");
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    /* Set HEVC decode params (includes RPS for VDPU381) */
    struct v4l2_ext_control dec_ctrl = { 0 };
    dec_ctrl.id = V4L2_CID_STATELESS_HEVC_DECODE_PARAMS;
    dec_ctrl.p_hevc_decode_params = &dec;
    dec_ctrl.size = sizeof(dec);

    struct v4l2_ext_controls dec_ctrls = { 0 };
    dec_ctrls.controls = &dec_ctrl;
    dec_ctrls.count = 1;

    if (vareq_set_request_controls(request_fd, v4l2_fd, &dec_ctrls) < 0) {
        fprintf(stderr, "v4l2request: failed to set HEVC decode params\n");
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    /*
     * Synchronous decode pipeline — mirrors the H.264 path: pop buffers
     * from the pools, queue, submit, wait, and hand the decoded frame to
     * the target surface.
     */
    if (ctx->n_free_out == 0) {
        fprintf(stderr, "v4l2request: HEVC no free output buffer\n");
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    int out_buf_idx = ctx->free_out_bufs[--ctx->n_free_out];
    if (out_buf_idx < 0 || out_buf_idx >= VAREQ_NUM_OUTPUT_BUFS || !ctx->output_buf_ptr[out_buf_idx]) {
        vareq_out_pool_push(ctx, out_buf_idx);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    uint64_t timestamp = ctx->current_surface ? ctx->current_surface->timestamp : 0;

    packet_status = vareq_packetize_slices(VAREQ_CODEC_HEVC, buffers, num_buffers,
        (uint8_t *)ctx->output_buf_ptr[out_buf_idx], ctx->output_buf_size, &total);
    if (packet_status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "v4l2request: HEVC slice packetization failed: %d\n", packet_status);
        vareq_out_pool_push(ctx, out_buf_idx);
        return packet_status;
    }

    /* On failure decode_submit resets both queues — nothing stays queued
     * in the kernel and the pools are rebuilt. */
    int done_cap = vareq_decode_submit(ctx, out_buf_idx, (uint32_t)total, timestamp);
    if (done_cap == -2)
        return VA_STATUS_ERROR_DECODING_ERROR;
    if (done_cap < 0)
        return VA_STATUS_ERROR_OPERATION_FAILED;

    struct vareq_surface *surf = ctx->current_surface;
    if (surf) {
        if (vareq_surface_pull_capture(ctx, surf, done_cap) < 0) {
            fprintf(stderr, "v4l2request: HEVC pull capture failed\n");
            if (!vareq_surface_capture_matches(ctx, surf) || surf->buf_index != done_cap) vareq_cap_pool_push(ctx, done_cap);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
    } else {
        vareq_cap_pool_push(ctx, done_cap);
    }

    return VA_STATUS_SUCCESS;
}

const int vareq_has_hevc = 1;
