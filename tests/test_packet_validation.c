/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <stdio.h>
#include <stdlib.h>
#include "stateful_fixture.h"
#include "codecs/bytestream.h"
static uint32_t random_value(uint32_t *state)
{
    *state ^= *state << 13; *state ^= *state >> 17; *state ^= *state << 5; return *state;
}
static int no_ioctl(int fd, unsigned long request, void *arg)
{
    (void)fd; (void)request; (void)arg; assert(!"packet validation must precede device access"); return -1;
}
static void bit_writer_bounds(void)
{
    struct bits b = {.bit = sizeof(b.data) * 8 - 1}; put(&b, 1, 1);
    assert(!b.error && b.bit == sizeof(b.data) * 8 && b.data[sizeof(b.data) - 1] == 1);
    put(&b, 1, 1); assert(b.error && b.bit == sizeof(b.data) * 8);
    b = (struct bits){.bit = SIZE_MAX}; put(&b, 1, 1);
    assert(b.error && b.bit == SIZE_MAX && !b.data[0]);
    uint8_t output[16]; memset(output, 0xa5, sizeof(output)); size_t used = 0;
    assert(nal(&b, 7, 0, output, sizeof(output), &used) == -1 && !used && output[0] == 0xa5);
}
static void av1_mutations(uint32_t *seed, unsigned iterations)
{
    for (unsigned i = 0; i < iterations; i++) {
        struct vav4l2_context c = {.codec = VAV4L2_CODEC_AV1, .width = 128, .height = 128};
        VADecPictureParameterBufferAV1 p = {.frame_width_minus1 = 127, .frame_height_minus1 = 127, .tile_cols = 1, .tile_rows = 1, .primary_ref_frame = 7};
        p.seq_info_fields.fields.subsampling_x = p.seq_info_fields.fields.subsampling_y = 1;
        p.pic_info_fields.bits.show_frame = p.pic_info_fields.bits.uniform_tile_spacing_flag = 1;
        ((uint8_t *)&p)[random_value(seed) % sizeof(p)] ^= random_value(seed);
        VASliceParameterBufferAV1 tile = {.slice_data_size = 4}; uint8_t data[] = {1,2,3,4};
        if (i & 1) tile.slice_data_offset = random_value(seed);
        struct vav4l2_buffer b[] = {{.type = VAPictureParameterBufferType, .size = sizeof(p), .data = &p},
            {.type = VASliceParameterBufferType, .size = sizeof(tile), .data = &tile},
            {.type = VASliceDataBufferType, .size = sizeof(data), .data = data}};
        struct vav4l2_buffer *batch[] = {b, b + 1, b + 2};
        uint8_t output[16384], poison[16384]; memset(output, 0xa5, sizeof(output)); memcpy(poison, output, sizeof(output)); size_t bytes = 17;
        struct vav4l2_packet_state next = {0};
        VAStatus status = vav4l2_stateful_packet_prepare(&c, batch, 3, output, sizeof(output), &bytes, &next, NULL);
        if (status != VA_STATUS_SUCCESS) assert(bytes == 17 && !next.av1 && !memcmp(output, poison, sizeof(output)));
        else { assert(bytes && bytes <= sizeof(output) && next.av1); vav4l2_packet_state_release(&next); }
        assert(!c.packet_state.av1);
    }
}
int main(int argc, char **argv)
{
    bit_writer_bounds();
    uint32_t seed = argc > 1 ? strtoul(argv[1], NULL, 0) : 0x57a9d103;
    if (!seed) return 2;
    printf("packet mutation seed=%u\n", seed); vav4l2_set_ioctl_hook(no_ioctl);
    unsigned iterations = argc > 2 ? strtoul(argv[2], NULL, 0) : 1024;
    if (!iterations || iterations > 10000000) return 2;
    av1_mutations(&seed, (iterations + 1) / 2);
    for (unsigned i = 0; i < iterations; i++) {
        struct stateful_fixture f; stateful_fixture(&f);
        struct vav4l2_context ctx = {.codec = VAV4L2_CODEC_H264, .profile = VAProfileH264High, .width = 16, .height = 16};
        VAPictureParameterBufferHEVC hevc = {.pic_width_in_luma_samples = 16, .pic_height_in_luma_samples = 16};
        VASliceParameterBufferHEVC hs = {.slice_data_size = 8, .slice_data_byte_offset = 3};
        hevc.pic_fields.bits.chroma_format_idc = 1; hevc.log2_diff_max_min_luma_coding_block_size = 1;
        hevc.slice_parsing_fields.bits.IdrPicFlag = hevc.slice_parsing_fields.bits.RapPicFlag = 1;
        for (unsigned j = 0; j < 15; j++) hevc.ReferenceFrames[j].flags = VA_PICTURE_HEVC_INVALID;
        hs.LongSliceFlags.fields.slice_type = 2;
        unsigned n = random_value(&seed), index = random_value(&seed) % sizeof(f.pic);
        ((uint8_t *)&f.pic)[index] ^= n;
        f.nal[5 + random_value(&seed) % 3] ^= random_value(&seed);
        if (i & 1) f.slice.slice_data_bit_offset = random_value(&seed);
        if (i & 2) f.slice.slice_data_offset = random_value(&seed);
        if (i & 4) {
            ctx.codec = VAV4L2_CODEC_HEVC; ctx.profile = VAProfileHEVCMain;
            memcpy(f.nal, (uint8_t[]){0,0,0,1,0x26,1,0xaf,0x80}, 8);
            f.buffers[0].data = &hevc; f.buffers[0].size = sizeof(hevc);
            f.buffers[1].data = &hs; f.buffers[1].size = sizeof(hs);
            ((uint8_t *)&hevc)[random_value(&seed) % sizeof(hevc)] ^= random_value(&seed);
            if (i & 1) hs.slice_data_byte_offset = random_value(&seed);
            else f.nal[6] ^= random_value(&seed);
        }
        uint8_t packet[16384], poison[16384]; memset(packet, 0xa5, sizeof(packet)); memcpy(poison, packet, sizeof(packet));
        struct vav4l2_packet_state next, unchanged; memset(&next, 0xa5, sizeof(next)); memcpy(&unchanged, &next, sizeof(next));
        size_t bytes = 17;
        VAStatus status = vav4l2_stateful_packet_prepare(&ctx, f.batch, 3, packet, sizeof(packet), &bytes, &next, NULL);
        if (status != VA_STATUS_SUCCESS) {
            assert(bytes == 17 && !memcmp(packet, poison, sizeof(packet)) && !memcmp(&next, &unchanged, sizeof(next)));
        } else {
            size_t cursor = 0, size; const uint8_t *nal; unsigned count = 0; int rc;
            while ((rc = vav4l2_annexb_next(packet, bytes, &cursor, &nal, &size)) == 1) { assert(size >= 5); count++; }
            assert(rc == 0 && count >= 3 && bytes <= sizeof(packet));
        }
        assert(!ctx.packet_state.h264_valid && !ctx.packet_state.hevc_started);
    }
    vav4l2_set_ioctl_hook(NULL); return 0;
}
