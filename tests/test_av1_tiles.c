/* Exercise the shipped AV1 geometry and payload validation without a VPU. */
#include <stdio.h>
#include <string.h>
#include "va_v4l2.h"

static int failures, ioctls;
static void expect(int ok, const char *tag)
{
    if (!ok) { fprintf(stderr, "FAIL %s\n", tag); failures++; }
}
static int reject_ioctl(int fd, unsigned long req, void *arg)
{
    (void)fd; (void)req; (void)arg;
    ioctls++;
    return -1;
}
static VADecPictureParameterBufferAV1 picture(unsigned w, unsigned h, unsigned cols, unsigned rows)
{
    VADecPictureParameterBufferAV1 p = {0};
    p.frame_width_minus1 = w - 1;
    p.frame_height_minus1 = h - 1;
    p.tile_cols = cols;
    p.tile_rows = rows;
    p.pic_info_fields.bits.uniform_tile_spacing_flag = 1;
    return p;
}
static void geometry(void)
{
    struct v4l2_av1_tile_info t;
    VADecPictureParameterBufferAV1 p;
    const unsigned grids[][2] = {{1,1}, {2,1}, {4,1}, {8,1}, {2,2}};
    for (unsigned g = 0; g < sizeof(grids) / sizeof(grids[0]); g++) {
        p = picture(3840, 2160, grids[g][0], grids[g][1]);
        expect(vav4l2_av1_fill_tile_info(&t, &p) == VA_STATUS_SUCCESS, "uniform-grid");
        expect(t.mi_col_starts[p.tile_cols] == 960 && t.mi_row_starts[p.tile_rows] == 540, "MI-sentinels");
        for (unsigned i = 1; i < p.tile_cols; i++)
            expect(t.mi_col_starts[i] % 16 == 0, "SB-column-alignment");
    }
    p = picture(3840, 2160, 8, 1);
    vav4l2_av1_fill_tile_info(&t, &p);
    for (unsigned i = 0; i < 8; i++)
        expect(t.mi_col_starts[i] == 128 * i && t.width_in_sbs_minus_1[i] == (i == 7 ? 3u : 7u), "4k-eight-columns");
    p.seq_info_fields.fields.use_128x128_superblock = 1;
    expect(vav4l2_av1_fill_tile_info(&t, &p) == VA_STATUS_SUCCESS && t.mi_col_starts[7] == 896, "SB128");
    p = picture(1921, 1081, 1, 1);
    expect(vav4l2_av1_fill_tile_info(&t, &p) == VA_STATUS_SUCCESS && t.mi_col_starts[1] == 482 && t.mi_row_starts[1] == 272, "odd-size-MI-rounding");
    p = picture(3840, 2160, 8, 1);
    p.superres_scale_denominator = 16;
    expect(vav4l2_av1_fill_tile_info(&t, &p) == VA_STATUS_SUCCESS && t.mi_col_starts[8] == 480 && t.mi_col_starts[1] == 64, "superres-coded-grid");
    p = picture(1920, 1080, 3, 2);
    p.pic_info_fields.bits.uniform_tile_spacing_flag = 0;
    p.width_in_sbs_minus_1[0] = 4;
    p.width_in_sbs_minus_1[1] = 9;
    p.width_in_sbs_minus_1[2] = 65535; /* Last width is inferred, per libva. */
    p.height_in_sbs_minus_1[0] = 7;
    expect(vav4l2_av1_fill_tile_info(&t, &p) == VA_STATUS_SUCCESS && t.mi_col_starts[2] == 240 && t.width_in_sbs_minus_1[2] == 14 && t.mi_row_starts[2] == 270, "nonuniform-last-SB-crop");
    p.width_in_sbs_minus_1[0] = 29;
    expect(vav4l2_av1_fill_tile_info(&t, &p) != VA_STATUS_SUCCESS, "nonuniform-overrun");
    p = picture(1280, 720, 0, 1);
    expect(vav4l2_av1_fill_tile_info(&t, &p) != VA_STATUS_SUCCESS, "zero-grid");
    p.tile_cols = 8; p.tile_rows = 8;
    expect(vav4l2_av1_fill_tile_info(&t, &p) != VA_STATUS_SUCCESS, "table-overflow");
    p = picture(64, 64, 2, 1);
    expect(vav4l2_av1_fill_tile_info(&t, &p) != VA_STATUS_SUCCESS, "empty-tile");
    p = picture(1280, 720, 2, 1); p.context_update_tile_id = 2;
    expect(vav4l2_av1_fill_tile_info(&t, &p) != VA_STATUS_SUCCESS, "bad-context-tile");
    p.context_update_tile_id = 0; p.order_hint_bits_minus_1 = 31;
    expect(vav4l2_av1_fill_tile_info(&t, &p) != VA_STATUS_SUCCESS, "bad-order-hint-bits");
}
static void payload(void)
{
    VADecPictureParameterBufferAV1 p = picture(1280, 720, 2, 2);
    VASliceParameterBufferAV1 t[4] = {0};
    const VASliceParameterBufferAV1 *tiles[4] = {&t[0], &t[1], &t[2], &t[3]};
    uint8_t data[14] = {2,0, 10,11,12, 1,0, 13,14, 1,0, 15,16, 17};
    const unsigned offsets[] = {2,7,11,13}, sizes[] = {3,2,2,1};
    struct v4l2_av1_tile_info info;
    struct v4l2_ctrl_av1_tile_group_entry e[32];
    uint32_t start, length;
    for (unsigned i = 0; i < 4; i++) {
        t[i].tile_row = i / 2; t[i].tile_column = i % 2;
        t[i].slice_data_offset = offsets[i]; t[i].slice_data_size = sizes[i];
        t[i].slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
    }
#define PREP(n, cap) vav4l2_av1_prepare_tiles(&p, tiles, n, data, sizeof(data), cap, &info, e, &start, &length)
    expect(PREP(4, 64) == VA_STATUS_SUCCESS && info.tile_size_bytes == 2 && start == 2 && length == 12 && e[1].tile_offset == 5, "tile-size-fields-preserved");
    expect(PREP(3, 64) != VA_STATUS_SUCCESS, "missing-tile");
    expect(PREP(4, 11) != VA_STATUS_SUCCESS, "output-too-small");
    t[1].tile_column = 0;
    expect(PREP(4, 64) != VA_STATUS_SUCCESS, "duplicate-tile");
    t[1].tile_column = 1; t[1].slice_data_offset = 4;
    expect(PREP(4, 64) != VA_STATUS_SUCCESS, "overlap");
    t[1].slice_data_offset = 7; t[3].slice_data_size = 0;
    expect(PREP(4, 64) != VA_STATUS_SUCCESS, "empty-payload");
    t[3].slice_data_size = 0xffffffff;
    expect(PREP(4, 64) != VA_STATUS_SUCCESS, "range-overflow");
    t[3].slice_data_size = 1; data[5] = 0;
    expect(PREP(4, 64) != VA_STATUS_SUCCESS, "invalid-size-prefix");
    data[5] = 1; t[0].slice_data_flag = VA_SLICE_DATA_FLAG_BEGIN;
    expect(PREP(4, 64) != VA_STATUS_SUCCESS, "partial-tile");
    p = picture(1280, 720, 2, 1);
    t[0].slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
    t[1].slice_data_offset = 5;
    expect(PREP(2, 64) == VA_STATUS_SUCCESS && info.tile_size_bytes == 2, "two-raw-tiles");
    t[0].slice_data_offset = 0; t[1].slice_data_offset = 3;
    expect(PREP(2, 64) != VA_STATUS_SUCCESS, "missing-size-metadata");
    p = picture(1280, 720, 1, 1);
    expect(PREP(0, 64) == VA_STATUS_SUCCESS && e[0].tile_size == sizeof(data), "single-raw-tile");
#undef PREP
}
static void reject_before_ioctl(void)
{
    struct vav4l2_context ctx = {0};
    VADecPictureParameterBufferAV1 p = picture(1280, 720, 2, 1);
    VASliceParameterBufferAV1 t[2] = {0};
    uint8_t data[16] = {0};
    struct vav4l2_buffer b[3] = {
        {.type = VAPictureParameterBufferType, .size = sizeof(p), .num_elements = 1, .data = &p},
        {.type = VASliceParameterBufferType, .size = sizeof(t[0]), .num_elements = 2, .data = t},
        {.type = VASliceDataBufferType, .size = sizeof(data), .num_elements = 1, .data = data},
    };
    struct vav4l2_buffer *buffers[] = {&b[0], &b[1], &b[2]};
    ctx.output_buf_size = 64;
    vav4l2_set_ioctl_hook(reject_ioctl);
    expect(vav4l2_av1_translate(&ctx, buffers, 3) == VA_STATUS_ERROR_INVALID_PARAMETER && ioctls == 0, "bad-payload-no-ioctl");
    b[0].size = 1;
    expect(vav4l2_av1_translate(&ctx, buffers, 3) == VA_STATUS_ERROR_INVALID_PARAMETER && ioctls == 0, "short-picture-no-ioctl");
    vav4l2_set_ioctl_hook(NULL);
}
int main(void)
{
    geometry(); payload(); reject_before_ioctl();
    printf("AV1 tile failures: %d\n", failures);
    return failures != 0;
}
