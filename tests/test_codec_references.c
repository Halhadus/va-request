#include <stdio.h>
#include <string.h>
#include "va_request.h"

static int failures;
static void expect(int ok, const char *tag)
{
    if (!ok) { fprintf(stderr, "FAIL %s\n", tag); failures++; }
}
static void h264_references(void)
{
    struct vareq_driver_data dd = {0};
    struct vareq_surface s = {.timestamp = 9000, .h264_frame_num = 14, .h264_frame_num_valid = 1};
    VAPictureParameterBufferH264 pic = {0};
    VASliceParameterBufferH264 slice = {.slice_type = 0};
    struct v4l2_ctrl_h264_decode_params dec;
    struct v4l2_ctrl_h264_sps sps;
    for (int i = 0; i < 16; i++) pic.ReferenceFrames[i].flags = VA_PICTURE_H264_INVALID;
    dd.surfaces[1] = &s;
    pic.ReferenceFrames[0] = (VAPictureH264){.picture_id = 1, .frame_idx = 3,
        .flags = VA_PICTURE_H264_LONG_TERM_REFERENCE | VA_PICTURE_H264_BOTTOM_FIELD};
    pic.CurrPic.flags = VA_PICTURE_H264_BOTTOM_FIELD;
    pic.pic_fields.bits.field_pic_flag = 1;
    h264_fill_decode_params(&dec, &pic, &dd, &slice);
    int found = 0;
    for (int i = 0; i < 16; i++) {
        if (!(dec.dpb[i].flags & V4L2_H264_DPB_ENTRY_FLAG_VALID)) continue;
        found++;
        expect((dec.dpb[i].flags & (V4L2_H264_DPB_ENTRY_FLAG_ACTIVE | V4L2_H264_DPB_ENTRY_FLAG_LONG_TERM |
               V4L2_H264_DPB_ENTRY_FLAG_FIELD)) == (V4L2_H264_DPB_ENTRY_FLAG_ACTIVE | V4L2_H264_DPB_ENTRY_FLAG_LONG_TERM |
               V4L2_H264_DPB_ENTRY_FLAG_FIELD), "h264-long-term-bottom-field-active");
        expect(dec.dpb[i].fields == V4L2_H264_BOTTOM_FIELD_REF && dec.dpb[i].reference_ts == 9000,
               "h264-bottom-reference-polarity");
        expect(dec.dpb[i].frame_num == 3 && dec.dpb[i].pic_num == 7, "h264-long-term-frame-index-and-same-parity-pic-num");
    }
    expect(found == 1 && (dec.flags & V4L2_H264_DECODE_PARAM_FLAG_BOTTOM_FIELD), "h264-current-bottom-field");
    pic.picture_height_in_mbs_minus1 = 67;
    h264_fill_sps(&sps, &pic, VAProfileH264High);
    expect(sps.pic_height_in_map_units_minus1 == 33, "h264-interlaced-map-units");
    pic.seq_fields.bits.frame_mbs_only_flag = 1;
    h264_fill_sps(&sps, &pic, VAProfileH264High);
    expect(sps.pic_height_in_map_units_minus1 == 67, "h264-progressive-map-units");
    pic.pic_fields.bits.field_pic_flag = 0; pic.CurrPic.flags = 0; pic.frame_num = 1;
    pic.ReferenceFrames[0].flags = VA_PICTURE_H264_SHORT_TERM_REFERENCE; pic.ReferenceFrames[0].frame_idx = 14;
    h264_fill_decode_params(&dec, &pic, &dd, &slice);
    for (unsigned i = 0; i < 16; i++) if (dec.dpb[i].flags & V4L2_H264_DPB_ENTRY_FLAG_VALID)
        expect(dec.dpb[i].frame_num == 14 && dec.dpb[i].pic_num == (uint32_t)-2, "h264-short-term-pic-num-wrap-keeps-raw-frame-num");
    pic.ReferenceFrames[0].flags = VA_PICTURE_H264_LONG_TERM_REFERENCE; pic.ReferenceFrames[0].frame_idx = 3;
    h264_fill_decode_params(&dec, &pic, &dd, &slice);
    for (unsigned i = 0; i < 16; i++) if (dec.dpb[i].flags & V4L2_H264_DPB_ENTRY_FLAG_VALID)
        expect(dec.dpb[i].frame_num == 3 && dec.dpb[i].pic_num == 3, "h264-long-term-index-never-wraps-or-borrows-surface-frame-num");
}
static void hevc_references(void)
{
    VAPictureParameterBufferHEVC pic = {0};
    struct v4l2_ctrl_hevc_decode_params dec;
    for (int i = 0; i < 15; i++) pic.ReferenceFrames[i].flags = VA_PICTURE_HEVC_INVALID;
    pic.CurrPic.pic_order_cnt = 10;
    /* Unused references on both sides and a future long-term reference. */
    pic.ReferenceFrames[0] = (VAPictureHEVC){.picture_id = 1, .pic_order_cnt = 2};
    pic.ReferenceFrames[1] = (VAPictureHEVC){.picture_id = 2, .pic_order_cnt = 8, .flags = VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE};
    pic.ReferenceFrames[2] = (VAPictureHEVC){.picture_id = 3, .pic_order_cnt = 20};
    pic.ReferenceFrames[3] = (VAPictureHEVC){.picture_id = 4, .pic_order_cnt = 12, .flags = VA_PICTURE_HEVC_RPS_ST_CURR_AFTER};
    pic.ReferenceFrames[4] = (VAPictureHEVC){.picture_id = 5, .pic_order_cnt = 1, .flags = VA_PICTURE_HEVC_LONG_TERM_REFERENCE};
    pic.ReferenceFrames[5] = (VAPictureHEVC){.picture_id = 6, .pic_order_cnt = 0,
        .flags = VA_PICTURE_HEVC_LONG_TERM_REFERENCE | VA_PICTURE_HEVC_RPS_LT_CURR};
    vareq_hevc_fill_decode_params(&dec, &pic, NULL);
    expect(dec.num_active_dpb_entries == 6 && dec.num_poc_st_curr_before == 1 && dec.poc_st_curr_before[0] == 1,
           "hevc-only-marked-st-before");
    expect(dec.num_poc_st_curr_after == 1 && dec.poc_st_curr_after[0] == 3, "hevc-only-marked-st-after");
    expect(dec.num_poc_lt_curr == 1 && dec.poc_lt_curr[0] == 5 &&
           (dec.dpb[4].flags & V4L2_HEVC_DPB_ENTRY_LONG_TERM_REFERENCE), "hevc-retains-unused-long-term-outside-current-rps");
    pic.ReferenceFrames[0] = (VAPictureHEVC){.picture_id = 1, .pic_order_cnt = -4, .flags = VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE};
    pic.ReferenceFrames[2] = (VAPictureHEVC){.picture_id = 3, .pic_order_cnt = 16, .flags = VA_PICTURE_HEVC_RPS_ST_CURR_AFTER};
    pic.ReferenceFrames[6] = (VAPictureHEVC){.picture_id = 7, .pic_order_cnt = 4, .flags = VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE};
    pic.ReferenceFrames[5].flags = VA_PICTURE_HEVC_RPS_LT_CURR | VA_PICTURE_HEVC_FIELD_PIC | VA_PICTURE_HEVC_BOTTOM_FIELD;
    vareq_hevc_fill_decode_params(&dec, &pic, NULL);
    expect(dec.num_poc_st_curr_before == 3 && dec.poc_st_curr_before[0] == 1 && dec.poc_st_curr_before[1] == 6 &&
        dec.poc_st_curr_before[2] == 0 && dec.num_poc_st_curr_after == 2 && dec.poc_st_curr_after[0] == 3 &&
        dec.poc_st_curr_after[1] == 2, "hevc-current-short-term-sets-use-poc-order-not-dpb-order");
    expect(dec.dpb[5].field_pic == 1 && (dec.dpb[5].flags & V4L2_HEVC_DPB_ENTRY_LONG_TERM_REFERENCE) &&
        dec.dpb[0].pic_order_cnt_val == -4 && dec.dpb[6].pic_order_cnt_val == 4,
        "hevc-lt-current-implies-long-term-and-field-metadata-preserves-dpb-indices");
}
static void h264_headers(void)
{
    const uint8_t idr[] = {0x0, 0x0, 0x1, 0x65, 0xb2, 0x1, 0x4e, 0x80}, wrap[] = {0x0, 0x0, 0x1, 0x41, 0xb8, 0x39, 0x6a, 0x46, 0x8a, 0x43, 0xac}, poc1[] = {0x0, 0x0, 0x1, 0x1, 0xb8, 0x32, 0x40}, field[] = {0x0, 0x0, 0x1, 0x65, 0xb8, 0x74, 0x60}, weighted[] = {0x0, 0x0, 0x1, 0x41, 0xe0, 0x5e, 0x89, 0xd6, 0xaa, 0x80}, escaped[] = {0x0, 0x0, 0x1, 0x65, 0xb8, 0x0, 0x4, 0x0, 0x0, 0x80};
    VAPictureParameterBufferH264 pic = {0}; VASliceParameterBufferH264 slice = {.slice_type = 2};
    struct v4l2_ctrl_h264_decode_params dec = {0}; struct v4l2_ctrl_h264_pps pps = {0};
    pic.seq_fields.bits.frame_mbs_only_flag = 1; pic.seq_fields.bits.chroma_format_idc = 1;
    pic.pic_fields.bits.reference_pic_flag = 1;
    expect(vareq_h264_parse_header(idr, sizeof(idr), &pic, &slice, &pps, &dec) == VA_STATUS_SUCCESS &&
        pps.pic_parameter_set_id == 3 && dec.idr_pic_id == 4 && dec.pic_order_cnt_lsb == 3 && dec.nal_ref_idc == 3 &&
        (dec.flags & V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC) && dec.pic_order_cnt_bit_size == 4 && dec.dec_ref_pic_marking_bit_size == 2,
        "h264-actual-idr-pps-poc-and-marking");
    pic.pic_fields.bits.pic_order_present_flag = 1;
    expect(vareq_h264_parse_header(wrap, sizeof(wrap), &pic, &slice, &pps, &dec) == VA_STATUS_SUCCESS &&
        !(dec.flags & V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC) && dec.nal_ref_idc == 2 && dec.delta_pic_order_cnt_bottom == -2 &&
        dec.pic_order_cnt_bit_size == 9 && dec.dec_ref_pic_marking_bit_size == 35,
        "h264-non-idr-intra-at-frame-num-zero-with-variable-mmco");
    pic.seq_fields.bits.pic_order_cnt_type = 1; pic.pic_fields.bits.reference_pic_flag = 0;
    memset(&dec, 0, sizeof(dec));
    expect(vareq_h264_parse_header(poc1, sizeof(poc1), &pic, &slice, &pps, &dec) == VA_STATUS_SUCCESS &&
        dec.delta_pic_order_cnt0 == -1 && dec.delta_pic_order_cnt1 == 2 && dec.pic_order_cnt_bit_size == 8 && !dec.dec_ref_pic_marking_bit_size,
        "h264-poc-type-one-deltas-and-non-reference-marking");
    pic.seq_fields.bits.pic_order_cnt_type = 0; pic.seq_fields.bits.frame_mbs_only_flag = 0;
    pic.pic_fields.bits.reference_pic_flag = 1; pic.pic_fields.bits.field_pic_flag = 1; pic.CurrPic.flags = VA_PICTURE_H264_BOTTOM_FIELD;
    expect(vareq_h264_parse_header(field, sizeof(field), &pic, &slice, &pps, &dec) == VA_STATUS_SUCCESS &&
        dec.pic_order_cnt_lsb == 4 && dec.pic_order_cnt_bit_size == 4 && !dec.delta_pic_order_cnt_bottom &&
        !dec.delta_pic_order_cnt0 && !dec.delta_pic_order_cnt1, "h264-bottom-field-header-clears-inapplicable-poc-deltas");
    pic.seq_fields.bits.frame_mbs_only_flag = 1; pic.pic_fields.bits.field_pic_flag = 0; pic.CurrPic.flags = 0;
    pic.pic_fields.bits.pic_order_present_flag = 0; pic.pic_fields.bits.weighted_pred_flag = 1; slice.slice_type = 0;
    expect(vareq_h264_parse_header(weighted, sizeof(weighted), &pic, &slice, &pps, &dec) == VA_STATUS_SUCCESS &&
        dec.dec_ref_pic_marking_bit_size == 1 && dec.pic_order_cnt_lsb == 2, "h264-ref-list-and-pred-weight-syntax-before-marking");
    pic.pic_fields.bits.weighted_pred_flag = 0; slice.slice_type = 2;
    pic.seq_fields.bits.log2_max_frame_num_minus4 = 12; pic.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 = 12;
    expect(vareq_h264_parse_header(escaped, sizeof(escaped), &pic, &slice, &pps, &dec) == VA_STATUS_SUCCESS &&
        dec.pic_order_cnt_bit_size == 16 && dec.dec_ref_pic_marking_bit_size == 2, "h264-emulation-prevention-not-counted-as-rbsp-syntax");
    struct v4l2_ctrl_h264_decode_params saved = dec; struct v4l2_ctrl_h264_pps saved_pps = pps;
    pic.frame_num = 1;
    expect(vareq_h264_parse_header(escaped, sizeof(escaped), &pic, &slice, &pps, &dec) == VA_STATUS_ERROR_INVALID_PARAMETER &&
        !memcmp(&saved, &dec, sizeof(dec)) && !memcmp(&saved_pps, &pps, sizeof(pps)), "h264-header-parameter-mismatch-is-atomic");
    pic.frame_num = 0;
    for (size_t n = 0; n < 5; n++) expect(vareq_h264_parse_header(escaped, n, &pic, &slice, &pps, &dec) == VA_STATUS_ERROR_INVALID_PARAMETER,
        "h264-truncated-header-rejected");
    uint8_t bad[] = {0, 0, 1, 0xe5, 0x80};
    expect(vareq_h264_parse_header(bad, sizeof(bad), &pic, &slice, &pps, &dec) == VA_STATUS_ERROR_INVALID_PARAMETER,
        "h264-forbidden-nal-bit-rejected");
}
static void hevc_tiles(void)
{
    VAPictureParameterBufferHEVC pic = {.pic_width_in_luma_samples = 640, .pic_height_in_luma_samples = 384,
        .log2_min_luma_coding_block_size_minus3 = 0, .log2_diff_max_min_luma_coding_block_size = 3,
        .num_tile_columns_minus1 = 1, .num_tile_rows_minus1 = 1, .column_width_minus1 = {4}, .row_height_minus1 = {2}};
    pic.pic_fields.bits.tiles_enabled_flag = 1;
    struct v4l2_ctrl_hevc_pps pps;
    expect(vareq_hevc_fill_pps(&pps, &pic) == VA_STATUS_SUCCESS && pps.column_width_minus1[1] == 4 &&
        pps.row_height_minus1[1] == 2 && (pps.flags & V4L2_HEVC_PPS_FLAG_UNIFORM_SPACING), "hevc-complete-uniform-tile-grid");
    pic.column_width_minus1[0] = 0;
    expect(vareq_hevc_fill_pps(&pps, &pic) == VA_STATUS_SUCCESS && pps.column_width_minus1[1] == 8 &&
        !(pps.flags & V4L2_HEVC_PPS_FLAG_UNIFORM_SPACING), "hevc-one-ctb-first-column-is-not-unknown-spacing");
    pic.column_width_minus1[0] = 9;
    expect(vareq_hevc_fill_pps(&pps, &pic) == VA_STATUS_ERROR_INVALID_PARAMETER, "hevc-no-room-for-last-tile");
    pic.num_tile_columns_minus1 = 20;
    expect(vareq_hevc_fill_pps(&pps, &pic) == VA_STATUS_ERROR_INVALID_PARAMETER, "hevc-tile-array-limit");
    pic.pic_fields.bits.tiles_enabled_flag = 0;
    expect(vareq_hevc_fill_pps(&pps, &pic) == VA_STATUS_SUCCESS && !pps.num_tile_columns_minus1 &&
        !pps.column_width_minus1[0] && (pps.flags & V4L2_HEVC_PPS_FLAG_UNIFORM_SPACING), "hevc-inactive-tile-tables-ignored-and-verified-untiled-convention-kept");
}
int main(void)
{
    h264_references();
    h264_headers();
    hevc_tiles();
    hevc_references();
    return failures != 0;
}
