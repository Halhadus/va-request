/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "codecs/vp9_request.h"
static int ref_delta = 2, fail_decode, fail_pull;
static uint32_t ten_capture = V4L2_PIX_FMT_P010, expected_capture;
static unsigned request_controls;
struct writer { uint8_t data[256]; unsigned bit; };
static void put(struct writer *w, unsigned n, unsigned v)
{
    assert(w->bit + n <= sizeof(w->data) * 8);
    while (n--) { w->data[w->bit / 8] |= ((v >> n) & 1) << (7 - w->bit % 8); w->bit++; }
}
static size_t key(struct writer *w, VADecPictureParameterBufferVP9 *p, unsigned profile)
{
    *w = (struct writer){0};
    *p = (VADecPictureParameterBufferVP9){.profile = profile, .bit_depth = profile ? 10 : 8,
        .frame_width = 16, .frame_height = 16, .first_partition_size = 16};
    p->pic_fields.bits.show_frame = p->pic_fields.bits.subsampling_x = p->pic_fields.bits.subsampling_y = 1;
    p->pic_fields.bits.refresh_frame_context = p->pic_fields.bits.frame_parallel_decoding_mode = 1;
    put(w, 2, 2); put(w, 1, profile & 1); put(w, 1, profile >> 1);
    put(w, 1, 0); put(w, 1, 0); put(w, 1, 1); put(w, 1, 0); put(w, 24, 0x498342);
    if (profile) put(w, 1, 0);
    put(w, 3, 1); put(w, 1, 0); put(w, 16, 15); put(w, 16, 15); put(w, 1, 0);
    put(w, 1, 1); put(w, 1, 1); put(w, 2, 0); /* context */
    put(w, 6, 7); put(w, 3, 3); put(w, 1, 1); put(w, 1, 1); /* loop filter */
    put(w, 1, 1); put(w, 6, ref_delta); put(w, 1, 0);
    for (unsigned i = 0; i < 3; i++) put(w, 1, 0);
    put(w, 1, 1); put(w, 6, 3); put(w, 1, 1); put(w, 1, 0);
    put(w, 8, 0); put(w, 3, 0); /* lossless */
    put(w, 1, 1); put(w, 1, 1); put(w, 7, 0); put(w, 1, 0); put(w, 1, 1); put(w, 1, 0);
    for (unsigned i = 0; i < 8; i++) for (unsigned j = 0; j < 4; j++) {
        put(w, 1, i == 0 && j == 0);
        if (i == 0 && j == 0) { put(w, 8, 9); put(w, 1, 1); }
    }
    put(w, 1, 0); put(w, 16, 16);
    while (w->bit % 8) put(w, 1, 0);
    p->frame_header_length_in_bytes = w->bit / 8;
    return w->bit / 8 + 17; /* zero probability updates and one tile byte */
}
static int hook(int fd, unsigned long request, void *arg)
{
    (void)fd;
    if (request == VIDIOC_QUERY_EXT_CTRL) return 0;
    if (request == VIDIOC_ENUM_FMT) {
        struct v4l2_fmtdesc *f = arg;
        if (f->index) return -1;
        f->pixelformat = ten_capture; return 0;
    }
    if (request == VIDIOC_S_EXT_CTRLS) {
        struct v4l2_ext_controls *c = arg;
        assert(c->controls[0].id == V4L2_CID_STATELESS_VP9_FRAME);
        const struct v4l2_ctrl_vp9_frame *frame = c->controls[0].ptr;
        assert(frame->lf.ref_deltas[0] == ref_delta);
        if (c->which == V4L2_CTRL_WHICH_REQUEST_VAL) {
            assert(c->count == 2 && c->controls[1].id == V4L2_CID_STATELESS_VP9_COMPRESSED_HDR);
            request_controls++;
        }
        return 0;
    }
    assert(!"unexpected VP9 ioctl"); return -1;
}
int __wrap_vav4l2_ensure_capture(struct vav4l2_context *c, int w, int h, uint32_t format)
{ (void)c; assert(w == 16 && h == 16 && format == expected_capture); return 0; }
int __wrap_vav4l2_decode_submit(struct vav4l2_context *c, int index, uint32_t bytes, uint64_t timestamp)
{ assert(bytes > 16 && !timestamp); vav4l2_out_pool_push(c, index); return fail_decode ? -2 : 0; }
int __wrap_vav4l2_surface_pull_capture(struct vav4l2_context *c, struct vav4l2_surface *s, int index)
{ (void)c; (void)s; assert(!index); return fail_pull ? -1 : 0; }
static void submission(void)
{
    struct writer w; VADecPictureParameterBufferVP9 pic;
    size_t size = key(&w, &pic, 2);
    VASliceParameterBufferVP9 slice = {.slice_data_size = size};
    struct vav4l2_buffer b[] = {{.type = VAPictureParameterBufferType, .data = &pic, .size = sizeof(pic)},
        {.type = VASliceParameterBufferType, .data = &slice, .size = sizeof(slice)},
        {.type = VASliceDataBufferType, .data = w.data, .size = size}};
    struct vav4l2_buffer *batch[] = {&b[0], &b[1], &b[2]};
    struct vav4l2_driver_data dd = {0}; uint8_t output[512];
    struct vav4l2_surface surface = {.timestamp = 0};
    struct vav4l2_context c = {.driver_data = &dd, .current_surface = &surface, .codec = VAV4L2_CODEC_VP9,
        .profile = VAProfileVP9Profile2, .width = 16, .height = 16, .v4l2_fd = 8, .request_fd = 9,
        .streamed = 1, .n_free_out = 1, .output_buf_ptr = {output}, .output_buf_size = sizeof(output)};
    vav4l2_set_ioctl_hook(hook);
    expected_capture = ten_capture;
    assert(!vav4l2_vp9_translate(&c, batch, 3) && c.vp9_state->lf_ref_deltas[0] == 2);
    struct vav4l2_vp9_state saved = *c.vp9_state;
    ref_delta = 9; key(&w, &pic, 2); fail_decode = 1;
    assert(vav4l2_vp9_translate(&c, batch, 3) == VA_STATUS_ERROR_DECODING_ERROR && !memcmp(c.vp9_state, &saved, sizeof(saved)));
    fail_decode = 0; fail_pull = 1;
    assert(vav4l2_vp9_translate(&c, batch, 3) != VA_STATUS_SUCCESS && !memcmp(c.vp9_state, &saved, sizeof(saved)));
    fail_pull = 0; ten_capture = expected_capture = V4L2_PIX_FMT_NV15;
    assert(!vav4l2_vp9_translate(&c, batch, 3) && c.vp9_state->lf_ref_deltas[0] == 9 && request_controls == 4);
    free(c.vp9_state); vav4l2_set_ioctl_hook(NULL); ref_delta = 2;
}
int main(void)
{
    struct vav4l2_driver_data dd = {0};
    struct vav4l2_context ctx = {.driver_data = &dd, .width = 16, .height = 16};
    struct writer w; VADecPictureParameterBufferVP9 p;
    struct v4l2_ctrl_vp9_frame frame;
    struct v4l2_ctrl_vp9_compressed_hdr compressed;
    for (unsigned profile = 0; profile <= 2; profile += 2) {
        ctx.profile = profile ? VAProfileVP9Profile2 : VAProfileVP9Profile0;
        size_t size = key(&w, &p, profile);
        struct vav4l2_vp9_state state = {0};
        assert(vav4l2_vp9_request_parse(&ctx, &p, w.data, size, &state, &frame, &compressed) == VA_STATUS_SUCCESS);
        assert(frame.profile == profile && frame.bit_depth == p.bit_depth && frame.quant.base_q_idx == 0);
        assert(frame.lf.level == 7 && frame.lf.sharpness == 3 && frame.lf.ref_deltas[0] == 2 && frame.lf.mode_deltas[0] == -3);
        assert(frame.seg.feature_data[0][V4L2_VP9_SEG_LVL_ALT_Q] == -9 && frame.seg.tree_probs[0] == 255);
        assert(frame.seg.flags & V4L2_VP9_SEGMENTATION_FLAG_UPDATE_DATA);
        assert(compressed.tx_mode == V4L2_VP9_TX_MODE_ONLY_4X4);
        struct vav4l2_vp9_state saved = state;
        struct v4l2_ctrl_vp9_frame saved_frame = frame;
        struct v4l2_ctrl_vp9_compressed_hdr saved_compressed = compressed;
        for (size_t n = 0; n < size - 1; n++) {
            assert(vav4l2_vp9_request_parse(&ctx, &p, w.data, n, &state, &frame, &compressed) != VA_STATUS_SUCCESS);
            assert(!memcmp(&state, &saved, sizeof(state)) && !memcmp(&frame, &saved_frame, sizeof(frame)) &&
                !memcmp(&compressed, &saved_compressed, sizeof(compressed)));
        }
        w.data[p.frame_header_length_in_bytes] = 0xff;
        assert(vav4l2_vp9_request_parse(&ctx, &p, w.data, size, &state, &frame, &compressed) != VA_STATUS_SUCCESS);
        assert(!memcmp(&state, &saved, sizeof(state)));
        size = key(&w, &p, profile); p.first_partition_size--;
        assert(vav4l2_vp9_request_parse(&ctx, &p, w.data, size, &state, &frame, &compressed) != VA_STATUS_SUCCESS);
    }
    submission();
    puts("VP9 Request headers, recovered controls and atomic rejection: PASS");
    return 0;
}
