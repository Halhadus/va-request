/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef VAV4L2_STATEFUL_FIXTURE_H
#define VAV4L2_STATEFUL_FIXTURE_H
#include <assert.h>
#include <string.h>
#include "va_v4l2.h"
struct stateful_fixture {
    VAPictureParameterBufferH264 pic;
    VASliceParameterBufferH264 slice;
    uint8_t nal[16];
    struct vav4l2_buffer buffers[3], *batch[3];
};
static void stateful_fixture(struct stateful_fixture *f)
{
    memset(f, 0, sizeof(*f));
    f->pic.seq_fields.bits.frame_mbs_only_flag = f->pic.seq_fields.bits.direct_8x8_inference_flag = 1;
    f->pic.seq_fields.bits.chroma_format_idc = 1; f->pic.pic_fields.bits.reference_pic_flag = 1;
    for (unsigned i = 0; i < 16; i++) f->pic.ReferenceFrames[i].flags = VA_PICTURE_H264_INVALID;
    /* IDR: first_mb=0,I,PPS3,frame_num=0,idr_id=0,POC0,marking=00. */
    const char *bits = "101100100000010000001";
    memcpy(f->nal, (uint8_t[]){0,0,0,1,0x65}, 5);
    for (unsigned i = 0; bits[i]; i++) if (bits[i] == '1') f->nal[5 + i / 8] |= 128 >> (i % 8);
    f->slice.slice_type = 2; f->slice.slice_data_size = 8;
    f->buffers[0] = (struct vav4l2_buffer){.type = VAPictureParameterBufferType, .data = &f->pic, .size = sizeof(f->pic)};
    f->buffers[1] = (struct vav4l2_buffer){.type = VASliceParameterBufferType, .data = &f->slice, .size = sizeof(f->slice)};
    f->buffers[2] = (struct vav4l2_buffer){.type = VASliceDataBufferType, .data = f->nal, .size = 8};
    for (unsigned i = 0; i < 3; i++) f->batch[i] = &f->buffers[i];
}
#endif
