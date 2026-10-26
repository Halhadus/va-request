/* Exercise the shipped AV1 geometry and payload validation without a VPU. */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "va_v4l2.h"

static int failures, ioctls;
static int submitted, pull_error, grain_missing, grain_sent;
int __wrap_vav4l2_ensure_capture(struct vav4l2_context *c, int w, int h, uint32_t f)
{ (void)c; (void)w; (void)h; (void)f; return 0; }
int __wrap_vav4l2_decode_submit(struct vav4l2_context *c, int i, uint32_t bytes, uint64_t timestamp)
{ (void)c; (void)i; (void)bytes; (void)timestamp; submitted++; return 0; }
int __wrap_vav4l2_surface_pull_capture(struct vav4l2_context *c, struct vav4l2_surface *s, int i)
{ (void)c; (void)s; (void)i; return pull_error ? -1 : 0; }
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
static int controls(int fd, unsigned long request, void *arg)
{
    (void)fd;
    if (request == VIDIOC_QUERY_EXT_CTRL && grain_missing) { errno = EINVAL; return -1; }
    if (request == VIDIOC_S_EXT_CTRLS) {
        struct v4l2_ext_controls *batch = arg;
        for (unsigned i = 0; i < batch->count; i++) grain_sent += batch->controls[i].id == V4L2_CID_STATELESS_AV1_FILM_GRAIN;
    }
    return 0;
}
static void completion(void)
{
    uint8_t output[64], data[] = {1,2,3,4};
    VADecPictureParameterBufferAV1 p = picture(16, 16, 1, 1);
    p.primary_ref_frame = 7; p.pic_info_fields.bits.show_frame = 1;
    p.seq_info_fields.fields.subsampling_x = p.seq_info_fields.fields.subsampling_y = 1;
    struct vav4l2_buffer b[] = {{.type = VAPictureParameterBufferType, .size = sizeof(p), .data = &p},
        {.type = VASliceDataBufferType, .size = sizeof(data), .data = data}};
    struct vav4l2_buffer *batch[] = {b, b + 1};
    for (unsigned mode = 0; mode < 4; mode++) {
        struct vav4l2_surface s = {.status = VASurfaceRendering, .buf_index = -1};
        struct vav4l2_context c = {.current_surface = &s, .v4l2_fd = 8, .request_fd = 9, .streamed = 1,
            .n_free_out = 1, .output_buf_ptr = {output}, .output_buf_size = sizeof(output), .capture_bufs_allocd = 4};
        submitted = grain_sent = 0; grain_missing = mode != 2; pull_error = mode == 0;
        p.film_grain_info.film_grain_info_fields.bits.apply_grain = mode == 3;
        vav4l2_set_ioctl_hook(controls);
        VAStatus status = vav4l2_av1_translate(&c, batch, 2);
        if (!mode) expect(status == VA_STATUS_ERROR_OPERATION_FAILED && submitted == 1 && !s.has_pic && c.n_free_cap == 1, "capture-transfer-failure-is-not-success");
        if (mode == 1) expect(status == VA_STATUS_SUCCESS && submitted == 1 && !grain_sent, "grainless-node-omits-optional-control");
        if (mode == 2) expect(status == VA_STATUS_SUCCESS && submitted == 1 && grain_sent == 1, "available-grain-control-is-submitted");
        if (mode == 3) expect(status == VA_STATUS_ERROR_UNIMPLEMENTED && !submitted, "missing-grain-feature-rejects-before-submit");
    }
    vav4l2_set_ioctl_hook(NULL);
    struct vav4l2_av1_header header = {0}; uint8_t refresh;
    p.pic_info_fields.bits.frame_type = 1;
    expect(vav4l2_av1_refresh_flags(&header, &p, data, sizeof(data), &refresh) == VA_STATUS_ERROR_UNIMPLEMENTED,
        "raw-inter-tiles-never-guess-encoder-GOP-refresh");
    p.pic_info_fields.bits.frame_type = 3;
    expect(vav4l2_av1_refresh_flags(&header, &p, data, sizeof(data), &refresh) == VA_STATUS_SUCCESS && refresh == 255,
        "switch-frame-refresh-is-defined-by-specification");
}
struct bits { uint8_t data[128]; unsigned bit; };
static void put(struct bits *b, unsigned value, unsigned n)
{
    while (n--) { b->data[b->bit / 8] |= (value >> n & 1) << (7 - b->bit % 8); b->bit++; }
}
static void metadata(void)
{
    for (unsigned equal = 0; equal < 2; equal++) for (unsigned included = 0; included < 2; included++) {
        struct bits sequence = {0}, frame = {0};
        put(&sequence, 0, 3); put(&sequence, 0, 1); put(&sequence, 0, 1);
        put(&sequence, 1, 1); put(&sequence, 1, 32); put(&sequence, 30, 32);
        put(&sequence, equal, 1); if (equal) put(&sequence, 1, 1); /* UVLC zero */
        put(&sequence, 1, 1); put(&sequence, 3, 5); put(&sequence, 1, 32); put(&sequence, 5, 5); put(&sequence, 4, 5);
        put(&sequence, 0, 1); put(&sequence, 1, 5); /* two operating points */
        for (unsigned i = 0; i < 2; i++) {
            put(&sequence, i ? (1u << (included ? 2 : 1)) | (1u << 9) : 0, 12); put(&sequence, 0, 5);
            put(&sequence, 1, 1); put(&sequence, 7, 4); put(&sequence, 3, 4); put(&sequence, 0, 1);
        }
        put(&sequence, 0, 4); put(&sequence, 0, 4); put(&sequence, 0, 1); put(&sequence, 0, 1);
        put(&sequence, 1, 1); put(&sequence, 1, 4); put(&sequence, 1, 3); /* five frame-id bits */
        put(&sequence, 0, 3); put(&sequence, 0, 4); put(&sequence, 1, 1); put(&sequence, 0, 2);
        put(&sequence, 0, 1); put(&sequence, 0, 1); put(&sequence, 2, 3); /* fixed screen tools off, order hints */
        put(&frame, 0, 1); put(&frame, 1, 2); put(&frame, 1, 1);
        if (!equal) put(&frame, 17, 5);
        put(&frame, 0, 1); put(&frame, 0, 1); put(&frame, 19, 5); put(&frame, 0, 1);
        put(&frame, 3, 3); put(&frame, 2, 3); put(&frame, 1, 1); put(&frame, 37, 6);
        if (included) put(&frame, 12, 6);
        put(&frame, 0x52, 8);
        uint8_t obu[256]; unsigned seq_size = (sequence.bit + 7) / 8, frame_size = (frame.bit + 7) / 8;
        obu[0] = 0x0a; obu[1] = seq_size; memcpy(obu + 2, sequence.data, seq_size);
        unsigned at = seq_size + 2; obu[at++] = 0x1e; obu[at++] = (2 << 5) | (1 << 3); obu[at++] = frame_size;
        memcpy(obu + at, frame.data, frame_size); unsigned size = at + frame_size;
        VADecPictureParameterBufferAV1 p = {0}; p.pic_info_fields.bits.frame_type = 1; p.pic_info_fields.bits.show_frame = 1;
        p.seq_info_fields.fields.enable_order_hint = 1; p.order_hint_bits_minus_1 = 2; p.order_hint = 3; p.primary_ref_frame = 2;
        struct vav4l2_av1_header header = {0}; uint8_t refresh = 0;
        expect(vav4l2_av1_refresh_flags(&header, &p, obu, size, &refresh) == VA_STATUS_SUCCESS && refresh == 0x52 &&
            header.frame_id == 19 && header.removal_present && header.removal_time[0] == 37 && header.removal_time[1] == (included ? 12u : 0u),
            "original-OBU-timing-frame-id-and-operating-point-membership");
        struct vav4l2_av1_header saved = header;
        expect(vav4l2_av1_refresh_flags(&header, &p, obu, size - 1, &refresh) == VA_STATUS_ERROR_UNIMPLEMENTED && !memcmp(&header, &saved, sizeof(header)),
            "truncated-original-metadata-does-not-commit");
        p.primary_ref_frame++;
        expect(vav4l2_av1_refresh_flags(&header, &p, obu, size, &refresh) == VA_STATUS_ERROR_UNIMPLEMENTED && !memcmp(&header, &saved, sizeof(header)),
            "VA-and-original-header-mismatch-does-not-commit");
    }
}
int main(void)
{
    geometry(); payload(); reject_before_ioctl(); completion(); metadata();
    printf("AV1 tile failures: %d\n", failures);
    return failures != 0;
}
