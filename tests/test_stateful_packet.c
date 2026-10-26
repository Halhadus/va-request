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
static unsigned av1_obus(const uint8_t *packet, size_t bytes, unsigned wanted)
{
    size_t pos = 0; unsigned count = 0;
    while (pos < bytes) {
        unsigned type = packet[pos++] >> 3, shift = 0; size_t length = 0;
        for (;;) { assert(pos < bytes && shift < 32); unsigned v = packet[pos++]; length |= (size_t)(v & 127) << shift; if (!(v & 128)) break; shift += 7; }
        assert(length <= bytes - pos); pos += length; count += type == wanted;
    }
    return count;
}
static void av1_packets(void)
{
    struct vav4l2_context c = {.codec = VAV4L2_CODEC_AV1, .profile = VAProfileAV1Profile0, .width = 128, .height = 128};
    VADecPictureParameterBufferAV1 p = {.frame_width_minus1 = 127, .frame_height_minus1 = 127,
        .tile_cols = 1, .tile_rows = 1, .primary_ref_frame = 7};
    p.seq_info_fields.fields.subsampling_x = p.seq_info_fields.fields.subsampling_y = 1;
    p.pic_info_fields.bits.show_frame = p.pic_info_fields.bits.uniform_tile_spacing_flag = 1;
    for (unsigned i = 0; i < 8; i++) p.ref_frame_map[i] = VA_INVALID_SURFACE;
    VASliceParameterBufferAV1 tile = {.slice_data_size = 4}; uint8_t raw[4] = {1,2,3,4}, packet[32768], poison[32768];
    struct vav4l2_buffer b[] = {{.type = VAPictureParameterBufferType, .data = &p, .size = sizeof(p)},
        {.type = VASliceParameterBufferType, .data = &tile, .size = sizeof(tile)},
        {.type = VASliceDataBufferType, .data = raw, .size = sizeof(raw)}};
    struct vav4l2_buffer *batch[] = {b, b + 1, b + 2};
    struct vav4l2_packet_layout layout; size_t bytes = 0;
    struct vav4l2_av1_header metadata = {0};
    for (unsigned frame = 0; frame < 10; frame++) {
        c.current_surface_id = p.current_frame = frame; p.pic_info_fields.bits.frame_type = frame ? 1 : 0;
        if (frame && frame <= 8) p.ref_frame_map[frame - 1] = frame - 1;
        if (frame == 9) p.ref_frame_map[0] = 8;
        struct vav4l2_packet_state prepared = {0};
        assert(vav4l2_stateful_packet_prepare(&c, batch, 3, packet, sizeof(packet), &bytes, &prepared, &layout) == VA_STATUS_SUCCESS);
        assert(av1_obus(packet, bytes, 2) == 1 && av1_obus(packet, bytes, 1) == (frame ? 0u : 1u));
        assert(av1_obus(packet, bytes, 6) == (frame == 9 ? 2u : 1u) && layout.frame_size[0] == bytes && !layout.frame_size[1]);
        uint8_t refresh;
        assert(vav4l2_av1_refresh_flags(&metadata, &p, packet, bytes, &refresh) == VA_STATUS_SUCCESS &&
            refresh == (!frame ? 255u : frame < 8 ? 1u << frame : 0u));
        assert(prepared.av1 != c.packet_state.av1); vav4l2_packet_state_commit(&c, &prepared);
    }
    memset(packet, 0xa5, sizeof(packet)); memcpy(poison, packet, sizeof(packet)); bytes = 55;
    struct vav4l2_av1_packet *committed = c.packet_state.av1;
    assert(vav4l2_stateful_packet(&c, batch, 3, packet, 8, &bytes) != VA_STATUS_SUCCESS && bytes == 55 &&
        c.packet_state.av1 == committed && !memcmp(packet, poison, sizeof(packet)));
    p.film_grain_info.film_grain_info_fields.bits.apply_grain = 1;
    assert(vav4l2_stateful_packet(&c, batch, 3, packet, sizeof(packet), &bytes) == VA_STATUS_SUCCESS);
    p.film_grain_info.num_y_points = 15;
    assert(vav4l2_stateful_packet(&c, batch, 3, packet, sizeof(packet), &bytes) == VA_STATUS_ERROR_INVALID_PARAMETER);
    p.film_grain_info.num_y_points = 0;
    p.film_grain_info.film_grain_info_fields.bits.apply_grain = 0; tile.tile_column = 1;
    assert(vav4l2_stateful_packet(&c, batch, 3, packet, sizeof(packet), &bytes) == VA_STATUS_ERROR_INVALID_PARAMETER);
    vav4l2_packet_state_release(&c.packet_state); assert(!c.packet_state.av1);
}
int main(void)
{
    av1_packets();
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
    f.pic.seq_fields.bits.pic_order_cnt_type = 1; f.pic.seq_fields.bits.delta_pic_order_always_zero_flag = 1;
    memset(f.nal + 5, 0, sizeof(f.nal) - 5);
    const char *poc1 = "10110010000001001";
    for (unsigned i = 0; poc1[i]; i++) if (poc1[i] == '1') f.nal[5 + i / 8] |= 128 >> (i % 8);
    assert(vav4l2_stateful_packet(&c, f.batch, 3, packet, sizeof(packet), &bytes) == VA_STATUS_SUCCESS);
    cursor = 0; assert(vav4l2_annexb_next(packet, bytes, &cursor, &unit, &length) == 1);
    r = reader(unit, length, 1); read_bits(&r, 24); read_ue(&r); read_ue(&r); read_ue(&r); read_ue(&r); read_bits(&r, 2);
    assert(read_ue(&r) == 0 && read_ue(&r) == 0 && read_ue(&r) == 12);
    stateful_fixture(&f);
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
    h.pic_width_in_luma_samples = h.pic_height_in_luma_samples = c.width = c.height = 4096;
    h.sps_max_dec_pic_buffering_minus1 = 15;
    assert(vav4l2_stateful_packet(&c, f.batch, 3, packet, sizeof(packet), &bytes) == VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
    h.sps_max_dec_pic_buffering_minus1 = 11;
    assert(vav4l2_stateful_packet(&c, f.batch, 3, packet, sizeof(packet), &bytes) == VA_STATUS_SUCCESS);
    cursor = 0; assert(vav4l2_annexb_next(packet, bytes, &cursor, &unit, &length) == 1);
    assert(vav4l2_annexb_next(packet, bytes, &cursor, &unit, &length) == 1);
    r = reader(unit, length, 2); assert(r.data[12] == 180); /* DPB and luma limits require level 6.0. */
    h.pic_width_in_luma_samples = h.pic_height_in_luma_samples = c.width = c.height = 16;
    h.sps_max_dec_pic_buffering_minus1 = 0;
    h.num_short_term_ref_pic_sets = 1; memset(packet, 0xa5, sizeof(packet));
    assert(vav4l2_stateful_packet(&c, f.batch, 3, packet, sizeof(packet), &bytes) == VA_STATUS_ERROR_UNIMPLEMENTED && !memcmp(packet, poison, sizeof(packet)));
    h.num_short_term_ref_pic_sets = 0; h.num_long_term_ref_pic_sps = 1;
    assert(vav4l2_stateful_packet(&c, f.batch, 3, packet, sizeof(packet), &bytes) == VA_STATUS_ERROR_UNIMPLEMENTED);
    puts("stateful parameter sets, slice preservation and atomic rejection: PASS"); return 0;
}
