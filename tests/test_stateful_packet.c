/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <stdio.h>
#include "stateful_fixture.h"
struct reader { uint8_t data[8192]; size_t bit, size; };
static unsigned read_bits(struct reader *r, unsigned n)
{
    assert(n <= 32 && r->bit + n <= r->size * 8); unsigned v = 0;
    while (n--) { v = (v << 1) | !!(r->data[r->bit / 8] & (128 >> (r->bit % 8))); r->bit++; }
    return v;
}
static unsigned read_ue(struct reader *r)
{
    unsigned zeros = 0; while (!read_bits(r, 1)) { zeros++; assert(zeros < 31); }
    return ((1u << zeros) - 1) + read_bits(r, zeros);
}
static struct reader reader(const uint8_t *unit, size_t size, unsigned header)
{
    struct reader r = {0}; unsigned prefix = unit[2] == 1 ? 3 : 4, zeros = 0;
    for (size_t i = prefix + header; i < size; i++) {
        if (zeros >= 2 && unit[i] == 3) { assert(i + 1 < size && unit[i + 1] <= 3); zeros = 0; continue; }
        assert(r.size < sizeof(r.data)); r.data[r.size++] = unit[i]; zeros = unit[i] ? 0 : zeros + 1;
    }
    return r;
}
int main(void)
{
    struct stateful_fixture f; stateful_fixture(&f);
    struct vav4l2_context c = {.codec = VAV4L2_CODEC_H264, .profile = VAProfileH264High, .width = 16, .height = 16};
    uint8_t packet[16384], poison[16384]; memset(packet, 0xa5, sizeof(packet)); memcpy(poison, packet, sizeof(packet)); size_t bytes = 0;
    assert(vav4l2_stateful_packet(&c, f.batch, 3, packet, sizeof(packet), &bytes) == VA_STATUS_SUCCESS);
    const uint8_t *unit; size_t cursor = 0, length; assert(vav4l2_annexb_next(packet, bytes, &cursor, &unit, &length) == 1 && (unit[4] & 31) == 7);
    struct reader r = reader(unit, length, 1);
    assert(read_bits(&r,8) == 100 && read_bits(&r,8) == 0 && read_bits(&r,8) == 30 && read_ue(&r) == 0);
    assert(read_ue(&r) == 1 && !read_ue(&r) && !read_ue(&r) && !read_bits(&r,1) && !read_bits(&r,1));
    assert(!read_ue(&r) && !read_ue(&r) && !read_ue(&r));
    assert(vav4l2_annexb_next(packet, bytes, &cursor, &unit, &length) == 1 && (unit[4] & 31) == 8);
    r = reader(unit, length, 1); assert(read_ue(&r) == 3 && read_ue(&r) == 0);
    assert(vav4l2_annexb_next(packet, bytes, &cursor, &unit, &length) == 1 && length == 8 && !memcmp(unit, f.nal, 8));
    assert(vav4l2_annexb_next(packet, bytes, &cursor, &unit, &length) == 0);
    memset(packet, 0xa5, sizeof(packet)); bytes = 123;
    assert(vav4l2_stateful_packet(&c, f.batch, 3, packet, 8, &bytes) == VA_STATUS_ERROR_ALLOCATION_FAILED && bytes == 123 && !memcmp(packet, poison, sizeof(packet)));
    f.pic.seq_fields.bits.pic_order_cnt_type = 1;
    assert(vav4l2_stateful_packet(&c, f.batch, 3, packet, sizeof(packet), &bytes) == VA_STATUS_ERROR_UNIMPLEMENTED && !memcmp(packet, poison, sizeof(packet)));
    f.pic.seq_fields.bits.pic_order_cnt_type = 0; f.pic.pic_fields.bits.field_pic_flag = 1;
    assert(vav4l2_stateful_packet(&c, f.batch, 3, packet, sizeof(packet), &bytes) == VA_STATUS_ERROR_UNIMPLEMENTED);
    f.pic.pic_fields.bits.field_pic_flag = 0; f.slice.slice_type = 3;
    assert(vav4l2_stateful_packet(&c, f.batch, 3, packet, sizeof(packet), &bytes) == VA_STATUS_ERROR_UNIMPLEMENTED);
    f.slice.slice_type = 0;
    assert(vav4l2_stateful_packet(&c, f.batch, 3, packet, sizeof(packet), &bytes) == VA_STATUS_ERROR_INVALID_PARAMETER);
    VAPictureParameterBufferHEVC h = {.pic_width_in_luma_samples = 16, .pic_height_in_luma_samples = 16};
    h.log2_diff_max_min_luma_coding_block_size = 1;
    h.pic_fields.bits.chroma_format_idc = 1; h.slice_parsing_fields.bits.IdrPicFlag = h.slice_parsing_fields.bits.RapPicFlag = 1;
    VASliceParameterBufferHEVC hs = {.slice_data_size = 7}; uint8_t hn[] = {0,0,0,1,0x26,1,0xa0};
    f.buffers[0].data = &h; f.buffers[0].size = sizeof(h); f.buffers[1].data = &hs; f.buffers[1].size = sizeof(hs); f.buffers[2].data = hn; f.buffers[2].size = sizeof(hn);
    c.codec = VAV4L2_CODEC_HEVC; c.profile = VAProfileHEVCMain;
    assert(vav4l2_stateful_packet(&c, f.batch, 3, packet, sizeof(packet), &bytes) == VA_STATUS_SUCCESS);
    cursor = 0;
    for (unsigned type = 32; type <= 34; type++) {
        assert(vav4l2_annexb_next(packet, bytes, &cursor, &unit, &length) == 1 && ((unit[4] >> 1) & 63) == type);
        r = reader(unit, length, 2); if (type == 34) assert(read_ue(&r) == 0 && read_ue(&r) == 0);
    }
    assert(vav4l2_annexb_next(packet, bytes, &cursor, &unit, &length) == 1 && length == sizeof(hn) && !memcmp(unit, hn, sizeof(hn)));
    h.num_short_term_ref_pic_sets = 1; memset(packet, 0xa5, sizeof(packet));
    assert(vav4l2_stateful_packet(&c, f.batch, 3, packet, sizeof(packet), &bytes) == VA_STATUS_ERROR_UNIMPLEMENTED && !memcmp(packet, poison, sizeof(packet)));
    h.num_short_term_ref_pic_sets = 0; h.num_long_term_ref_pic_sps = 1;
    assert(vav4l2_stateful_packet(&c, f.batch, 3, packet, sizeof(packet), &bytes) == VA_STATUS_ERROR_UNIMPLEMENTED);
    puts("stateful parameter sets, slice preservation and atomic rejection: PASS"); return 0;
}
