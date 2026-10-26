/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "codecs/request_slices.h"
#include "codecs/bytestream.h"
#include "probe.h"
static unsigned submits, controls, pulls, fail_control, fail_submit, stops;
static int slice_only = 1, no_hold, hevc_case, frame_case;
static unsigned array_capacity = 2;
static unsigned expected_header[2];
static int hook(int fd, unsigned long request, void *arg)
{
    (void)fd;
    if (request == VIDIOC_QUERYCTRL) { struct v4l2_queryctrl *q = arg; q->minimum = 0; q->maximum = slice_only ? 0 : 1; return 0; }
    if (request == VIDIOC_QUERYMENU || request == VIDIOC_G_FMT) return 0;
    if (request == VIDIOC_CREATE_BUFS) { ((struct v4l2_create_buffers *)arg)->capabilities = no_hold ? 0 : V4L2_BUF_CAP_SUPPORTS_M2M_HOLD_CAPTURE_BUF; return 0; }
    if (request == VIDIOC_QUERY_EXT_CTRL) {
        struct v4l2_query_ext_ctrl *q = arg;
        if (q->id == V4L2_CID_STATELESS_HEVC_ENTRY_POINT_OFFSETS) { q->elem_size = 4; q->elems = 4; q->nr_of_dims = 1; q->dims[0] = 4; q->flags = V4L2_CTRL_FLAG_DYNAMIC_ARRAY; return 0; }
        if (frame_case && q->id == V4L2_CID_STATELESS_HEVC_SLICE_PARAMS) {
            q->elem_size = sizeof(struct v4l2_ctrl_hevc_slice_params); q->nr_of_dims = 1; q->dims[0] = array_capacity; q->flags = V4L2_CTRL_FLAG_DYNAMIC_ARRAY;
        }
        return 0;
    }
    if (request == VIDIOC_STREAMOFF) { stops++; return 0; }
    if (request == VIDIOC_S_EXT_CTRLS) {
        controls++;
        struct v4l2_ext_controls *batch = arg;
        for (unsigned i = 0; i < batch->count; i++) if (batch->controls[i].id == V4L2_CID_STATELESS_H264_SLICE_PARAMS) {
            const struct v4l2_ctrl_h264_slice_params *p = batch->controls[i].ptr;
            assert(p->first_mb_in_slice == submits && p->header_bit_size == expected_header[submits]);
        }
        if (hevc_case) {
            unsigned found = 0;
            for (unsigned i = 0; i < batch->count; i++) {
                if (batch->controls[i].id == V4L2_CID_STATELESS_HEVC_SLICE_PARAMS) {
                    const struct v4l2_ctrl_hevc_slice_params *p = batch->controls[i].ptr;
                    assert(p->nal_unit_type == 19 && p->slice_type == 2 && p->nuh_temporal_id_plus1 == 1);
                    assert(p->num_entry_point_offsets == 1 && p->slice_segment_addr == submits && p->data_byte_offset == 4);
                    if (frame_case) assert(batch->controls[i].size == 2*sizeof(*p) && p[1].slice_segment_addr == 1 && p[1].num_entry_point_offsets == 1);
                    found++;
                }
                if (batch->controls[i].id == V4L2_CID_STATELESS_HEVC_ENTRY_POINT_OFFSETS) {
                    assert(batch->controls[i].size == (frame_case ? 8u : 4u) && *(const uint32_t *)batch->controls[i].ptr == 1); found++;
                }
            }
            assert(found == 2);
        }
        return fail_control && controls == fail_control ? -1 : 0;
    }
    return 0;
}
int __wrap_vav4l2_decode_submit(struct vav4l2_context *ctx, int index, uint32_t bytes, uint64_t timestamp)
{
    assert(frame_case && !submits && timestamp == 0 && bytes > 8 && ((uint8_t *)ctx->output_buf_ptr[index])[0] == 0x26);
    submits++; vav4l2_out_pool_push(ctx, index); return 1;
}
int __wrap_vav4l2_decode_submit_field(struct vav4l2_context *ctx, int index, uint32_t bytes, uint64_t ts, int capture, int hold)
{
    assert(ts == 0 && bytes >= 5 && ((uint8_t *)ctx->output_buf_ptr[index])[0] == (hevc_case ? 0x26 : 0x65));
    assert(capture == (submits ? 1 : -1) && hold == !submits);
    submits++; vav4l2_out_pool_push(ctx, index);
    return fail_submit && submits == fail_submit ? -2 : 1;
}
int __wrap_vav4l2_surface_pull_capture(struct vav4l2_context *ctx, struct vav4l2_surface *s, int index)
{
    (void)ctx; (void)s; assert(index == 1 && submits == (frame_case ? 1u : 2u)); pulls++; return 0;
}
static size_t make_nal(uint8_t *data, unsigned mb)
{
    struct bits b = {0}; size_t size = 0;
    ue(&b, mb); ue(&b, 2); ue(&b, 3); put(&b, 4, 0); ue(&b, 0); put(&b, 4, 0); put(&b, 2, 0); se(&b, 0);
    expected_header[mb] = b.bit + 8; put(&b, 8, 0xa5);
    assert(!nal(&b, 5, 0, data, 64, &size)); return size;
}
static size_t make_hevc(uint8_t *data, unsigned address)
{
    struct bits b = {0}; size_t size = 0;
    put(&b, 1, !address); put(&b, 1, 0); ue(&b, 0);
    if (address) put(&b, 1, 1);
    ue(&b, 2); se(&b, 0); ue(&b, 1); ue(&b, 0); put(&b, 1, 0);
    put(&b, 1, 1); while (b.bit % 8) put(&b, 1, 0);
    put(&b, 16, 0xa5a5);
    assert(!nal(&b, 19, 1, data, 64, &size)); return size;
}
static void hevc_submission(void)
{
    uint8_t data[128], output[128]; size_t first = make_hevc(data, 0), second = make_hevc(data + first, 1);
    VAPictureParameterBufferHEVC pic = {.pic_width_in_luma_samples = 16, .pic_height_in_luma_samples = 32,
        .log2_diff_max_min_luma_coding_block_size = 1};
    pic.pic_fields.bits.chroma_format_idc = pic.pic_fields.bits.entropy_coding_sync_enabled_flag = 1;
    VASliceParameterBufferHEVC slices[2] = {{.slice_data_byte_offset = 4}, {.slice_data_byte_offset = 4, .slice_segment_address = 1}};
    for (unsigned i = 0; i < 2; i++) slices[i].LongSliceFlags.fields.slice_type = 2;
    struct vav4l2_buffer b = {.type = VASliceParameterBufferType, .data = slices, .size = sizeof(*slices), .num_elements = 2, .allocated_elements = 2}, *batch[] = {&b};
    struct v4l2_ctrl_hevc_decode_params dec = {0};
    struct v4l2_ext_control base = {.id = V4L2_CID_STATELESS_HEVC_DECODE_PARAMS, .ptr = &dec, .size = sizeof(dec)};
    struct vav4l2_surface surface = {0};
    struct vav4l2_context ctx = {.codec = VAV4L2_CODEC_HEVC, .v4l2_fd = 8, .request_fd = 9, .media_fd = -1,
        .request_slice_based = 1, .request_no_start_code = 1, .current_surface = &surface,
        .n_free_out = 1, .output_buf_ptr = {output}, .output_buf_size = sizeof(output)};
    struct vav4l2_slice_plan *plan = NULL;
    submits = controls = pulls = fail_control = 0; hevc_case = 1;
    slices[1].slice_data_byte_offset = 5;
    assert(vav4l2_request_slice_plan(&ctx, batch, 1, data, first + second, &pic, &dec, &plan) != VA_STATUS_SUCCESS && !plan && !submits);
    slices[1].slice_data_byte_offset = 4;
    assert(!vav4l2_request_slice_plan(&ctx, batch, 1, data, first + second, &pic, &dec, &plan));
    assert(!vav4l2_request_slice_submit(&ctx, plan, &base, 1) && submits == 2 && pulls == 1);
    submits = controls = pulls = 0; frame_case = 1; no_hold = 1;
    ctx.request_slice_based = 0; ctx.output_bufs_allocd = 1; ctx.n_free_out = 1;
    assert(!vav4l2_request_slice_submit(&ctx, plan, &base, 1) && submits == 1 && pulls == 1);
    submits = controls = pulls = 0; array_capacity = 1;
    assert(vav4l2_request_slice_submit(&ctx, plan, &base, 1) == VA_STATUS_ERROR_UNIMPLEMENTED && !submits && !controls && !pulls);
    frame_case = no_hold = 0; array_capacity = 2;
    vav4l2_request_slice_free(plan); hevc_case = 0;
}
int main(void)
{
    vav4l2_set_ioctl_hook(hook);
    for (unsigned codec = VAV4L2_CODEC_H264; codec <= VAV4L2_CODEC_HEVC; codec++) {
        int slice, raw;
        assert(!vav4l2_request_modes(8, codec, &slice, &raw) && slice && raw);
        no_hold = 1; assert(vav4l2_request_modes(8, codec, &slice, &raw) < 0); no_hold = 0;
        slice_only = 0; assert(!vav4l2_request_modes(8, codec, &slice, &raw) && !slice && !raw); slice_only = 1;
    }
    struct vav4l2_driver_data dd = {0};
    uint8_t data[128], output[128]; size_t first = make_nal(data, 0), second = make_nal(data + first, 1);
    VAPictureParameterBufferH264 pic = {.picture_width_in_mbs_minus1 = 1};
    pic.seq_fields.bits.frame_mbs_only_flag = 1; pic.seq_fields.bits.chroma_format_idc = 1;
    pic.pic_fields.bits.reference_pic_flag = 1;
    VASliceParameterBufferH264 slices[2] = {{.slice_type = 2}, {.slice_type = 2, .first_mb_in_slice = 1}};
    struct vav4l2_buffer b = {.type = VASliceParameterBufferType, .data = slices, .size = sizeof(*slices), .num_elements = 2, .allocated_elements = 2}, *batch[] = {&b};
    struct v4l2_ctrl_h264_decode_params dec = {0}; struct v4l2_ctrl_h264_pps pps = {0};
    struct v4l2_ext_control base[] = {{.id = V4L2_CID_STATELESS_H264_PPS, .ptr = &pps, .size = sizeof(pps)},
        {.id = V4L2_CID_STATELESS_H264_DECODE_PARAMS, .ptr = &dec, .size = sizeof(dec)}};
    struct vav4l2_surface surface = {.surface_id = 1};
    struct vav4l2_context ctx = {.codec = VAV4L2_CODEC_H264, .v4l2_fd = 8, .request_fd = 9, .media_fd = -1,
        .request_slice_based = 1, .request_no_start_code = 1, .driver_data = &dd, .current_surface = &surface,
        .n_free_out = 1, .output_buf_ptr = {output}, .output_buf_size = sizeof(output)};
    struct vav4l2_slice_plan *plan = NULL;
    slices[1].slice_qp_delta = 1;
    assert(vav4l2_request_slice_plan(&ctx, batch, 1, data, first + second, &pic, &dec, &plan) != VA_STATUS_SUCCESS && !plan && !submits);
    slices[1].slice_qp_delta = 0;
    assert(!vav4l2_request_slice_plan(&ctx, batch, 1, data, first + second, &pic, &dec, &plan));
    memset(data, 0xff, sizeof(data)); memset(slices, 0xff, sizeof(slices));
    assert(!vav4l2_request_slice_submit(&ctx, plan, base, 2) && submits == 2 && pulls == 1);
    submits = controls = pulls = 0; ctx.n_free_out = 1; fail_control = 2; ctx.streamed = 1;
    assert(vav4l2_request_slice_submit(&ctx, plan, base, 2) != VA_STATUS_SUCCESS && submits == 1 && !pulls && stops == 2);
    vav4l2_request_slice_free(plan);
    hevc_submission();
    vav4l2_set_ioctl_hook(NULL);
    puts("Request mode selection, owned slice plans, HOLD and failed-batch recovery: PASS"); return 0;
}
