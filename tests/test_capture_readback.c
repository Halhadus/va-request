/* Public callbacks: independent packed samples, offset crops and OOM. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "va_v4l2.h"
extern VAStatus __vaDriverInit_1_20(VADriverContextP ctx);
void *__real_realloc(void *p, size_t size);
static int failures, fail_scratch;
void *__wrap_realloc(void *p, size_t size)
{
    return fail_scratch ? NULL : __real_realloc(p, size);
}
static void expect(int ok, const char *tag)
{
    if (!ok) { fprintf(stderr, "FAIL %s\n", tag); failures++; }
}
static unsigned sample(unsigned plane, unsigned x, unsigned y)
{
    return (17 + plane * 193 + x * 47 + y * 101) & 1023;
}
static void pack(uint8_t *row, unsigned x, unsigned value)
{
    /* Bit-by-bit fixture, independent of the driver's 40-bit unpacker. */
    for (unsigned bit = 0; bit < 10; bit++)
        row[(x * 10 + bit) / 8] |= ((value >> bit) & 1) << ((x * 10 + bit) % 8);
}
static void oracle(const VAImage *img, uint8_t *out, unsigned sx, unsigned sy,
                   unsigned width, unsigned height)
{
    memset(out, 0xa5, img->data_size);
    for (unsigned y = 0; y < height; y++) {
        uint8_t *row = out + (size_t)y * img->pitches[0];
        if (img->format.fourcc == VA_FOURCC_P010) {
            for (unsigned x = 0; x < width; x++)
                ((uint16_t *)row)[x] = sample(0, sx + x, sy + y) << 6;
        } else if (img->format.fourcc == VA_FOURCC_422H || img->format.fourcc == VA_FOURCC_YV16) {
            for (unsigned x = 0; x < width; x++) row[x] = sample(0, sx + x, sy + y) >> 2;
            unsigned u = img->format.fourcc == VA_FOURCC_YV16 ? 2 : 1, v = 3 - u;
            for (unsigned x = 0; x < (width + 1) / 2; x++) {
                out[img->offsets[u] + (size_t)y * img->pitches[u] + x] = sample(1, sx + x * 2, sy + y) >> 2;
                out[img->offsets[v] + (size_t)y * img->pitches[v] + x] = sample(1, sx + x * 2 + 1, sy + y) >> 2;
            }
        } else {
            for (unsigned x = 0; x < width; x += 2) {
                unsigned last = x + 1 < width ? x + 1 : x;
                unsigned values[] = {sample(0, sx + x, sy + y), sample(1, sx + x, sy + y),
                    sample(0, sx + last, sy + y), sample(1, sx + x + 1, sy + y)};
                for (unsigned i = 0; i < 4; i++) {
                    if (img->format.fourcc == VA_FOURCC_Y210)
                        ((uint16_t *)row)[x * 2 + i] = values[i] << 6;
                    else row[x * 2 + i] = values[i] >> 2;
                }
            }
        }
    }
    if (img->format.fourcc == VA_FOURCC_P010)
        for (unsigned y = 0; y < (height + 1) / 2; y++) {
            uint16_t *row = (uint16_t *)(out + img->offsets[1] + (size_t)y * img->pitches[1]);
            for (unsigned x = 0; x < ((width + 1) & ~1u); x++)
                row[x] = sample(1, sx + x, sy / 2 + y) << 6;
        }
}
static void readback(VADriverContextP ctx, unsigned cap, unsigned rt, unsigned fcc)
{
    VASurfaceID sid = VA_INVALID_ID;
    VAImage image, crop;
    VAImageFormat fmt = {.fourcc = fcc};
    if (ctx->vtable->vaCreateSurfaces(ctx, 7, 5, rt, 1, &sid) ||
        ctx->vtable->vaCreateImage(ctx, &fmt, 7, 5, &image) ||
        ctx->vtable->vaCreateImage(ctx, &fmt, 5, 3, &crop)) {
        expect(0, "capture-readback-setup"); return;
    }
    uint8_t capture[240] = {0}, *pixels, *cropped;
    unsigned stride = cap == V4L2_PIX_FMT_NV16 ? 12 : 15, aligned_h = 8;
    for (unsigned p = 0; p < 2; p++)
        for (unsigned y = 0; y < (p && cap == V4L2_PIX_FMT_NV15 ? 3 : 5); y++)
            for (unsigned x = 0; x < (p ? 8 : 7); x++) {
                uint8_t *row = capture + (p * aligned_h + y) * stride;
                if (cap == V4L2_PIX_FMT_NV16) row[x] = sample(p, x, y) >> 2;
                else pack(row, x, sample(p, x, y));
            }
    struct vav4l2_surface *s = ((struct vav4l2_driver_data *)ctx->pDriverData)->surfaces[sid];
    s->has_pic = 1; s->buf_index = 0; s->cap_view = capture; s->cap_fourcc = cap;
    s->stride = stride; s->aligned_h = aligned_h; s->last_writer = VAV4L2_WRITER_MEMFD;
    unsigned capture_size = vav4l2_capture_plane_size(cap, stride, aligned_h);
    if (vav4l2_surface_grow_memfd(s, capture_size) < 0) { expect(0, "capture-memfd-allocation"); return; }
    void *backing = vav4l2_surface_map_memfd(s, capture_size);
    if (!backing) { expect(0, "capture-memfd-map"); return; }
    memcpy(backing, capture, capture_size);
    ctx->vtable->vaMapBuffer(ctx, image.buf, (void **)&pixels);
    ctx->vtable->vaMapBuffer(ctx, crop.buf, (void **)&cropped);
    uint8_t *expected = malloc(image.data_size);
    if (!expected) { expect(0, "oracle-allocation"); goto done; }
    for (unsigned fallback = 0; fallback < 2; fallback++) {
        setenv("VAV4L2_EXPBUF_EXPORT", fallback ? "0" : "1", 1);
        memset(pixels, 0xa5, image.data_size);
        oracle(&image, expected, 0, 0, 7, 5);
        expect(ctx->vtable->vaGetImage(ctx, sid, 0, 0, 7, 5, image.image_id) == VA_STATUS_SUCCESS &&
               !memcmp(pixels, expected, image.data_size), "odd-edge-whole-image-independent-sample-oracle");
        memset(cropped, 0xa5, crop.data_size);
        oracle(&crop, expected, 2, 2, 5, 3);
        expect(ctx->vtable->vaGetImage(ctx, sid, 2, 2, 5, 3, crop.image_id) == VA_STATUS_SUCCESS &&
               !memcmp(cropped, expected, crop.data_size), "offset-crop-odd-right-and-bottom-edges");
        memset(cropped, 0xa5, crop.data_size);
        oracle(&crop, expected, 2, 2, 4, 2);
        expect(ctx->vtable->vaGetImage(ctx, sid, 2, 2, 4, 2, crop.image_id) == VA_STATUS_SUCCESS &&
               !memcmp(cropped, expected, crop.data_size), "crop-preserves-padding-and-unused-destination-rows");
        memset(cropped, 0xa5, crop.data_size); memset(expected, 0xa5, crop.data_size);
        expect(ctx->vtable->vaGetImage(ctx, sid, 1, 2, 4, 2, crop.image_id) == VA_STATUS_ERROR_INVALID_PARAMETER &&
               !memcmp(cropped, expected, crop.data_size), "unaligned-origin-does-not-write-destination");
        expect(ctx->vtable->vaGetImage(ctx, sid, 0, 0, 3, 2, crop.image_id) == VA_STATUS_ERROR_INVALID_PARAMETER &&
               !memcmp(cropped, expected, crop.data_size), "unaligned-interior-width-does-not-write-destination");
        if (cap == V4L2_PIX_FMT_NV15)
            expect(ctx->vtable->vaGetImage(ctx, sid, 2, 1, 4, 2, crop.image_id) == VA_STATUS_ERROR_INVALID_PARAMETER &&
                   !memcmp(cropped, expected, crop.data_size), "420-crop-rejects-odd-row-origin");
        if (cap == V4L2_PIX_FMT_NV20) {
            vav4l2_format_cleanup(); fail_scratch = 1;
            expect(ctx->vtable->vaGetImage(ctx, sid, 0, 0, 4, 2, crop.image_id) == VA_STATUS_ERROR_ALLOCATION_FAILED &&
                   !memcmp(cropped, expected, crop.data_size), "scratch-oom-is-an-error-without-partial-output");
            expect(ctx->vtable->vaGetImage(ctx, sid, 2, 2, 4, 2, crop.image_id) == VA_STATUS_ERROR_ALLOCATION_FAILED &&
                   !memcmp(cropped, expected, crop.data_size), "crop-propagates-converter-oom");
            fail_scratch = 0;
        }
    }
    free(expected);
done:
    s->buf_index = -1; s->cap_view = NULL;
    ctx->vtable->vaDestroyImage(ctx, image.image_id); ctx->vtable->vaDestroyImage(ctx, crop.image_id);
    ctx->vtable->vaDestroySurfaces(ctx, &sid, 1);
}
int main(void)
{
    struct VADriverVTable v = {0};
    struct VADriverContext ctx = {.vtable = &v};
    setenv("VAV4L2_PROBE_NOCACHE", "1", 1);
    if (__vaDriverInit_1_20(&ctx)) return 1;
    readback(&ctx, V4L2_PIX_FMT_NV15, VA_RT_FORMAT_YUV420_10, VA_FOURCC_P010);
    readback(&ctx, V4L2_PIX_FMT_NV16, VA_RT_FORMAT_YUV422, VA_FOURCC_YUY2);
    readback(&ctx, V4L2_PIX_FMT_NV16, VA_RT_FORMAT_YUV422, VA_FOURCC_422H);
    readback(&ctx, V4L2_PIX_FMT_NV16, VA_RT_FORMAT_YUV422, VA_FOURCC_YV16);
    readback(&ctx, V4L2_PIX_FMT_NV20, VA_RT_FORMAT_YUV422_10, VA_FOURCC_Y210);
    readback(&ctx, V4L2_PIX_FMT_NV20, VA_RT_FORMAT_YUV422_10, VA_FOURCC_YUY2);
    v.vaTerminate(&ctx);
    return failures != 0;
}
