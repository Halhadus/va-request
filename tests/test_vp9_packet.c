/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "va_v4l2.h"
int main(void)
{
    VADecPictureParameterBufferVP9 p = {.frame_width = 16, .frame_height = 16, .profile = 0, .bit_depth = 8,
        .frame_header_length_in_bytes = 2, .first_partition_size = 1};
    p.pic_fields.bits.subsampling_x = p.pic_fields.bits.subsampling_y = p.pic_fields.bits.show_frame = 1;
    VASliceParameterBufferVP9 slice = {.slice_data_size = 4};
    uint8_t frame[] = {0x82, 0x49, 0x83, 0x42}, out[32], poison[32];
    struct vav4l2_buffer pic = {.type = VAPictureParameterBufferType, .data = &p, .size = sizeof(p)},
        params = {.type = VASliceParameterBufferType, .data = &slice, .size = sizeof(slice)},
        data = {.type = VASliceDataBufferType, .data = frame, .size = sizeof(frame)};
    struct vav4l2_buffer *batch[] = {&pic, &params, &data};
    struct vav4l2_context c = {.codec = VAV4L2_CODEC_VP9, .profile = VAProfileVP9Profile0, .width = 16, .height = 16};
    size_t size = 123;
    assert(vav4l2_stateful_packet(&c, batch, 3, out, sizeof(out), &size) == VA_STATUS_SUCCESS && size == 4 && !memcmp(out, frame, 4));
    p.pic_fields.bits.show_frame = 0; frame[0] = 0x80;
    struct vav4l2_packet_state state;
    struct vav4l2_packet_layout layout;
    assert(vav4l2_stateful_packet_prepare(&c, batch, 3, out, sizeof(out), &size, &state, &layout) == VA_STATUS_SUCCESS && size == 5);
    assert(!memcmp(out, frame, 4) && out[4] == 0x88 && layout.frame_size[0] == 4 && layout.frame_size[1] == 1);
    memset(out, 0xa5, sizeof(out)); memcpy(poison, out, sizeof(out)); size = 123;
    layout = (struct vav4l2_packet_layout){.frame_size = {123, 456}};
    assert(vav4l2_stateful_packet_prepare(&c, batch, 3, out, 4, &size, &state, &layout) == VA_STATUS_ERROR_ALLOCATION_FAILED &&
        size == 123 && !memcmp(out, poison, sizeof(out)) && layout.frame_size[0] == 123 && layout.frame_size[1] == 456);
    frame[0] = 0x88;
    assert(vav4l2_stateful_packet(&c, batch, 3, out, sizeof(out), &size) == VA_STATUS_ERROR_UNIMPLEMENTED && size == 123 && !memcmp(out, poison, sizeof(out)));
    frame[0] = 0x84; frame[1] = 0x04; frame[2] = 0; p.frame_header_length_in_bytes = 3; /* hidden inter, intra_only=0, reset=0, refresh slot 5 */
    p.pic_fields.bits.frame_type = 1;
    assert(vav4l2_stateful_packet(&c, batch, 3, out, sizeof(out), &size) == VA_STATUS_SUCCESS && out[4] == 0x8d);
    frame[1] = 0;
    assert(vav4l2_stateful_packet(&c, batch, 3, out, sizeof(out), &size) == VA_STATUS_ERROR_UNIMPLEMENTED);
    p.profile = 2; p.bit_depth = 10; c.profile = VAProfileVP9Profile2; p.pic_fields.bits.frame_type = 0;
    frame[0] = 0x90;
    assert(vav4l2_stateful_packet(&c, batch, 3, out, sizeof(out), &size) == VA_STATUS_SUCCESS && out[4] == 0x98);
    p.bit_depth = 12;
    assert(vav4l2_stateful_packet(&c, batch, 3, out, sizeof(out), &size) == VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
    puts("VP9 frame identity, hidden reference completion and atomic rejection: PASS"); return 0;
}
