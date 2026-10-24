/* RBSP syntax oracles for defaults,POC identity and nonzero HEVC PPS IDs. */
#include <stdio.h>
#include <string.h>
#include "va_v4l2.h"
static int failures;
static void expect(int ok, const char *tag) { if (!ok) { fprintf(stderr, "FAIL headers %s\n", tag); failures++; } }
struct writer { uint8_t data[256]; unsigned bits; };
static void bit(struct writer *w, unsigned v) { if (v) w->data[w->bits / 8] |= 128u >> (w->bits % 8); w->bits++; }
static void fixed(struct writer *w, unsigned v, unsigned n) { while (n) bit(w, (v >> --n) & 1); }
static void ue(struct writer *w, unsigned v) { unsigned n = 0, value = ++v; while (value >>= 1) n++; fixed(w, 0, n); fixed(w, v, n + 1); }
static size_t h264_nal(uint8_t *out, unsigned mb, unsigned type, unsigned pps, unsigned poc, int override, unsigned refs)
{
    struct writer w = {0}; ue(&w, mb); ue(&w, type); ue(&w, pps); fixed(&w, 1, 4); fixed(&w, poc, 4);
    if (type % 5 == 1) bit(&w, 1);
    if (type % 5 != 2) { bit(&w, override); if (override) { ue(&w, refs); if (type % 5 == 1) ue(&w, 0); } bit(&w, 0); if (type % 5 == 1) bit(&w, 0); }
    bit(&w, 0); bit(&w, 1); /* marking mode and a stop bit */
    memcpy(out, (uint8_t[]){0, 0, 1, 0x41}, 4); memcpy(out + 4, w.data, (w.bits + 7) / 8); return 4 + (w.bits + 7) / 8;
}
static void h264(void)
{
    uint8_t data[512]; VAPictureParameterBufferH264 pic = {.frame_num = 1};
    pic.seq_fields.bits.frame_mbs_only_flag = 1; pic.pic_fields.bits.reference_pic_flag = 1;
    VASliceParameterBufferH264 p[2] = {{.slice_type = 0, .num_ref_idx_l0_active_minus1 = 2}, {.slice_type = 0, .first_mb_in_slice = 5}};
    struct vav4l2_buffer params = {.type = VASliceParameterBufferType, .data = p, .size = sizeof(p[0]), .num_elements = 2, .allocated_elements = 2};
    struct vav4l2_buffer *buffers[] = {&params};
    struct v4l2_ctrl_h264_pps pps = {.num_ref_idx_l0_default_active_minus1 = 2}; struct v4l2_ctrl_h264_decode_params dec = {0};
    size_t a = h264_nal(data, 0, 0, 3, 2, 1, 2), b = h264_nal(data + a, 5, 0, 3, 2, 0, 0);
    expect(vav4l2_h264_validate_headers(data, a + b, &pic, buffers, 1, &pps, &dec) == VA_STATUS_SUCCESS &&
        pps.pic_parameter_set_id == 3 && !pps.num_ref_idx_l0_default_active_minus1 && (dec.flags & V4L2_H264_DECODE_PARAM_FLAG_PFRAME),
        "later-non-override-slice-supplies-the-real-PPS-default");
    uint8_t unaligned[2 * (sizeof(p[0]) + 1) + 1];
    memcpy(unaligned + 1, &p[0], sizeof(p[0])); memcpy(unaligned + sizeof(p[0]) + 2, &p[1], sizeof(p[1]));
    params.data = unaligned + 1; params.size++;
    expect(!vav4l2_h264_validate_headers(data, a + b, &pic, buffers, 1, &pps, &dec), "unaligned-parameter-array-stride-is-read-without-undefined-access");
    params.data = p; params.size = sizeof(p[0]);
    struct v4l2_ctrl_h264_pps old_pps = pps; struct v4l2_ctrl_h264_decode_params old_dec = dec;
    b = h264_nal(data + a, 5, 0, 3, 3, 0, 0);
    expect(vav4l2_h264_validate_headers(data, a + b, &pic, buffers, 1, &pps, &dec) == VA_STATUS_ERROR_INVALID_PARAMETER &&
        !memcmp(&pps, &old_pps, sizeof(pps)) && !memcmp(&dec, &old_dec, sizeof(dec)), "second-slice-POC-mismatch-is-atomic");
    b = h264_nal(data + a, 5, 0, 4, 2, 0, 0);
    expect(vav4l2_h264_validate_headers(data, a + b, &pic, buffers, 1, &pps, &dec) == VA_STATUS_ERROR_INVALID_PARAMETER, "second-slice-PPS-mismatch");
    b = h264_nal(data + a, 6, 0, 3, 2, 0, 0);
    expect(vav4l2_h264_validate_headers(data, a + b, &pic, buffers, 1, &pps, &dec) == VA_STATUS_ERROR_INVALID_PARAMETER, "second-slice-first-macroblock-mismatch");
    b = h264_nal(data + a, 5, 0, 3, 2, 0, 0); p[0].num_ref_idx_l0_active_minus1 = 1;
    expect(vav4l2_h264_validate_headers(data, a + b, &pic, buffers, 1, &pps, &dec) == VA_STATUS_ERROR_INVALID_PARAMETER, "override-must-match-VA-effective-reference-count");
    p[0].num_ref_idx_l0_active_minus1 = 2; params.num_elements = 1;
    expect(vav4l2_h264_validate_headers(data, a + b, &pic, buffers, 1, &pps, &dec) == VA_STATUS_ERROR_INVALID_PARAMETER, "extra-hidden-NAL-is-not-submitted");
    params.num_elements = 2; p[0].slice_type = 2; p[0].num_ref_idx_l0_active_minus1 = 0;
    a = h264_nal(data, 0, 2, 3, 2, 0, 0); b = h264_nal(data + a, 5, 0, 3, 2, 0, 0);
    expect(!vav4l2_h264_validate_headers(data, a + b, &pic, buffers, 1, &pps, &dec) && (dec.flags & V4L2_H264_DECODE_PARAM_FLAG_PFRAME), "intra-first-slice-does-not-hide-later-P-slice");
    p[1].slice_type = 5; b = h264_nal(data + a, 5, 5, 3, 2, 0, 0);
    expect(vav4l2_h264_validate_headers(data, a + b, &pic, buffers, 1, &pps, &dec) == VA_STATUS_ERROR_INVALID_PARAMETER, "all-slices-of-this-type-flag-cannot-contradict-earlier-intra");
    params.num_elements = 1; p[0].slice_type = 2;
    p[0].num_ref_idx_l0_active_minus1 = p[0].num_ref_idx_l1_active_minus1 = 255;
    h264_fill_pps(&pps, &pic, &p[0]); a = h264_nal(data, 0, 2, 3, 2, 0, 0);
    expect(!pps.num_ref_idx_l0_default_active_minus1 && !pps.num_ref_idx_l1_default_active_minus1 &&
        !vav4l2_h264_validate_headers(data, a, &pic, buffers, 1, &pps, &dec), "intra-unused-reference-count-markers-are-not-active-defaults");
    VAPictureParameterBufferH264 refs = {0}; struct vav4l2_driver_data dd = {0};
    for (unsigned i = 0; i < 16; i++) refs.ReferenceFrames[i].flags = VA_PICTURE_H264_INVALID;
    refs.ReferenceFrames[0] = (VAPictureH264){.picture_id = 77}; h264_fill_decode_params(&dec, &refs, &dd, NULL);
    unsigned valid = 0; for (unsigned i = 0; i < 16; i++) valid += !!(dec.dpb[i].flags & V4L2_H264_DPB_ENTRY_FLAG_VALID);
    expect(!valid, "inactive-unvalidated-surface-is-not-promoted-to-valid-DPB");
}
static size_t hevc_nal(uint8_t *out, unsigned type, unsigned temporal, unsigned first, unsigned id)
{
    struct writer w = {0}; bit(&w, first); if (type >= 16 && type <= 21) bit(&w, 0); ue(&w, id); bit(&w, 1);
    memcpy(out, (uint8_t[]){0, 0, 0, 1, type << 1, temporal}, 6); memcpy(out + 6, w.data, (w.bits + 7) / 8); return 6 + (w.bits + 7) / 8;
}
static size_t marked_nal(uint8_t *out, unsigned mb, int idr, unsigned op, unsigned arg)
{
    struct writer w = {0}; ue(&w, mb); ue(&w, 2); ue(&w, 3); fixed(&w, idr ? 0 : 1, 4);
    if (idr) ue(&w, 0);
    fixed(&w, 2, 4);
    if (idr) fixed(&w, arg, 2);
    else {
        bit(&w, !!op);
        if (op) {
            ue(&w, op);
            if (op == 1 || op == 2 || op == 3 || op == 4 || op == 6) ue(&w, arg);
            if (op == 3) ue(&w, 0);
            ue(&w, 0);
        }
    }
    bit(&w, 1); memcpy(out, (uint8_t[]){0, 0, 1, idr ? 0x65 : 0x41}, 4);
    memcpy(out + 4, w.data, (w.bits + 7) / 8); return 4 + (w.bits + 7) / 8;
}
static void h264_marking(void)
{
    uint8_t data[512]; VAPictureParameterBufferH264 pic = {.frame_num = 1};
    pic.seq_fields.bits.frame_mbs_only_flag = 1; pic.pic_fields.bits.reference_pic_flag = 1;
    VASliceParameterBufferH264 slices[2] = {{.slice_type = 2}, {.slice_type = 2, .first_mb_in_slice = 5}};
    struct vav4l2_buffer params = {.type = VASliceParameterBufferType, .data = slices, .size = sizeof(slices[0]), .num_elements = 2};
    struct vav4l2_buffer *buffers[] = {&params};
    struct v4l2_ctrl_h264_pps pps = {0}; struct v4l2_ctrl_h264_decode_params dec = {0};
    for (unsigned op = 0; op <= 6; op++) {
        size_t a = marked_nal(data, 0, 0, op, 1), b = marked_nal(data + a, 5, 0, op, 1);
        expect(!vav4l2_h264_validate_headers(data, a + b, &pic, buffers, 1, &pps, &dec), "same-MMCO-at-different-slice-bit-offsets-is-valid");
        struct v4l2_ctrl_h264_pps old_pps = pps; struct v4l2_ctrl_h264_decode_params old_dec = dec;
        b = marked_nal(data + a, 5, 0, op == 0 || op == 5 ? 1 : op, 2);
        expect(vav4l2_h264_validate_headers(data, a + b, &pic, buffers, 1, &pps, &dec) == VA_STATUS_ERROR_INVALID_PARAMETER &&
            !memcmp(&pps, &old_pps, sizeof(pps)) && !memcmp(&dec, &old_dec, sizeof(dec)), "different-MMCO-or-argument-is-rejected-atomically");
    }
    pic.frame_num = 0;
    size_t a = marked_nal(data, 0, 1, 0, 0), b = marked_nal(data + a, 5, 1, 0, 0);
    expect(!vav4l2_h264_validate_headers(data, a + b, &pic, buffers, 1, &pps, &dec), "identical-IDR-marking-at-different-bit-offsets");
    for (unsigned flags = 1; flags < 4; flags++) {
        b = marked_nal(data + a, 5, 1, 0, flags);
        expect(vav4l2_h264_validate_headers(data, a + b, &pic, buffers, 1, &pps, &dec) == VA_STATUS_ERROR_INVALID_PARAMETER,
            "equal-length-IDR-output-and-long-term-flags-cannot-disagree");
    }
}
static void hevc(void)
{
    VAPictureParameterBufferHEVC pic = {0}; pic.slice_parsing_fields.bits.IdrPicFlag = pic.slice_parsing_fields.bits.RapPicFlag = 1;
    uint8_t data[512]; struct v4l2_ctrl_hevc_pps pps = {0}; size_t a = hevc_nal(data, 19, 1, 1, 15), b = hevc_nal(data + a, 19, 1, 0, 15);
    expect(!vav4l2_hevc_parse_headers(data, a + b, &pic, &pps, NULL) && pps.pic_parameter_set_id == 15, "HEVC-two-slice-nonzero-PPS-identity");
    a = hevc_nal(data, 19, 1, 1, 63); expect(!vav4l2_hevc_parse_headers(data, a, &pic, &pps, NULL) && pps.pic_parameter_set_id == 63, "HEVC-maximum-PPS-ID");
    struct v4l2_ctrl_hevc_pps saved = pps;
    a = hevc_nal(data, 19, 1, 1, 64); expect(vav4l2_hevc_parse_headers(data, a, &pic, &pps, NULL) == VA_STATUS_ERROR_INVALID_PARAMETER && !memcmp(&saved, &pps, sizeof(pps)), "HEVC-PPS-overflow-is-atomic");
    a = hevc_nal(data, 19, 1, 1, 15); b = hevc_nal(data + a, 19, 1, 0, 14);
    expect(vav4l2_hevc_parse_headers(data, a + b, &pic, &pps, NULL) == VA_STATUS_ERROR_INVALID_PARAMETER, "HEVC-second-PPS-mismatch");
    b = hevc_nal(data + a, 19, 1, 1, 15);
    expect(vav4l2_hevc_parse_headers(data, a + b, &pic, &pps, NULL) == VA_STATUS_ERROR_INVALID_PARAMETER, "HEVC-two-first-slices-are-two-pictures");
    data[5] = 0; expect(vav4l2_hevc_parse_headers(data, a, &pic, &pps, NULL) == VA_STATUS_ERROR_INVALID_PARAMETER, "HEVC-temporal-ID-zero");
    data[5] = 9; expect(vav4l2_hevc_parse_headers(data, a, &pic, &pps, NULL) == VA_STATUS_ERROR_INVALID_PARAMETER, "HEVC-unsupported-multilayer-not-silently-decoded");
    data[5] = 1; data[4] |= 128; expect(vav4l2_hevc_parse_headers(data, a, &pic, &pps, NULL) == VA_STATUS_ERROR_INVALID_PARAMETER, "HEVC-forbidden-NAL-bit");
    a = hevc_nal(data, 19, 1, 1, 15); pic.slice_parsing_fields.bits.IdrPicFlag = 0;
    expect(vav4l2_hevc_parse_headers(data, a, &pic, &pps, NULL) == VA_STATUS_ERROR_INVALID_PARAMETER, "HEVC-VA-IDR-must-match-real-NAL");
    pic.slice_parsing_fields.bits.IdrPicFlag = 1;
    for (size_t n = 0; n < 7; n++) expect(vav4l2_hevc_parse_headers(data, n, &pic, &pps, NULL) == VA_STATUS_ERROR_INVALID_PARAMETER, "HEVC-truncated-prefix-and-header");
    pic.slice_parsing_fields.bits.pps_slice_chroma_qp_offsets_present_flag = 1;
    expect(!vav4l2_hevc_fill_pps(&pps, &pic) && (pps.flags & V4L2_HEVC_PPS_FLAG_PPS_SLICE_CHROMA_QP_OFFSETS_PRESENT), "HEVC-slice-chroma-QP-syntax-preserved");
    a = hevc_nal(data, 19, 1, 1, 15); b = hevc_nal(data + a, 19, 1, 0, 15);
    data[6] |= 64; data[a + 6] |= 64;
    struct v4l2_ctrl_hevc_decode_params dec = {.flags = V4L2_HEVC_DECODE_PARAM_FLAG_IDR_PIC};
    expect(!vav4l2_hevc_parse_headers(data, a + b, &pic, &pps, &dec) &&
        dec.flags == (V4L2_HEVC_DECODE_PARAM_FLAG_IDR_PIC | V4L2_HEVC_DECODE_PARAM_FLAG_NO_OUTPUT_OF_PRIOR), "HEVC-IRAP-no-output-flag-reaches-decode-control");
    struct v4l2_ctrl_hevc_decode_params old_dec = dec; saved = pps; data[a + 6] &= ~64;
    expect(vav4l2_hevc_parse_headers(data, a + b, &pic, &pps, &dec) == VA_STATUS_ERROR_INVALID_PARAMETER &&
        !memcmp(&dec, &old_dec, sizeof(dec)) && !memcmp(&pps, &saved, sizeof(pps)), "HEVC-conflicting-IRAP-output-flags-are-atomic");
    data[6] &= ~64;
    expect(!vav4l2_hevc_parse_headers(data, a + b, &pic, &pps, &dec) && dec.flags == V4L2_HEVC_DECODE_PARAM_FLAG_IDR_PIC,
        "HEVC-no-output-flag-is-cleared-on-next-picture");
    data[5] = 2;
    expect(vav4l2_hevc_parse_headers(data, a, &pic, &pps, &dec) == VA_STATUS_ERROR_INVALID_PARAMETER, "HEVC-IRAP-must-use-temporal-layer-zero");
}
int main(void) { h264(); h264_marking(); hevc(); return failures != 0; }
