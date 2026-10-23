/* Public image metadata defines the oracle; exercise real driver callbacks. */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "va_request.h"
extern VAStatus __vaDriverInit_1_20(VADriverContextP ctx);
static int failures;
static void expect(int ok, const char *tag)
{
    if (!ok) { fprintf(stderr, "FAIL %s\n", tag); failures++; }
}
static unsigned bytes_per_pixel(unsigned fcc)
{
    return fcc == VA_FOURCC_P010 || fcc == VA_FOURCC_YUY2 || fcc == VA_FOURCC_UYVY || fcc == VA_FOURCC_VYUY || fcc == VA_FOURCC_YVYU || fcc == VA_FOURCC_RGB565 ? 2 :
        fcc == VA_FOURCC_Y210 || fcc == VA_FOURCC_BGRX || fcc == VA_FOURCC_BGRA || fcc == VA_FOURCC_ARGB || fcc == VA_FOURCC_RGBA || fcc == VA_FOURCC_RGBX ? 4 : 1;
}
static void compare(const VAImage *a, const unsigned char *ap, unsigned ax, unsigned ay,
                    const VAImage *b, const unsigned char *bp, unsigned width, unsigned height)
{
    unsigned bpp = bytes_per_pixel(a->format.fourcc);
    for (unsigned p = 0; p < a->num_planes; p++) {
        unsigned planar422 = a->format.fourcc == VA_FOURCC_422H || a->format.fourcc == VA_FOURCC_YV16;
        unsigned h = p && !planar422 ? (height + 1) / 2 : height;
        unsigned x = p && a->num_planes == 3 ? ax / 2 : ax * bpp;
        unsigned y = p && !planar422 ? ay / 2 : ay;
        unsigned w = p && a->num_planes == 3 ? (width + 1) / 2 : width * bpp;
        for (unsigned row = 0; row < h; row++)
            expect(!memcmp(ap + a->offsets[p] + (size_t)(y + row) * a->pitches[p] + x,
                           bp + b->offsets[p] + (size_t)row * b->pitches[p], w), "visible-plane-pixels");
    }
}
static void roundtrip(VADriverContextP ctx, unsigned rt, unsigned fcc)
{
    VASurfaceID sid = VA_INVALID_ID;
    VAImage in, out, crop;
    VAImageFormat format = {.fourcc = fcc};
    VASurfaceAttrib attr = {.type = VASurfaceAttribPixelFormat, .flags = VA_SURFACE_ATTRIB_SETTABLE,
        .value = {.type = VAGenericValueTypeInteger, .value.i = fcc}};
    expect(ctx->vtable->vaCreateSurfaces2(ctx, rt, 6, 6, &sid, 1, &attr, 1) == VA_STATUS_SUCCESS, "create-surface-format");
    if (sid == VA_INVALID_ID) return;
    if (ctx->vtable->vaCreateImage(ctx, &format, 6, 6, &in) ||
        ctx->vtable->vaCreateImage(ctx, &format, 8, 8, &out) ||
        ctx->vtable->vaCreateImage(ctx, &format, 2, 2, &crop)) { expect(0, "create-images"); return; }
    unsigned char *ip, *op, *cp;
    ctx->vtable->vaMapBuffer(ctx, in.buf, (void **)&ip);
    ctx->vtable->vaMapBuffer(ctx, out.buf, (void **)&op);
    ctx->vtable->vaMapBuffer(ctx, crop.buf, (void **)&cp);
    for (unsigned i = 0; i < in.data_size; i++) ip[i] = (i * 17 + 23) % 251;
    memset(op, 0xa5, out.data_size);
    expect(ctx->vtable->vaPutImage(ctx, sid, in.image_id, 0, 0, 6, 6, 0, 0, 6, 6) == VA_STATUS_SUCCESS,
           "upload-whole-format");
    expect(ctx->vtable->vaGetImage(ctx, sid, 0, 0, 6, 6, out.image_id) == VA_STATUS_SUCCESS, "download-whole-format");
    compare(&in, ip, 0, 0, &out, op, 6, 6);
    expect(op[out.pitches[0] * 7] == 0xa5, "readback-keeps-destination-outside-rectangle");
    expect(ctx->vtable->vaGetImage(ctx, sid, 2, 2, 2, 2, crop.image_id) == VA_STATUS_SUCCESS, "download-offset-rectangle");
    compare(&in, ip, 2, 2, &crop, cp, 2, 2);
    if (rt == VA_RT_FORMAT_RGB16 || rt == VA_RT_FORMAT_RGB32 || fcc == VA_FOURCC_UYVY || fcc == VA_FOURCC_VYUY || fcc == VA_FOURCC_YVYU || fcc == VA_FOURCC_422H || fcc == VA_FOURCC_YV16) {
        VAImage derived; unsigned char *dp;
        VAStatus status = ctx->vtable->vaDeriveImage(ctx, sid, &derived);
        expect(status == VA_STATUS_SUCCESS, "derive-packed422-cpu-surface");
        if (status == VA_STATUS_SUCCESS) {
            expect(derived.format.fourcc == fcc &&
                   derived.num_planes == (fcc == VA_FOURCC_422H || fcc == VA_FOURCC_YV16 ? 3u : 1u),
                   "derived-packed422-reports-the-packed-allocation");
            if (rt == VA_RT_FORMAT_RGB16 || rt == VA_RT_FORMAT_RGB32)
                expect(derived.format.byte_order == VA_LSB_FIRST && derived.format.bits_per_pixel == bytes_per_pixel(fcc) * 8 &&
                    derived.format.red_mask && derived.format.green_mask && derived.format.blue_mask,
                    "derived-RGB-format-reports-byte-order-storage-and-channel-masks");
            status = ctx->vtable->vaMapBuffer(ctx, derived.buf, (void **)&dp);
            expect(status == VA_STATUS_SUCCESS, "map-derived-packed422");
            if (status == VA_STATUS_SUCCESS) {
                compare(&in, ip, 0, 0, &derived, dp, 6, 6);
                ctx->vtable->vaUnmapBuffer(ctx, derived.buf);
            }
            ctx->vtable->vaDestroyImage(ctx, derived.image_id);
        }
    }
    expect(ctx->vtable->vaPutImage(ctx, sid, in.image_id, 2, 2, 2, 2, 0, 0, 2, 2) == VA_STATUS_SUCCESS &&
           ctx->vtable->vaGetImage(ctx, sid, 0, 0, 2, 2, crop.image_id) == VA_STATUS_SUCCESS, "upload-source-and-destination-rectangles");
    compare(&in, ip, 2, 2, &crop, cp, 2, 2);
    expect(ctx->vtable->vaGetImage(ctx, sid, -1, 0, 2, 2, crop.image_id) == VA_STATUS_ERROR_INVALID_PARAMETER &&
           ctx->vtable->vaPutImage(ctx, sid, in.image_id, INT_MAX, 0, 2, 2, 0, 0, 2, 2) == VA_STATUS_ERROR_INVALID_PARAMETER,
           "rectangle-overflow-and-negative-origin");
    ctx->vtable->vaDestroyImage(ctx, in.image_id); ctx->vtable->vaDestroyImage(ctx, out.image_id);
    ctx->vtable->vaDestroyImage(ctx, crop.image_id); ctx->vtable->vaDestroySurfaces(ctx, &sid, 1);
}
static void conversion(VADriverContextP ctx, unsigned in_fcc, unsigned surface_fcc)
{
    VASurfaceID sid;
    VAImage in, out;
    VAImageFormat format = {.fourcc = in_fcc};
    VASurfaceAttrib attr = {.type = VASurfaceAttribPixelFormat, .flags = VA_SURFACE_ATTRIB_SETTABLE,
        .value = {.type = VAGenericValueTypeInteger, .value.i = surface_fcc}};
    expect(ctx->vtable->vaCreateSurfaces2(ctx, in_fcc == VA_FOURCC_422H || in_fcc == VA_FOURCC_YV16 ?
           VA_RT_FORMAT_YUV422 : VA_RT_FORMAT_YUV420, 6, 6, &sid, 1, &attr, 1) == VA_STATUS_SUCCESS,
           "create-conversion-surface");
    if (ctx->vtable->vaCreateImage(ctx, &format, 6, 6, &in) || ctx->vtable->vaCreateImage(ctx, &format, 6, 6, &out)) {
        expect(0, "conversion-images"); return;
    }
    unsigned char *ip, *op;
    ctx->vtable->vaMapBuffer(ctx, in.buf, (void **)&ip); ctx->vtable->vaMapBuffer(ctx, out.buf, (void **)&op);
    for (unsigned i = 0; i < in.data_size; i++) ip[i] = (i * 29 + 61) % 251;
    expect(ctx->vtable->vaPutImage(ctx, sid, in.image_id, 0, 0, 6, 6, 0, 0, 6, 6) == VA_STATUS_SUCCESS &&
           ctx->vtable->vaGetImage(ctx, sid, 0, 0, 6, 6, out.image_id) == VA_STATUS_SUCCESS, "nv12-i420-roundtrip");
    compare(&in, ip, 0, 0, &out, op, 6, 6);
    expect(ctx->vtable->vaPutImage(ctx, sid, in.image_id, 1, 0, 2, 2, 0, 0, 2, 2) == VA_STATUS_ERROR_INVALID_PARAMETER,
           "chroma-origin-must-align");
    ctx->vtable->vaDestroyImage(ctx, in.image_id); ctx->vtable->vaDestroyImage(ctx, out.image_id);
    ctx->vtable->vaDestroySurfaces(ctx, &sid, 1);
}
static void decoded_partial(VADriverContextP ctx)
{
    struct vareq_driver_data *dd = ctx->pDriverData;
    VASurfaceID sid;
    VAImage in, out;
    VAImageFormat format = {.fourcc = VA_FOURCC_NV12};
    unsigned char capture[192]; memset(capture, 0x11, 128); memset(capture + 128, 0x66, 64);
    if (ctx->vtable->vaCreateSurfaces(ctx, 6, 6, VA_RT_FORMAT_YUV420, 1, &sid) ||
        ctx->vtable->vaCreateImage(ctx, &format, 2, 2, &in) || ctx->vtable->vaCreateImage(ctx, &format, 6, 6, &out)) {
        expect(0, "partial-decode-setup"); return;
    }
    struct vareq_surface *s = dd->surfaces[sid];
    s->has_pic = 1; s->buf_index = 0; s->cap_view = capture; s->cap_fourcc = V4L2_PIX_FMT_NV12;
    s->stride = 16; s->aligned_h = 8; s->last_writer = VAREQ_WRITER_MEMFD;
    unsigned char *ip, *op;
    ctx->vtable->vaMapBuffer(ctx, in.buf, (void **)&ip); memset(ip, 0x77, in.data_size);
    expect(ctx->vtable->vaPutImage(ctx, sid, in.image_id, 0, 0, 2, 2, 2, 2, 2, 2) == VA_STATUS_SUCCESS,
           "partial-upload-preserves-decoded-frame");
    expect(ctx->vtable->vaGetImage(ctx, sid, 0, 0, 6, 6, out.image_id) == VA_STATUS_SUCCESS, "partial-upload-readback");
    ctx->vtable->vaMapBuffer(ctx, out.buf, (void **)&op);
    expect(op[0] == 0x11 && op[out.pitches[0] * 2 + 2] == 0x77 && op[out.pitches[0] * 5 + 5] == 0x11 &&
           op[out.offsets[1]] == 0x66 && op[out.offsets[1] + out.pitches[1] + 2] == 0x77,
           "partial-upload-keeps-uncopied-luma-and-chroma");
    s->status = VASurfaceSkipped;
    expect(ctx->vtable->vaPutImage(ctx, sid, out.image_id, 0, 0, 6, 6, 0, 0, 6, 6) == VA_STATUS_SUCCESS &&
           s->status == VASurfaceReady, "full-upload-replaces-failed-content");
    s->buf_index = -1; s->cap_view = NULL;
    ctx->vtable->vaDestroyImage(ctx, in.image_id); ctx->vtable->vaDestroyImage(ctx, out.image_id);
    ctx->vtable->vaDestroySurfaces(ctx, &sid, 1);
}
static void overlapping_readback(VADriverContextP ctx)
{
    VASurfaceID sid; VAImage derived;
    if (ctx->vtable->vaCreateSurfaces(ctx, 8, 4, VA_RT_FORMAT_YUV420, 1, &sid)) { failures++; return; }
    struct vareq_surface *s = ((struct vareq_driver_data *)ctx->pDriverData)->surfaces[sid];
    if (vareq_surface_ensure_cpu(s) || ctx->vtable->vaDeriveImage(ctx, sid, &derived)) { failures++; return; }
    unsigned char *data, *before = malloc(derived.data_size);
    if (!before || ctx->vtable->vaMapBuffer(ctx, derived.buf, (void **)&data)) { failures++; free(before); return; }
    for (unsigned i = 0; i < derived.data_size; i++) data[i] = (i * 13 + 7) % 251;
    memcpy(before, data, derived.data_size); ctx->vtable->vaUnmapBuffer(ctx, derived.buf);
    expect(ctx->vtable->vaGetImage(ctx, sid, 2, 0, 6, 4, derived.image_id) == VA_STATUS_ERROR_SURFACE_BUSY,
           "readback-keeps-derived-image-borrow-contract");
    expect(!memcmp(data, before, derived.data_size), "busy-readback-keeps-derived-storage");
    free(before); ctx->vtable->vaDestroyImage(ctx, derived.image_id); ctx->vtable->vaDestroySurfaces(ctx, &sid, 1);
    unsigned char rows[96], saved_rows[96];
    for (unsigned i = 0; i < sizeof(rows); i++) rows[i] = i;
    memcpy(saved_rows, rows, sizeof(rows));
    struct vareq_image_view alias = {.data = rows, .fourcc = VA_FOURCC_NV12, .width = 8, .height = 8, .stride = 8, .size = 96};
    expect(vareq_copy_image_rect(&alias, 0, 2, &alias, 0, 0, 8, 6) == VA_STATUS_SUCCESS &&
           !memcmp(rows + 16, saved_rows, 48) && !memcmp(rows + 72, saved_rows + 64, 24),
           "downward-alias-copy-does-not-overwrite-future-source-rows");
    unsigned char storage[48], original[48];
    for (unsigned i = 0; i < sizeof(storage); i++) storage[i] = i;
    memcpy(original, storage, sizeof(storage));
    struct vareq_image_view nv12 = {.data = storage, .fourcc = VA_FOURCC_NV12, .width = 8, .height = 4, .stride = 8, .size = 48};
    struct vareq_image_view planar = nv12; planar.fourcc = VA_FOURCC_I420;
    expect(vareq_copy_image_rect(&planar, 0, 0, &nv12, 0, 0, 8, 4) == VA_STATUS_SUCCESS,
           "same-storage-nv12-i420-conversion");
    for (unsigned i = 0; i < 8; i++) expect(storage[32 + i] == original[32 + 2 * i] &&
        storage[40 + i] == original[33 + 2 * i], "overlapping-chroma-repack-uses-unmodified-source");
}
/* Logical component address from public layout definitions, independent of
 * the production repacker. Exercise every ordered pair, including aliases. */
