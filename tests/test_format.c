/*
 * Drive shipped pixel-format helpers and VPP capability queries.
 */

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <limits.h>
#include <string.h>
#include <stdint.h>
#include <va/va.h>
#include <va/va_vpp.h>
#include <linux/videodev2.h>
#include <drm_fourcc.h>

#include "va_v4l2.h"

static int g_fail;

static void expect_true(int cond, const char *tag)
{
    if (!cond) {
        fprintf(stderr, "FAIL %s\n", tag);
        g_fail++;
    } else {
        printf("OK %s\n", tag);
    }
}

static void test_fourcc_map(void)
{
    expect_true(vav4l2_capture_fourcc_from_rt(VA_RT_FORMAT_YUV420) == V4L2_PIX_FMT_NV12,
                "rt-420-nv12");
    expect_true(vav4l2_capture_fourcc_from_rt(VA_RT_FORMAT_YUV420_10) == V4L2_PIX_FMT_NV15,
                "rt-42010-nv15");
    expect_true(vav4l2_capture_fourcc_from_rt(VA_RT_FORMAT_YUV422) == V4L2_PIX_FMT_NV16,
                "rt-422-nv16");
    expect_true(vav4l2_capture_fourcc_from_rt(VA_RT_FORMAT_YUV422_10) == V4L2_PIX_FMT_NV20,
                "rt-42210-nv20");
    expect_true(vav4l2_capture_fourcc_from_sps(0, 1) == V4L2_PIX_FMT_NV12, "sps-8-420");
    expect_true(vav4l2_capture_fourcc_from_sps(2, 1) == V4L2_PIX_FMT_NV15, "sps-10-420");
    expect_true(vav4l2_capture_fourcc_from_sps(0, 2) == V4L2_PIX_FMT_NV16, "sps-8-422");
    expect_true(vav4l2_capture_fourcc_from_sps(2, 2) == V4L2_PIX_FMT_NV20, "sps-10-422");
    expect_true(vav4l2_va_fourcc_for_capture(V4L2_PIX_FMT_NV15) == VA_FOURCC_P010,
                "va-p010");
    expect_true(vav4l2_va_fourcc_for_capture(V4L2_PIX_FMT_NV12) == VA_FOURCC_NV12,
                "va-nv12");
    expect_true(vav4l2_drm_fourcc_for_capture(V4L2_PIX_FMT_NV12) == DRM_FORMAT_NV12,
                "drm-nv12");
    expect_true(vav4l2_capture_plane_size(V4L2_PIX_FMT_NV12, 16, 16) == 16 * 16 * 3 / 2,
                "plane-nv12");
    expect_true(vav4l2_capture_plane_size(V4L2_PIX_FMT_NV16, 16, 16) == 16 * 16 * 2,
                "plane-nv16");
    expect_true(vav4l2_va_image_size(VA_FOURCC_NV12, 320, 240) == 320 * 240 * 3 / 2,
                "image-nv12");
    expect_true(vav4l2_va_image_size(VA_FOURCC_YUY2, 640, 240) == 640 * 240,
                "image-yuy2");
    const uint32_t planar[] = {VA_FOURCC_I420, VA_FOURCC_YV12, VA_FOURCC_422H, VA_FOURCC_YV16};
    for (unsigned i = 0; i < sizeof(planar) / sizeof(planar[0]); i++) {
        expect_true(vav4l2_default_image_stride(planar[i], 64) == 128, "planar-half-width-chroma-row-alignment");
        expect_true(vav4l2_default_image_stride(planar[i], 129) == 256, "planar-chroma-alignment-across-boundary");
        expect_true(vav4l2_default_image_stride(planar[i], INT_MAX) == 2147483648u, "planar-stride-without-signed-overflow");
    }
    expect_true(vav4l2_default_image_stride(VA_FOURCC_P010, 128) == 256,
                "stride-p010");
    expect_true(vav4l2_default_image_stride(VA_FOURCC_NV12, 128) == 128,
                "stride-nv12");
}

static void test_copy_nv12(void)
{
    uint8_t src[12];
    uint8_t dst[24];
    int i;

    memset(src, 0, sizeof(src));
    memset(dst, 0xaa, sizeof(dst));
    /* 2x2 NV12, stride 2, aligned_h 2: Y=4, UV=2 */
    src[0] = 10; src[1] = 11;
    src[2] = 12; src[3] = 13;
    src[4] = 20; src[5] = 21;
    vav4l2_copy_nv12(dst, 4, 2, src, 2, 2, 2, 2);
    expect_true(dst[0] == 10 && dst[1] == 11, "copy-y0");
    expect_true(dst[4] == 12 && dst[5] == 13, "copy-y1");
    expect_true(dst[8] == 20 && dst[9] == 21, "copy-uv");
    for (i = 2; i < 4; i++)
        expect_true(dst[i] == 0xaa, "copy-y0-pad");
}

static void test_nv15_to_p010(void)
{
    uint8_t src[15];
    uint16_t dst[4 * 2 + 4]; /* 2 Y rows + 1 UV row at stride 8 bytes = 4 u16 */
    uint8_t *d8 = (uint8_t *)dst;

    memset(src, 0xff, sizeof(src));
    memset(dst, 0, sizeof(dst));
    /* width=4, height=2, NV15 stride=5, aligned_h=2 */
    vav4l2_nv15_to_p010(d8, 8, 2, src, 5, 2, 4, 2);
    expect_true(dst[0] == (uint16_t)(0x3ffu << 6), "p010-y0");
}

