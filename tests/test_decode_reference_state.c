/* Reject invalid reference storage through the actual codec entrypoints before ioctl/QBUF. */
#include <stdio.h>
#include <string.h>
#include "va_v4l2.h"
static int failures, ioctls;
static void expect(int ok, const char *name)
{
    if (!ok) { fprintf(stderr, "FAIL %s\n", name); failures++; }
}
static int ioctl_count(int fd, unsigned long request, void *arg)
{
    (void)fd; (void)request; (void)arg; ioctls++; return -1;
}
static void h264_contract(struct vav4l2_context *ctx)
{
    VAPictureParameterBufferH264 pic = {0}; uint8_t payload[] = {0, 0, 1, 0x65, 0x80};
    pic.seq_fields.bits.frame_mbs_only_flag = 1;
    for (unsigned i = 0; i < 16; i++) pic.ReferenceFrames[i].flags = VA_PICTURE_H264_INVALID;
    struct vav4l2_buffer picture = {.type = VAPictureParameterBufferType, .data = &pic},
        data = {.type = VASliceDataBufferType, .data = payload, .size = sizeof(payload)};
    struct vav4l2_buffer *buffers[] = {&picture, &data};
    pic.seq_fields.bits.pic_order_cnt_type = 1; ioctls = 0;
    expect(vav4l2_h264_translate(ctx, buffers, 2) == VA_STATUS_ERROR_UNIMPLEMENTED && !ioctls,
        "poc1-without-original-sps-offsets-never-submits-zero-invented-cycle");
    pic.seq_fields.bits.pic_order_cnt_type = 0; pic.seq_fields.bits.frame_mbs_only_flag = 0;
    pic.picture_height_in_mbs_minus1 = 1; pic.pic_fields.bits.field_pic_flag = 1;
    for (unsigned bottom = 0; bottom < 2; bottom++) {
        pic.CurrPic.flags = bottom ? VA_PICTURE_H264_BOTTOM_FIELD : VA_PICTURE_H264_TOP_FIELD;
        expect(vav4l2_h264_translate(ctx, buffers, 2) == VA_STATUS_ERROR_INVALID_SURFACE && !ioctls,
            "paff-staging-requires-an-active-owned-render-target");
    }
    pic.seq_fields.bits.frame_mbs_only_flag = 1;
    expect(vav4l2_h264_translate(ctx, buffers, 2) == VA_STATUS_ERROR_INVALID_PARAMETER && !ioctls,
        "progressive-sps-cannot-describe-a-field-picture");
    pic.pic_fields.bits.field_pic_flag = 0; pic.seq_fields.bits.log2_max_frame_num_minus4 = 13;
    expect(vav4l2_h264_translate(ctx, buffers, 2) == VA_STATUS_ERROR_INVALID_PARAMETER && !ioctls, "oversized-frame-num-syntax-rejected-before-shift");
    pic.seq_fields.bits.log2_max_frame_num_minus4 = 0; pic.frame_num = 16;
    expect(vav4l2_h264_translate(ctx, buffers, 2) == VA_STATUS_ERROR_INVALID_PARAMETER && !ioctls, "frame-num-must-fit-the-sps-bit-width");
    pic.frame_num = 0; pic.seq_fields.bits.frame_mbs_only_flag = 0; pic.picture_height_in_mbs_minus1 = 0;
    expect(vav4l2_h264_translate(ctx, buffers, 2) == VA_STATUS_ERROR_INVALID_PARAMETER && !ioctls, "interlaced-map-units-cannot-underflow");
}
static void hevc_contract(struct vav4l2_context *ctx)
{
    VAPictureParameterBufferHEVC pic = {0}; uint8_t payload[] = {0, 0, 1, 0x26, 1, 0x80};
    for (unsigned i = 0; i < 15; i++) pic.ReferenceFrames[i].flags = VA_PICTURE_HEVC_INVALID;
    struct vav4l2_buffer picture = {.type = VAPictureParameterBufferType, .data = &pic},
        data = {.type = VASliceDataBufferType, .data = payload, .size = sizeof(payload)};
    struct vav4l2_buffer *buffers[] = {&picture, &data};
    pic.st_rps_bits = UINT16_MAX + 1u; ioctls = 0;
    expect(vav4l2_hevc_translate(ctx, buffers, 2) == VA_STATUS_ERROR_INVALID_PARAMETER && !ioctls, "hevc-rps-bit-count-must-not-truncate-to-u16");
    pic.st_rps_bits = 0; pic.num_short_term_ref_pic_sets = 65;
    expect(vav4l2_hevc_translate(ctx, buffers, 2) == VA_STATUS_ERROR_INVALID_PARAMETER && !ioctls, "hevc-sps-short-term-set-count-is-bounded");
    pic.num_short_term_ref_pic_sets = 0; pic.num_long_term_ref_pic_sps = 33;
    expect(vav4l2_hevc_translate(ctx, buffers, 2) == VA_STATUS_ERROR_INVALID_PARAMETER && !ioctls, "hevc-sps-long-term-set-count-is-bounded");
    pic.num_long_term_ref_pic_sps = 0; pic.sps_max_dec_pic_buffering_minus1 = 16;
    expect(vav4l2_hevc_translate(ctx, buffers, 2) == VA_STATUS_ERROR_INVALID_PARAMETER && !ioctls, "hevc-sps-dpb-count-is-bounded");
    pic.sps_max_dec_pic_buffering_minus1 = 0; pic.CurrPic.pic_order_cnt = 10;
    pic.ReferenceFrames[0] = (VAPictureHEVC){.picture_id = 1, .pic_order_cnt = 10, .flags = VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE};
    expect(vav4l2_hevc_translate(ctx, buffers, 2) == VA_STATUS_ERROR_INVALID_PARAMETER && !ioctls, "hevc-before-must-have-an-earlier-poc");
    pic.ReferenceFrames[0].pic_order_cnt = 9; pic.ReferenceFrames[0].flags = VA_PICTURE_HEVC_RPS_ST_CURR_AFTER;
    expect(vav4l2_hevc_translate(ctx, buffers, 2) == VA_STATUS_ERROR_INVALID_PARAMETER && !ioctls, "hevc-after-must-have-a-later-poc");
    pic.ReferenceFrames[0].flags = VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE | VA_PICTURE_HEVC_LONG_TERM_REFERENCE;
    expect(vav4l2_hevc_translate(ctx, buffers, 2) == VA_STATUS_ERROR_INVALID_PARAMETER && !ioctls, "hevc-current-reference-cannot-be-both-short-and-long-term");
    for (unsigned i = 0; i < 9; i++) pic.ReferenceFrames[i] = (VAPictureHEVC){.picture_id = 1,
        .pic_order_cnt = (int)i - 9, .flags = VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE};
    expect(vav4l2_hevc_translate(ctx, buffers, 2) == VA_STATUS_ERROR_INVALID_PARAMETER && !ioctls, "hevc-num-poc-total-curr-cannot-exceed-eight");
    for (unsigned i = 0; i < 15; i++) pic.ReferenceFrames[i].flags = VA_PICTURE_HEVC_INVALID;
    pic.ReferenceFrames[0] = (VAPictureHEVC){.picture_id = 1, .pic_order_cnt = 1};
    pic.ReferenceFrames[1] = (VAPictureHEVC){.picture_id = 1, .pic_order_cnt = 2};
    expect(vav4l2_hevc_translate(ctx, buffers, 2) == VA_STATUS_ERROR_INVALID_PARAMETER && !ioctls,
        "hevc-future-DPB-cannot-alias-one-surface-with-two-POCs");
}
static VAStatus submit(struct vav4l2_context *ctx, VASurfaceID ref, unsigned codec)
{
    VAPictureParameterBufferH264 h264 = {0}; VAPictureParameterBufferHEVC hevc = {0};
    VAPictureParameterBufferVP8 vp8 = {0}; VAPictureParameterBufferMPEG2 mpeg2 = {.picture_coding_type = 3};
    for (unsigned i = 0; i < 16; i++) h264.ReferenceFrames[i].flags = VA_PICTURE_H264_INVALID;
    for (unsigned i = 0; i < 15; i++) hevc.ReferenceFrames[i].flags = VA_PICTURE_HEVC_INVALID;
    h264.ReferenceFrames[0] = (VAPictureH264){.picture_id = ref, .flags = VA_PICTURE_H264_SHORT_TERM_REFERENCE};
    hevc.ReferenceFrames[0] = (VAPictureHEVC){.picture_id = ref, .flags = codec == 4 ? 0 :
        codec == 5 ? VA_PICTURE_HEVC_LONG_TERM_REFERENCE : VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE};
    vp8.pic_fields.bits.key_frame = 1; vp8.last_ref_frame = vp8.golden_ref_frame = vp8.alt_ref_frame = ref;
    mpeg2.forward_reference_picture = mpeg2.backward_reference_picture = ref;
    void *params[] = {&h264, &hevc, &vp8, &mpeg2};
    VAStatus (*translate[])(struct vav4l2_context *, struct vav4l2_buffer **, int) = {
        vav4l2_h264_translate, vav4l2_hevc_translate, vav4l2_vp8_translate, vav4l2_mpeg2_translate};
    uint8_t payload[] = {0, 0, 1, 0x41, 0xe0, 0x5e, 0x89, 0xd6, 0xaa, 0x80};
    h264.seq_fields.bits.frame_mbs_only_flag = 1; h264.seq_fields.bits.chroma_format_idc = 1;
    h264.pic_fields.bits.reference_pic_flag = 1; h264.pic_fields.bits.weighted_pred_flag = 1;
    hevc.pic_width_in_luma_samples = hevc.pic_height_in_luma_samples = 16;
    vp8.frame_width = vp8.frame_height = 16;
    mpeg2.horizontal_size = mpeg2.vertical_size = 16;
    union { VASliceParameterBufferH264 h264; VASliceParameterBufferHEVC hevc; VASliceParameterBufferVP8 vp8;
        VASliceParameterBufferMPEG2 mpeg2; } slice = {0};
    VASliceParameterBufferBase base = {.slice_data_size = sizeof(payload)};
    memcpy(&slice, &base, sizeof(base));
    unsigned index = codec < 4 ? codec : 1;
    struct vav4l2_buffer picture = {.type = VAPictureParameterBufferType, .data = params[index]},
        data = {.type = VASliceDataBufferType, .data = payload, .size = sizeof(payload)};
    struct vav4l2_buffer parameter = {.type = VASliceParameterBufferType, .data = &slice, .size = sizeof(slice)};
    struct vav4l2_buffer *buffers[] = {&picture, &parameter, &data};
    return translate[index](ctx, buffers, 3);
}
static void av1_contract(struct vav4l2_context *ctx, struct vav4l2_surface *s, struct vav4l2_context *foreign)
{
    VADecPictureParameterBufferAV1 p = {0};
    p.pic_info_fields.bits.frame_type = V4L2_AV1_INTER_FRAME;
    p.primary_ref_frame = 0;
    for (unsigned i = 0; i < 8; i++) p.ref_frame_map[i] = 1;
    expect(vav4l2_av1_validate_references(ctx, &p) == VA_STATUS_SUCCESS, "av1-retained-zero-timestamp-reference");
    p.ref_frame_idx[0] = 8;
    expect(vav4l2_av1_validate_references(ctx, &p) == VA_STATUS_ERROR_INVALID_PARAMETER, "av1-primary-slot-bounds");
    p.ref_frame_idx[0] = 0; p.ref_frame_map[0] = VA_INVALID_SURFACE;
    expect(vav4l2_av1_validate_references(ctx, &p) == VA_STATUS_ERROR_INVALID_SURFACE, "av1-missing-primary-CDF-reference");
    p.ref_frame_map[0] = 1; s->capture_ctx = foreign;
    expect(vav4l2_av1_validate_references(ctx, &p) == VA_STATUS_ERROR_INVALID_SURFACE, "av1-foreign-CDF-storage");
    s->capture_ctx = ctx; s->buf_index = -1;
    expect(vav4l2_av1_validate_references(ctx, &p) == VA_STATUS_ERROR_INVALID_SURFACE, "av1-detached-CDF-storage");
    s->buf_index = 0; s->status = VASurfaceSkipped;
    expect(vav4l2_av1_validate_references(ctx, &p) == VA_STATUS_ERROR_DECODING_ERROR, "av1-failed-CDF-reference");
    s->status = VASurfaceReady; ctx->av1.model_active = 1;
    for (unsigned i = 0; i < 8; i++) ctx->av1.slot_buf[i] = 0;
    ctx->av1.buf_owner[0] = 1;
    expect(vav4l2_av1_validate_references(ctx, &p) == VA_STATUS_SUCCESS, "av1-slot-owner-model-matches-retained-reference");
    ctx->av1.slot_buf[0] = 1;
    expect(vav4l2_av1_validate_references(ctx, &p) == VA_STATUS_ERROR_INVALID_SURFACE, "av1-stale-slot-model-rejected");
    ctx->av1.model_active = 0;
    struct vav4l2_buffer picture = {.type = VAPictureParameterBufferType, .data = &p, .size = sizeof(p)}, *batch[] = {&picture};
    p.ref_frame_map[0] = VA_INVALID_SURFACE; ioctls = 0;
    expect(vav4l2_av1_translate(ctx, batch, 1) == VA_STATUS_ERROR_INVALID_SURFACE && !ioctls, "av1-rejection-precedes-controls-and-state-update");
}
int main(void)
{
    struct vav4l2_driver_data dd = {0}; struct vav4l2_context owner = {.driver_data = &dd, .capture_bufs_allocd = 2, .cap_width = 16, .cap_height = 16,
        .cap_pixelformat = V4L2_PIX_FMT_NV12, .streamed = 1, .request_fd = 78, .v4l2_fd = 77, .n_free_out = 1,
        .output_buf_size = 64}, foreign = {0};
    uint8_t output[64]; owner.output_buf_ptr[0] = output;
    struct vav4l2_surface s = {.surface_id = 1, .timestamp = 0, .has_pic = 1, .status = VASurfaceReady,
        .buf_index = 0, .capture_ctx = &owner, .last_writer = VAV4L2_WRITER_MEMFD};
    dd.surfaces[1] = &s; vav4l2_set_ioctl_hook(ioctl_count);
    av1_contract(&owner, &s, &foreign);
    h264_contract(&owner);
    hevc_contract(&owner);
    expect(vav4l2_validate_reference(&owner, 1) == VA_STATUS_SUCCESS, "first-frame-timestamp-zero-is-valid-with-retained-unmapped-capture");
    for (unsigned codec = 0; codec < 6; codec++) {
        ioctls = 0;
        expect(submit(&owner, VA_INVALID_SURFACE, codec) == VA_STATUS_ERROR_INVALID_SURFACE && !ioctls,
            "missing-reference-rejected-before-device-access");
        s.status = VASurfaceRendering;
        expect(submit(&owner, 1, codec) == VA_STATUS_ERROR_SURFACE_BUSY && !ioctls, "pending-reference-is-not-old-ready-content");
        s.status = VASurfaceSkipped;
        expect(submit(&owner, 1, codec) == VA_STATUS_ERROR_DECODING_ERROR && !ioctls, "failed-reference-cannot-be-used-by-next-picture");
        s.status = VASurfaceReady; s.has_pic = 0;
        expect(submit(&owner, 1, codec) == VA_STATUS_ERROR_DECODING_ERROR && !ioctls, "allocated-but-undecoded-surface-is-not-a-reference");
        s.has_pic = 1; s.last_writer = VAV4L2_WRITER_CPU;
        expect(submit(&owner, 1, codec) == VA_STATUS_ERROR_DECODING_ERROR && !ioctls, "cpu-replacement-does-not-update-the-kernel-reference-slot");
        s.last_writer = VAV4L2_WRITER_MEMFD; s.capture_ctx = &foreign;
        expect(submit(&owner, 1, codec) == VA_STATUS_ERROR_INVALID_SURFACE && !ioctls, "foreign-context-reference-is-not-this-queue-slot");
        s.capture_ctx = &owner; s.buf_index = -1;
        expect(submit(&owner, 1, codec) == VA_STATUS_ERROR_INVALID_SURFACE && !ioctls, "detached-snapshot-cannot-be-fed-as-retained-capture");
        s.buf_index = 0;
        expect(vav4l2_validate_reference(&owner, 1) == VA_STATUS_SUCCESS, "valid-reference-remains-usable-after-rejections");
    }
    vav4l2_set_ioctl_hook(NULL);
    return failures != 0;
}