static uint8_t *component(struct vareq_image_view *view, unsigned plane, unsigned x, unsigned y)
{
    uint8_t *base = view->data;
    unsigned rows = view->alloc_height;
    if (!plane) return base + (size_t)y * view->stride + x;
    base += (size_t)view->stride * rows;
    if (view->fourcc == VA_FOURCC_NV12 || view->fourcc == VA_FOURCC_NV21 ||
        view->fourcc == V4L2_PIX_FMT_NV16 || view->fourcc == V4L2_PIX_FMT_NV61) {
        unsigned v_first = view->fourcc == VA_FOURCC_NV21 || view->fourcc == V4L2_PIX_FMT_NV61;
        return base + (size_t)y * view->stride + x * 2 + ((plane == 2) ^ v_first);
    }
    unsigned pitch = (view->stride + 1) / 2;
    unsigned second = (plane == 2) ^ (view->fourcc == VA_FOURCC_YV12 || view->fourcc == VA_FOURCC_YV16);
    unsigned chroma_rows = view->fourcc == VA_FOURCC_422H || view->fourcc == VA_FOURCC_YV16 ? rows : (rows + 1) / 2;
    return base + (size_t)second * pitch * chroma_rows + (size_t)y * pitch + x;
}
static void layout_oracle(void)
{
    const unsigned formats[] = {VA_FOURCC_NV12, VA_FOURCC_NV21, VA_FOURCC_I420, VA_FOURCC_YV12};
    for (unsigned a = 0; a < 4; a++) for (unsigned b = 0; b < 4; b++) {
        uint8_t original[144], output[72], expected[144], alias[144];
        memset(original, 0x93, sizeof(original)); memset(output, 0xa5, sizeof(output));
        memset(expected, 0xa5, sizeof(expected));
        struct vareq_image_view src = {.data = original, .fourcc = formats[a], .width = 7, .height = 5,
            .stride = 12, .alloc_height = 8, .size = sizeof(original)};
        struct vareq_image_view dst = {.data = output, .fourcc = formats[b], .width = 5, .height = 3,
            .stride = 8, .alloc_height = 6, .size = sizeof(output)}, wanted = dst;
        wanted.data = expected;
        for (unsigned p = 0; p < 3; p++)
            for (unsigned y = 0; y < (p ? 3u : 5u); y++)
                for (unsigned x = 0; x < (p ? 4u : 7u); x++)
                    *component(&src, p, x, y) = 31 + p * 67 + x * 3 + y * 7;
        for (unsigned p = 0; p < 3; p++)
            for (unsigned y = 0; y < (p ? 2u : 3u); y++)
                for (unsigned x = 0; x < (p ? 3u : 5u); x++)
                    *component(&wanted, p, x, y) = *component(&src, p, x + (p ? 1 : 2), y + (p ? 1 : 2));
        expect(vareq_copy_image_rect(&dst, 0, 0, &src, 2, 2, 5, 3) == VA_STATUS_SUCCESS &&
               !memcmp(output, expected, sizeof(output)), "all-420-layout-pairs-independent-crop-oracle-and-padding");
        memcpy(alias, original, sizeof(alias)); memcpy(expected, original, sizeof(original));
        dst = src; dst.data = alias; dst.fourcc = formats[b]; wanted = dst; wanted.data = expected;
        for (unsigned p = 0; p < 3; p++)
            for (unsigned y = 0; y < (p ? 3u : 5u); y++)
                for (unsigned x = 0; x < (p ? 4u : 7u); x++)
                    *component(&wanted, p, x, y) = *component(&src, p, x, y);
        src.data = alias;
        expect(vareq_copy_image_rect(&dst, 0, 0, &src, 0, 0, 7, 5) == VA_STATUS_SUCCESS &&
               !memcmp(alias, expected, sizeof(alias)), "all-420-layout-pairs-overlapping-storage-oracle");
    }
}
static void planar422_oracle(void)
{
    const unsigned formats[] = {VA_FOURCC_422H, VA_FOURCC_YV16, V4L2_PIX_FMT_NV16, V4L2_PIX_FMT_NV61};
    for (unsigned a = 0; a < 4; a++) for (unsigned b = 0; b < 4; b++) {
        unsigned char original[192], output[96], expected[192], alias[192];
        memset(original, 0x93, sizeof(original)); memset(output, 0xa5, sizeof(output)); memset(expected, 0xa5, sizeof(expected));
        struct vareq_image_view src = {.data = original, .fourcc = formats[a], .width = 7, .height = 5,
            .stride = 12, .alloc_height = 8, .size = sizeof(original)};
        struct vareq_image_view dst = {.data = output, .fourcc = formats[b], .width = 5, .height = 3,
            .stride = 8, .alloc_height = 6, .size = sizeof(output)}, wanted = dst;
        wanted.data = expected;
        for (unsigned p = 0; p < 3; p++) for (unsigned y = 0; y < 5; y++)
            for (unsigned x = 0; x < (p ? 4u : 7u); x++) *component(&src, p, x, y) = 31 + p * 67 + x * 3 + y * 7;
        for (unsigned p = 0; p < 3; p++) for (unsigned y = 0; y < 3; y++)
            for (unsigned x = 0; x < (p ? 3u : 5u); x++)
                *component(&wanted, p, x, y) = *component(&src, p, x + (p ? 1 : 2), y + 1);
        expect(vareq_copy_image_rect(&dst, 0, 0, &src, 2, 1, 5, 3) == VA_STATUS_SUCCESS &&
               !memcmp(output, expected, sizeof(output)), "422-full-height-chroma-odd-row-crop-and-padding-oracle");
        expect(vareq_copy_image_rect(&dst, 0, 0, &src, 1, 1, 4, 3) == VA_STATUS_ERROR_INVALID_PARAMETER &&
               !memcmp(output, expected, sizeof(output)), "422-invalid-chroma-origin-keeps-output-unchanged");
        dst.size--;
        expect(vareq_copy_image_rect(&dst, 0, 0, &src, 2, 1, 5, 3) == VA_STATUS_ERROR_INVALID_PARAMETER &&
               !memcmp(output, expected, sizeof(output)), "422-short-backing-keeps-output-unchanged");
        memcpy(alias, original, sizeof(alias)); memcpy(expected, original, sizeof(expected));
        dst = src; dst.data = alias; dst.fourcc = formats[b]; wanted = dst; wanted.data = expected;
        for (unsigned p = 0; p < 3; p++) for (unsigned y = 0; y < 5; y++)
            for (unsigned x = 0; x < (p ? 4u : 7u); x++) *component(&wanted, p, x, y) = *component(&src, p, x, y);
        src.data = alias;
        expect(vareq_copy_image_rect(&dst, 0, 0, &src, 0, 0, 7, 5) == VA_STATUS_SUCCESS &&
               !memcmp(alias, expected, sizeof(alias)), "422-all-layout-permutations-preserve-overlapping-source");
    }
    expect(vareq_va_image_size(VA_FOURCC_422H, 7, 5) == 75 &&
           vareq_va_image_size(VA_FOURCC_YV16, UINT_MAX, 1) == 0,
           "planar422-odd-stride-and-overflow-geometry");
}
static void packed422_rectangles(unsigned fourcc)
{
    uint8_t packed[128], output[72], expected[72];
    memset(packed, 0x93, sizeof(packed)); memset(output, 0xa5, sizeof(output)); memset(expected, 0xa5, sizeof(expected));
    for (unsigned y = 0; y < 5; y++) for (unsigned x = 0; x < 3; x++) {
        uint8_t *p = packed + y * 16 + x * 4;
        unsigned char y0 = 71 + y * 7 + x * 2, u = 43 + x, v = 181 + y;
        if (fourcc == VA_FOURCC_VYUY) { p[0] = v; p[1] = y0; p[2] = u; p[3] = y0 + 1; }
        else if (fourcc == VA_FOURCC_YVYU) { p[0] = y0; p[1] = v; p[2] = y0 + 1; p[3] = u; }
        else { p[0] = u; p[1] = y0; p[2] = v; p[3] = y0 + 1; }
    }
    struct vareq_image_view src = {.data = packed, .fourcc = fourcc, .width = 6, .height = 5,
        .stride = 16, .alloc_height = 8, .size = sizeof(packed)};
    struct vareq_image_view dst = {.data = output, .fourcc = fourcc, .width = 4, .height = 4,
        .stride = 12, .alloc_height = 6, .size = sizeof(output)};
    for (unsigned y = 0; y < 3; y++) {
        unsigned char *p = expected + y * 12;
        if (fourcc == VA_FOURCC_VYUY) { p[0] = 182 + y; p[1] = 80 + y * 7; p[2] = 44; p[3] = 81 + y * 7; }
        else if (fourcc == VA_FOURCC_YVYU) { p[0] = 80 + y * 7; p[1] = 182 + y; p[2] = 81 + y * 7; p[3] = 44; }
        else { p[0] = 44; p[1] = 80 + y * 7; p[2] = 182 + y; p[3] = 81 + y * 7; }
    }
    expect(vareq_copy_image_rect(&dst, 0, 0, &src, 2, 1, 2, 3) == VA_STATUS_SUCCESS &&
           !memcmp(output, expected, sizeof(output)), "packed422-offset-odd-row-crop-byte-order-and-padding");
    expect(vareq_copy_image_rect(&dst, 0, 0, &src, 1, 1, 2, 3) == VA_STATUS_ERROR_INVALID_PARAMETER &&
           !memcmp(output, expected, sizeof(output)), "packed422-odd-x-is-rejected-before-writing");
    expect(vareq_copy_image_rect(&dst, 0, 0, &src, 2, 1, 1, 3) == VA_STATUS_ERROR_INVALID_PARAMETER &&
           !memcmp(output, expected, sizeof(output)), "packed422-interior-odd-width-is-rejected-before-writing");
    dst.size = 47;
    expect(vareq_copy_image_rect(&dst, 0, 0, &src, 2, 1, 2, 3) == VA_STATUS_ERROR_INVALID_PARAMETER &&
           !memcmp(output, expected, sizeof(output)), "packed422-short-allocation-is-not-a-valid-destination");
    expect(vareq_default_image_stride(fourcc, 33) == 128, "packed422-odd-width-reserves-the-complete-pixel-pair");
}
int main(void)
{
    struct VADriverVTable v = {0};
    struct VADriverContext ctx = {.vtable = &v};
    setenv("VAREQ_PROBE_NOCACHE", "1", 1); setenv("VAREQ_EXPBUF_EXPORT", "1", 1);
    if (__vaDriverInit_1_20(&ctx)) return 1;
    static const struct { unsigned rt, fcc; } formats[] = {
        {VA_RT_FORMAT_YUV420, VA_FOURCC_NV12}, {VA_RT_FORMAT_YUV420, VA_FOURCC_I420},
        {VA_RT_FORMAT_YUV420, VA_FOURCC_NV21}, {VA_RT_FORMAT_YUV420, VA_FOURCC_YV12},
        {VA_RT_FORMAT_YUV420_10, VA_FOURCC_P010}, {VA_RT_FORMAT_YUV422, VA_FOURCC_YUY2}, {VA_RT_FORMAT_YUV422, VA_FOURCC_UYVY},
        {VA_RT_FORMAT_YUV422, VA_FOURCC_VYUY}, {VA_RT_FORMAT_YUV422, VA_FOURCC_YVYU},
        {VA_RT_FORMAT_YUV422, VA_FOURCC_422H}, {VA_RT_FORMAT_YUV422, VA_FOURCC_YV16},
        {VA_RT_FORMAT_YUV422_10, VA_FOURCC_Y210}, {VA_RT_FORMAT_RGB32, VA_FOURCC_BGRX},
        {VA_RT_FORMAT_RGB32, VA_FOURCC_BGRA}, {VA_RT_FORMAT_RGB32, VA_FOURCC_ARGB},
        {VA_RT_FORMAT_RGB16, VA_FOURCC_RGB565}, {VA_RT_FORMAT_RGB32, VA_FOURCC_RGBA}, {VA_RT_FORMAT_RGB32, VA_FOURCC_RGBX},
    };
    for (unsigned i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) roundtrip(&ctx, formats[i].rt, formats[i].fcc);
    const unsigned yuv[] = {VA_FOURCC_NV12, VA_FOURCC_NV21, VA_FOURCC_I420, VA_FOURCC_YV12};
    for (unsigned a = 0; a < 4; a++) for (unsigned b = 0; b < 4; b++)
        conversion(&ctx, yuv[a], yuv[b]);
    const unsigned planar422[] = {VA_FOURCC_422H, VA_FOURCC_YV16};
    for (unsigned a = 0; a < 2; a++) for (unsigned b = 0; b < 2; b++) conversion(&ctx, planar422[a], planar422[b]);
    decoded_partial(&ctx);
    overlapping_readback(&ctx);
    layout_oracle();
    planar422_oracle();
    packed422_rectangles(VA_FOURCC_UYVY);
    packed422_rectangles(VA_FOURCC_VYUY);
    packed422_rectangles(VA_FOURCC_YVYU);
    expect(vareq_va_image_size(VA_FOURCC_I420, UINT_MAX, 1) == 0, "i420-chroma-stride-overflow");
    v.vaTerminate(&ctx);
    return failures != 0;
}