static void test_nv16_to_yuy2(void)
{
    uint8_t src[8];
    uint8_t dst[8];

    memset(src, 0, sizeof(src));
    src[0] = 1; src[1] = 2; /* Y */
    src[2] = 3; src[3] = 4;
    src[4] = 10; src[5] = 11; /* UV row 0 */
    src[6] = 12; src[7] = 13;
    vav4l2_nv16_to_yuy2(dst, 4, src, 2, 2, 2, 2);
    expect_true(dst[0] == 1 && dst[1] == 10 && dst[2] == 2 && dst[3] == 11,
                "nv16-yuy2-row0");
}

static void test_vpp_query(void)
{
    unsigned n = 7;
    VAProcFilterType filters[4];
    VAProcPipelineCaps caps;

    expect_true(vav4l2_vpp_query_filters(NULL, &n) == VA_STATUS_SUCCESS && n == 0,
                "vpp-filters-count");
    n = 4;
    expect_true(vav4l2_vpp_query_filters(filters, &n) == VA_STATUS_SUCCESS && n == 0,
                "vpp-filters-empty");
    expect_true(vav4l2_vpp_query_filter_caps(VAProcFilterNoiseReduction, NULL, &n)
                == VA_STATUS_ERROR_UNSUPPORTED_FILTER,
                "vpp-denoise-unsupported");
    expect_true(vav4l2_vpp_query_pipeline_caps(NULL) == VA_STATUS_ERROR_INVALID_PARAMETER,
                "vpp-caps-null");
    expect_true(vav4l2_vpp_query_pipeline_caps(&caps) == VA_STATUS_SUCCESS, "vpp-caps");
    expect_true(caps.max_input_width == 8192 && caps.max_output_width == 8192,
                "vpp-max-size");
    expect_true(caps.rotation_flags & (1u << VA_ROTATION_90), "vpp-rot90");
    expect_true(caps.mirror_flags & VA_MIRROR_HORIZONTAL, "vpp-mirror");
}

static void test_annexb_bounds(void)
{
    const uint8_t nal[] = {0x65, 0xaa, 0xbb}; const uint8_t *parts[] = {nal, nal};
    uint32_t sizes[] = {3, 3}; uint8_t out[16]; memset(out, 0xa5, sizeof(out));
    expect_true(vav4l2_annexb_concat(parts, sizes, 2, 3, out, 11) == 0 && out[0] == 0xa5,
                "annexb-capacity-failure-does-not-partially-write");
    expect_true(vav4l2_annexb_concat(parts, sizes, 2, 3, out, 12) == 12 && out[2] == 1 && out[3] == 0x65,
                "annexb-exact-capacity");
    sizes[0] = UINT32_MAX;
    expect_true(vav4l2_annexb_concat(parts, sizes, 1, 3, out, sizeof(out)) == 0, "annexb-large-count-preflight");
    expect_true(vav4l2_annexb_concat(NULL, sizes, 1, 3, out, sizeof(out)) == 0 &&
                vav4l2_annexb_concat(parts, sizes, -1, 3, out, sizeof(out)) == 0 &&
                vav4l2_annexb_concat(parts, sizes, 1, 8, out, sizeof(out)) == 0, "annexb-input-contract");
}
static void tiled_layouts(void)
{
    const uint32_t formats[] = {V4L2_PIX_FMT_NV12_4L4, V4L2_PIX_FMT_NV12_16L16, V4L2_PIX_FMT_NV12_32L32, V4L2_PIX_FMT_NV12_8L128, V4L2_PIX_FMT_P010_4L4};
    for (unsigned i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
        unsigned t = vav4l2_capture_tile(formats[i]), th = i == 3 ? 128 : i == 4 ? 4 : t;
        unsigned bytes = i == 4 ? 2 : 1, stride = t*2, rows = th*2;
        uint32_t size = vav4l2_capture_plane_size(formats[i], stride, rows);
        uint8_t *source = malloc(size), *result = malloc(size); assert(source && result); memset(result, 0xa5, size);
        for (unsigned plane = 0; plane < 2; plane++) for (unsigned y = 0; y < (plane ? rows/2 : rows); y++) for (unsigned x = 0; x < stride; x++) {
            unsigned offset = (y/th)*stride*th+(x/t)*t*th+(y%th)*t+x%t;
            source[(plane ? stride*rows : 0)+offset] = (uint8_t)(plane*71+y*13+x);
        }
        assert(!vav4l2_detile_capture(result, stride, size, source, size, stride, rows, stride/bytes, rows, formats[i]));
        for (unsigned y = 0; y < rows*3/2; y++) for (unsigned x = 0; x < stride; x++)
            assert(result[y*stride+x] == (uint8_t)((y >= rows ? 71 : 0)+(y >= rows ? y-rows : y)*13+x));
        memset(result, 0xa5, size);
        assert(vav4l2_detile_capture(result, stride, size, source, size-1, stride, rows, stride/bytes, rows, formats[i]) < 0 && result[0] == 0xa5);
        assert(!vav4l2_capture_plane_size(formats[i], stride-1, rows)); free(source); free(result);
    }
}
int main(void)
{
    tiled_layouts();
    test_fourcc_map();
    test_copy_nv12();
    test_nv15_to_p010();
    test_nv16_to_yuy2();
    test_vpp_query();
    test_annexb_bounds();
    return g_fail ? 1 : 0;
}
