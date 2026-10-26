/*
 * Pixel-format helpers: V4L2 capture fourcc ↔ VA image fourcc, size, convert.
 *
 * rkvdec exposes NV12 / NV15 / NV16 / NV20. VA-API clients want NV12 / P010 /
 * YUY2. NV15 is packed 10-bit 4:2:0 (4 samples / 5 bytes, little-endian);
 * P010 stores 10 bits in the high end of a uint16 (sample << 6).
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <va/va.h>
#include <linux/videodev2.h>
#include <drm_fourcc.h>

#include "va_v4l2.h"

struct capture_format { uint32_t v4l2, va, drm, rt; unsigned depth, chroma; };
static const struct capture_format capture_formats[] = {
    {V4L2_PIX_FMT_NV12, VA_FOURCC_NV12, DRM_FORMAT_NV12, VA_RT_FORMAT_YUV420, 8, 1},
    {V4L2_PIX_FMT_NV12_4L4, VA_FOURCC_NV12, DRM_FORMAT_NV12, VA_RT_FORMAT_YUV420, 8, 1},
    {V4L2_PIX_FMT_NV12_16L16, VA_FOURCC_NV12, DRM_FORMAT_NV12, VA_RT_FORMAT_YUV420, 8, 1},
    {V4L2_PIX_FMT_NV12_32L32, VA_FOURCC_NV12, DRM_FORMAT_NV12, VA_RT_FORMAT_YUV420, 8, 1},
    {V4L2_PIX_FMT_NV15, VA_FOURCC_P010, DRM_FORMAT_NV15, VA_RT_FORMAT_YUV420_10, 10, 1},
    {V4L2_PIX_FMT_P010, VA_FOURCC_P010, DRM_FORMAT_P010, VA_RT_FORMAT_YUV420_10, 10, 1},
    {V4L2_PIX_FMT_NV16, VA_FOURCC_YUY2, DRM_FORMAT_NV16, VA_RT_FORMAT_YUV422, 8, 2},
    {V4L2_PIX_FMT_NV20, VA_FOURCC_YUY2, DRM_FORMAT_NV20, VA_RT_FORMAT_YUV422_10, 10, 2},
    {V4L2_PIX_FMT_NV12_8L128, VA_FOURCC_NV12, DRM_FORMAT_NV12, VA_RT_FORMAT_YUV420, 8, 1},
    {V4L2_PIX_FMT_P010_4L4, VA_FOURCC_P010, DRM_FORMAT_P010, VA_RT_FORMAT_YUV420_10, 10, 1},
};
uint32_t vav4l2_capture_fourcc_from_rt(unsigned int rt)
{
    for (unsigned i = 0; i < sizeof(capture_formats) / sizeof(capture_formats[0]); i++) if (capture_formats[i].rt == rt) return capture_formats[i].v4l2;
    return 0;
}
uint32_t vav4l2_capture_fourcc_from_sps(int minus8, int chroma)
{
    for (unsigned i = 0; i < sizeof(capture_formats) / sizeof(capture_formats[0]); i++) if ((int)capture_formats[i].depth == minus8 + 8 && (int)capture_formats[i].chroma == chroma) return capture_formats[i].v4l2;
    return 0;
}
uint32_t vav4l2_va_fourcc_for_capture(uint32_t fcc)
{
    for (unsigned i = 0; i < sizeof(capture_formats) / sizeof(capture_formats[0]); i++) if (capture_formats[i].v4l2 == fcc) return capture_formats[i].va;
    return 0;
}
uint32_t vav4l2_drm_fourcc_for_capture(uint32_t fcc)
{
    for (unsigned i = 0; i < sizeof(capture_formats) / sizeof(capture_formats[0]); i++) if (capture_formats[i].v4l2 == fcc) return capture_formats[i].drm;
    return 0;
}

static unsigned tile_geometry(uint32_t f, unsigned *height)
{
    unsigned width = f == V4L2_PIX_FMT_NV12_4L4 ? 4 : f == V4L2_PIX_FMT_NV12_16L16 ? 16 :
        f == V4L2_PIX_FMT_NV12_32L32 ? 32 : f == V4L2_PIX_FMT_NV12_8L128 || f == V4L2_PIX_FMT_P010_4L4 ? 8 : 0;
    *height = f == V4L2_PIX_FMT_NV12_8L128 ? 128 : f == V4L2_PIX_FMT_P010_4L4 ? 4 : width;
    return width;
}
unsigned vav4l2_capture_tile(uint32_t f)
{
    unsigned height; return tile_geometry(f, &height);
}
int vav4l2_detile_capture(uint8_t *dst, unsigned pitch, unsigned size, const uint8_t *src, unsigned span,
    unsigned stride, unsigned rows, unsigned width, unsigned height, uint32_t fourcc)
{
    unsigned th, tw = tile_geometry(fourcc, &th), required = vav4l2_capture_plane_size(fourcc, stride, rows);
    unsigned bytes = fourcc == V4L2_PIX_FMT_P010_4L4 ? 2 : 1;
    uint64_t target = (uint64_t)pitch * (height + ((uint64_t)height + 1) / 2);
    if (!tw || !dst || !src || !width || !height || (uint64_t)width * bytes > stride || height > rows ||
        pitch < ((uint64_t)width + 1) / 2 * 2 * bytes || !required || span < required || target > size) return -1;
    for (unsigned plane = 0; plane < 2; plane++) {
        unsigned h = plane ? (height + 1) / 2 : height, w = (plane ? (width + 1) & ~1u : width) * bytes;
        size_t start = plane ? (size_t)stride * rows : 0, output = plane ? (size_t)pitch * height : 0;
        for (unsigned y = 0; y < h; y++) for (unsigned x = 0; x < w;) {
            size_t offset = (size_t)(y / th) * stride * th + (size_t)(x / tw) * tw * th + (y % th) * tw + x % tw;
            unsigned n = tw - x % tw; if (n > w - x) n = w - x;
            memcpy(dst + output + (size_t)y * pitch + x, src + start + offset, n); x += n;
        }
    }
    return 0;
}
uint32_t vav4l2_capture_plane_size(uint32_t fourcc, uint32_t stride, uint32_t aligned_h)
{
    if ((!vav4l2_drm_fourcc_for_capture(fourcc) && fourcc != V4L2_PIX_FMT_NV61) || !stride || !aligned_h) return 0;
    unsigned th, tile = tile_geometry(fourcc, &th);
    if (tile && ((stride % tile) || (aligned_h % th))) return 0;
    uint64_t rows = aligned_h;
    if (tile) rows += ((((uint64_t)aligned_h + 1) / 2 + th - 1) / th) * th;
    else rows += (fourcc == V4L2_PIX_FMT_NV16 || fourcc == V4L2_PIX_FMT_NV61 || fourcc == V4L2_PIX_FMT_NV20)
        ? (uint64_t)aligned_h : ((uint64_t)aligned_h + 1) / 2;
    uint64_t size = (uint64_t)stride * rows;
    return size <= UINT32_MAX ? (uint32_t)size : 0;
}

/*
 * Shared per-row 10-bit unpack scratch: heap, grows on demand, sized for
 * one Y row + one UV row. Every caller runs under vav4l2_api_lock
 * (get_image / put_image / vpp), so one buffer serves all converters and
 * wide frames never hit a silent width clamp. Returns NULL on OOM.
 */
static uint16_t *g_row_scratch;
static uint32_t g_row_scratch_samples;

void vav4l2_format_cleanup(void)
{
    free(g_row_scratch); g_row_scratch = NULL; g_row_scratch_samples = 0;
}

static uint16_t *row_scratch_get(int samples)
{
    if (samples <= 0)
        return NULL;
    if (samples > (int)g_row_scratch_samples) {
        uint16_t *p = realloc(g_row_scratch,
                              (size_t)samples * 2 * sizeof(uint16_t));

        if (!p)
            return NULL;
        g_row_scratch = p;
        g_row_scratch_samples = (uint32_t)samples;
    }
    return g_row_scratch;
}

static void unpack_le40_to_p010(const uint8_t *src, uint16_t *dst, int samples)
{
    int i = 0;

    while (i + 4 <= samples) {
        uint64_t v = (uint64_t)src[0]
                   | ((uint64_t)src[1] << 8)
                   | ((uint64_t)src[2] << 16)
                   | ((uint64_t)src[3] << 24)
                   | ((uint64_t)src[4] << 32);
        dst[0] = (uint16_t)((v & 0x3ffu) << 6);
        dst[1] = (uint16_t)(((v >> 10) & 0x3ffu) << 6);
        dst[2] = (uint16_t)(((v >> 20) & 0x3ffu) << 6);
        dst[3] = (uint16_t)(((v >> 30) & 0x3ffu) << 6);
        src += 5;
        dst += 4;
        i += 4;
    }
    if (i < samples) {
        uint64_t v = 0;
        int b, n = samples - i;
        for (b = 0; b < 5; b++)
            v |= (uint64_t)src[b] << (8 * b);
        for (b = 0; b < n; b++)
            dst[b] = (uint16_t)(((v >> (10 * b)) & 0x3ffu) << 6);
    }
}

void vav4l2_nv15_to_p010(uint8_t *dst, uint32_t dst_stride, uint32_t dst_height,
                         const uint8_t *src, uint32_t src_stride,
                         uint32_t src_aligned_h,
                         int width, int height)
{
    int y;
    int rows = height;
    const uint8_t *src_uv = src + (size_t)src_stride * src_aligned_h;
    uint8_t *dst_uv = dst + (size_t)dst_stride * dst_height;

    if (rows > (int)src_aligned_h)
        rows = (int)src_aligned_h;

    for (y = 0; y < rows; y++)
        unpack_le40_to_p010(src + (size_t)y * src_stride,
                            (uint16_t *)(dst + (size_t)y * dst_stride),
                            width);

    rows = (height + 1) / 2;
    if (rows > (int)((src_aligned_h + 1) / 2))
        rows = (int)((src_aligned_h + 1) / 2);
    for (y = 0; y < rows; y++)
        unpack_le40_to_p010(src_uv + (size_t)y * src_stride,
                            (uint16_t *)(dst_uv + (size_t)y * dst_stride),
                            (width + 1) & ~1);
}

void vav4l2_copy_nv12(uint8_t *dst, uint32_t dst_stride, uint32_t dst_height,
                      const uint8_t *src, uint32_t src_stride,
                      uint32_t src_aligned_h,
                      int width, int height)
{
    int y;
    int rows = height;
    const uint8_t *src_uv = src + (size_t)src_stride * src_aligned_h;
    uint8_t *dst_uv = dst + (size_t)dst_stride * dst_height;

    if (rows > (int)src_aligned_h)
        rows = (int)src_aligned_h;
    for (y = 0; y < rows; y++)
        memcpy(dst + (size_t)y * dst_stride,
               src + (size_t)y * src_stride, (size_t)width);
    rows = (height + 1) / 2;
    if (rows > (int)((src_aligned_h + 1) / 2))
        rows = (int)((src_aligned_h + 1) / 2);
    for (y = 0; y < rows; y++)
        memcpy(dst_uv + (size_t)y * dst_stride,
               src_uv + (size_t)y * src_stride, (size_t)((width + 1) & ~1));
}

void vav4l2_nv16_to_yuy2(uint8_t *dst, uint32_t dst_stride,
                         const uint8_t *src, uint32_t src_stride,
                         uint32_t src_aligned_h,
                         int width, int height)
{
    int y, x;
    int rows = height;
    const uint8_t *src_uv = src + (size_t)src_stride * src_aligned_h;

    if (rows > (int)src_aligned_h)
        rows = (int)src_aligned_h;

    for (y = 0; y < rows; y++) {
        const uint8_t *ys = src + (size_t)y * src_stride;
        const uint8_t *uv = src_uv + (size_t)y * src_stride;
        uint8_t *d = dst + (size_t)y * dst_stride;
        for (x = 0; x + 1 < width; x += 2) {
            d[0] = ys[x];
            d[1] = uv[x];
            d[2] = ys[x + 1];
            d[3] = uv[x + 1];
            d += 4;
        }
        if (x < width) {
            d[0] = ys[x];
            d[1] = uv[x];
            d[2] = ys[x];
            d[3] = uv[x + 1];
        }
    }
}

int vav4l2_nv20_to_yuy2(uint8_t *dst, uint32_t dst_stride,
                         const uint8_t *src, uint32_t src_stride,
                         uint32_t src_aligned_h,
                         int width, int height)
{
    /* Explicit 8-bit readback; Y210 is available for lossless 10-bit. */
    int y, x;
    int rows = height;
    const uint8_t *src_uv = src + (size_t)src_stride * src_aligned_h;
    uint16_t *y10, *uv10;

    if (rows > (int)src_aligned_h)
        rows = (int)src_aligned_h;
    int samples = (width + 1) & ~1;
    y10 = row_scratch_get(samples);
    if (!y10) {
        fprintf(stderr, "v4l2: 10-bit row scratch OOM at width %d\n",
                width);
        return -1;
    }
    uv10 = y10 + samples;

    for (y = 0; y < rows; y++) {
        uint8_t *d = dst + (size_t)y * dst_stride;
        unpack_le40_to_p010(src + (size_t)y * src_stride, y10, width);
        unpack_le40_to_p010(src_uv + (size_t)y * src_stride, uv10, samples);
        for (x = 0; x + 1 < width; x += 2) {
            d[0] = (uint8_t)(y10[x] >> 8);
            d[1] = (uint8_t)(uv10[x] >> 8);
            d[2] = (uint8_t)(y10[x + 1] >> 8);
            d[3] = (uint8_t)(uv10[x + 1] >> 8);
            d += 4;
        }
        if (x < width) {
            d[0] = d[2] = (uint8_t)(y10[x] >> 8);
            d[1] = (uint8_t)(uv10[x] >> 8);
            d[3] = (uint8_t)(uv10[x + 1] >> 8);
        }
    }
    return 0;
}

int vav4l2_nv20_to_y210(uint8_t *dst, uint32_t dst_stride,
                         const uint8_t *src, uint32_t src_stride,
                         uint32_t src_aligned_h, int width, int height)
{
    /* NV20 (packed 10-bit, Y plane + full-height UV plane) -> Y210
     * (Y0 U Y1 V in little-endian 16-bit containers, value << 6). */
    int y, x;
    int rows = height;
    const uint8_t *src_uv = src + (size_t)src_stride * src_aligned_h;
    uint16_t *y10, *uv10;

    if (rows > (int)src_aligned_h)
        rows = (int)src_aligned_h;
    int samples = (width + 1) & ~1;
    y10 = row_scratch_get(samples);
    if (!y10) {
        fprintf(stderr, "v4l2: 10-bit row scratch OOM at width %d\n",
                width);
        return -1;
    }
    uv10 = y10 + samples;

    for (y = 0; y < rows; y++) {
        uint16_t *d = (uint16_t *)(dst + (size_t)y * dst_stride);
        unpack_le40_to_p010(src + (size_t)y * src_stride, y10, width);
        unpack_le40_to_p010(src_uv + (size_t)y * src_stride, uv10, samples);
        for (x = 0; x + 1 < width; x += 2) {
            d[0] = y10[x];
            d[1] = uv10[x];
            d[2] = y10[x + 1];
            d[3] = uv10[x + 1];
            d += 4;
        }
        if (x < width) {
            d[0] = y10[x];
            d[1] = uv10[x];
            d[2] = y10[x];
            d[3] = uv10[x + 1];
        }
    }
    return 0;
}

uint32_t vav4l2_va_image_size(uint32_t va_fourcc, uint32_t stride, uint32_t height)
{
    uint64_t size = (uint64_t)stride * height;
    switch (va_fourcc) {
    case VA_FOURCC_NV12:
    case VA_FOURCC_NV21:
    case VA_FOURCC_P010:
        size += (uint64_t)stride * (((uint64_t)height + 1) / 2);
        break;
    /* NV16/NV61 are private capture/queue views, not public VA formats. */
    case V4L2_PIX_FMT_NV16:
    case V4L2_PIX_FMT_NV61:
        size += (uint64_t)stride * height;
        break;
    case VA_FOURCC_422H:
    case VA_FOURCC_YV16:
        size += (((uint64_t)stride + 1) / 2) * height * 2;
        break;
    case VA_FOURCC_I420:
    case VA_FOURCC_YV12:
        size += (((uint64_t)stride + 1) / 2) * (((uint64_t)height + 1) / 2) * 2;
        break;
    default:
        break;
    }
    return size <= UINT32_MAX ? (uint32_t)size : 0;
}

static int annexb_missing_prefix(const uint8_t *d, uint32_t sz)
{
    return !(sz >= 3 && d[0] == 0 && d[1] == 0 &&
             (d[2] == 1 || (sz >= 4 && d[2] == 0 && d[3] == 1)));
}

/* Concatenate slice NALs into a pre-mapped Annex-B bitstream. A NAL that
 * already carries a start code is copied verbatim; one without gets a
 * `prefix_len`-byte start code (3 or 4, per codec convention). Returns
 * the total byte count (0 when it does not fit dst_cap; a NULL dst only
 * sizes the result, matching the anon-buffer test mode). */
size_t vav4l2_annexb_concat(const uint8_t * const *datas, const uint32_t *sizes,
                            int n, int prefix_len, uint8_t *dst, size_t dst_cap)
{
    size_t total = 0, off = 0;
    int i;

    if (n < 0 || (n && (!datas || !sizes)) || (prefix_len != 3 && prefix_len != 4)) return 0;
    for (i = 0; i < n; i++) {
        if (!datas[i] || !sizes[i] || sizes[i] > dst_cap - total) return 0;
        total += sizes[i];
        if (annexb_missing_prefix(datas[i], sizes[i])) {
            if ((size_t)prefix_len > dst_cap - total) return 0;
            total += prefix_len;
        }
    }
    if (!dst)
        return total;
    for (i = 0; i < n; i++) {
        if (annexb_missing_prefix(datas[i], sizes[i])) {
            if (prefix_len == 4)
                dst[off++] = 0;
            dst[off++] = 0;
            dst[off++] = 0;
            dst[off++] = 1;
        }
        memcpy(dst + off, datas[i], sizes[i]);
        off += sizes[i];
    }
    return total;
}

struct slice_writer {
    uint8_t *dst;
    size_t capacity, bytes;
    uint8_t header[4];
    unsigned header_size;
    int prefix_len, open, single_unit;
    unsigned units;
};

static VAStatus slice_emit(struct slice_writer *w, const uint8_t *data, size_t size)
{
    if (size > w->capacity - w->bytes) return VA_STATUS_ERROR_OPERATION_FAILED;
    if (w->dst) memcpy(w->dst + w->bytes, data, size);
    w->bytes += size;
    return VA_STATUS_SUCCESS;
}

static VAStatus slice_header(struct slice_writer *w)
{
    static const uint8_t prefix[] = {0, 0, 0, 1};
    VAStatus status;
    if (w->prefix_len && annexb_missing_prefix(w->header, w->header_size)) {
        status = slice_emit(w, prefix + 4 - w->prefix_len, w->prefix_len);
        if (status != VA_STATUS_SUCCESS) return status;
    }
    return slice_emit(w, w->header, w->header_size);
}

static VAStatus slice_fragment(struct slice_writer *w, const VASliceParameterBufferBase *p,
                               const uint8_t *data)
{
    int first = p->slice_data_flag == VA_SLICE_DATA_FLAG_ALL || p->slice_data_flag == VA_SLICE_DATA_FLAG_BEGIN;
    int last = p->slice_data_flag == VA_SLICE_DATA_FLAG_ALL || p->slice_data_flag == VA_SLICE_DATA_FLAG_END;
    if (p->slice_data_flag != VA_SLICE_DATA_FLAG_ALL && p->slice_data_flag != VA_SLICE_DATA_FLAG_BEGIN &&
        p->slice_data_flag != VA_SLICE_DATA_FLAG_MIDDLE && p->slice_data_flag != VA_SLICE_DATA_FLAG_END)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (first == w->open) return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (first) {
        if (w->single_unit && w->units) return VA_STATUS_ERROR_INVALID_PARAMETER;
        w->open = 1; w->header_size = 0; w->units++;
    }
    size_t size = p->slice_data_size;
    VAStatus status;
    if (w->header_size < sizeof(w->header)) {
        size_t take = sizeof(w->header) - w->header_size;
        if (take > size) take = size;
        memcpy(w->header + w->header_size, data, take);
        w->header_size += take; data += take; size -= take;
        if (w->header_size == sizeof(w->header) || last) {
            status = slice_header(w);
            if (status != VA_STATUS_SUCCESS) return status;
        }
    }
    status = slice_emit(w, data, size);
    if (last) w->open = 0;
    return status;
}

static VAStatus packetize_slices(enum vav4l2_codec codec, struct vav4l2_buffer **buffers,
                                 int n, struct slice_writer *w)
{
    int pending = -1;
    size_t param_size = codec == VAV4L2_CODEC_H264 ? sizeof(VASliceParameterBufferH264) :
        codec == VAV4L2_CODEC_HEVC ? sizeof(VASliceParameterBufferHEVC) :
        codec == VAV4L2_CODEC_MPEG2 ? sizeof(VASliceParameterBufferMPEG2) :
        codec == VAV4L2_CODEC_VP9 ? sizeof(VASliceParameterBufferVP9) : sizeof(VASliceParameterBufferVP8);
    for (int i = 0; i < n; i++) {
        struct vav4l2_buffer *b = buffers[i];
        if (!b) return VA_STATUS_ERROR_INVALID_PARAMETER;
        if (b->type != VASliceParameterBufferType && b->type != VASliceDataBufferType) continue;
        unsigned count = b->allocated_elements ? b->num_elements : (b->num_elements ? b->num_elements : 1);
        if (!b->data || !count || !b->size || (b->allocated_elements && count > b->allocated_elements) ||
            b->size > UINT32_MAX / count) return VA_STATUS_ERROR_INVALID_PARAMETER;
        if (b->type == VASliceParameterBufferType) {
            if (b->size < param_size) return VA_STATUS_ERROR_INVALID_PARAMETER;
            if (pending < 0) pending = i;
            continue;
        }
        if (pending < 0) return VA_STATUS_ERROR_INVALID_PARAMETER;
        uint32_t data_size = b->size * count;
        for (int j = pending; j < i; j++) {
            struct vav4l2_buffer *params = buffers[j];
            if (params->type != VASliceParameterBufferType) continue;
            unsigned elements = params->num_elements ? params->num_elements : 1;
            for (unsigned k = 0; k < elements; k++) {
                VASliceParameterBufferBase p;
                memcpy(&p, (const uint8_t *)params->data + (size_t)k * params->size, sizeof(p));
                if (!p.slice_data_size || p.slice_data_offset > data_size || p.slice_data_size > data_size - p.slice_data_offset)
                    return VA_STATUS_ERROR_INVALID_PARAMETER;
                VAStatus status = slice_fragment(w, &p, (const uint8_t *)b->data + p.slice_data_offset);
                if (status != VA_STATUS_SUCCESS) return status;
            }
        }
        pending = -1;
    }
    return pending >= 0 || w->open || !w->bytes ? VA_STATUS_ERROR_INVALID_PARAMETER : VA_STATUS_SUCCESS;
}

/* Parameter arrays preceding each data buffer describe byte ranges in that buffer.
 * Preflight the entire picture before writing: bad ranges, incomplete fragments or
 * insufficient capacity must not leave a partially rewritten OUTPUT bitstream. */
VAStatus vav4l2_packetize_slices(enum vav4l2_codec codec, struct vav4l2_buffer **buffers,
                                int n, uint8_t *dst, size_t dst_cap, size_t *bytes)
{
    if (!bytes) return VA_STATUS_ERROR_INVALID_PARAMETER;
    *bytes = 0;
    if ((codec != VAV4L2_CODEC_H264 && codec != VAV4L2_CODEC_HEVC && codec != VAV4L2_CODEC_VP8 && codec != VAV4L2_CODEC_VP9 && codec != VAV4L2_CODEC_MPEG2) || n <= 0 || !buffers)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    struct slice_writer w = {.capacity = dst_cap, .prefix_len = codec == VAV4L2_CODEC_H264 ? 3 : codec == VAV4L2_CODEC_HEVC ? 4 : 0,
        .single_unit = codec == VAV4L2_CODEC_VP8 || codec == VAV4L2_CODEC_VP9};
    VAStatus status = packetize_slices(codec, buffers, n, &w);
    if (status != VA_STATUS_SUCCESS) return status;
    if (dst) {
        w.dst = dst; w.bytes = 0; w.header_size = 0; w.units = 0;
        status = packetize_slices(codec, buffers, n, &w);
        if (status != VA_STATUS_SUCCESS) return status;
    }
    *bytes = w.bytes;
    return VA_STATUS_SUCCESS;
}

uint32_t vav4l2_default_image_stride(uint32_t va_fourcc, int width)
{
    uint64_t stride;

    if (width <= 0)
        return 0;
    switch (va_fourcc) {
    case VA_FOURCC_BGR565:
    case VA_FOURCC_RGB565:
        stride = (uint64_t)width * 2; break;
    case VA_FOURCC_P010:
    case VA_FOURCC_YUY2:
    case VA_FOURCC_UYVY:
    case VA_FOURCC_VYUY:
    case VA_FOURCC_YVYU:
        stride = ((uint64_t)width + (va_fourcc != VA_FOURCC_P010 ? (width & 1) : 0)) * 2;
        break;
    case VA_FOURCC_Y210:
    case VA_FOURCC_RGBA:
    case VA_FOURCC_RGBX:
    case VA_FOURCC_XRGB:
    case VA_FOURCC_ABGR:
    case VA_FOURCC_XBGR:
    case VA_FOURCC_ARGB:
    case VA_FOURCC_BGRA:
    case VA_FOURCC_BGRX:
        stride = ((uint64_t)width + (va_fourcc == VA_FOURCC_Y210 ? (width & 1) : 0)) * 4;
        break;
    default:
        stride = (uint32_t)width;
        break;
    }
    /* Keep every plane's rows 64-byte aligned, including half-width chroma.
     * Allocation sizes and client pitches derive from the same stride. */
    uint64_t alignment = va_fourcc == VA_FOURCC_I420 || va_fourcc == VA_FOURCC_YV12 ||
                         va_fourcc == VA_FOURCC_422H || va_fourcc == VA_FOURCC_YV16 ? 128 : 64;
    stride = (stride + alignment - 1) & ~(alignment - 1);
    return stride <= UINT32_MAX ? (uint32_t)stride : 0;
}

static int image_420_8bit(uint32_t fourcc)
{
    return fourcc == VA_FOURCC_NV12 || fourcc == VA_FOURCC_NV21 ||
           fourcc == VA_FOURCC_I420 || fourcc == VA_FOURCC_YV12;
}

static int image_422_8bit(uint32_t fourcc)
{
    return fourcc == VA_FOURCC_422H || fourcc == VA_FOURCC_YV16 ||
           fourcc == V4L2_PIX_FMT_NV16 || fourcc == V4L2_PIX_FMT_NV61;
}

/* Public images plus private NV16/NV61 capture/queue views. CreateImage
 * and surface allocation keep those kernel-only FOURCCs unadvertised. */
static int image_planes(const struct vav4l2_image_view *v, uint8_t *planes[3],
                        uint32_t pitches[3], unsigned *bpp, unsigned *n)
{
    uint32_t rows = v->alloc_height ? v->alloc_height : v->height;
    unsigned pair = 0;
    *bpp = 1; *n = 1;
    switch (v->fourcc) {
    case VA_FOURCC_NV12: case VA_FOURCC_NV21: case V4L2_PIX_FMT_NV16: case V4L2_PIX_FMT_NV61: *n = 2; break;
    case VA_FOURCC_I420: case VA_FOURCC_YV12: case VA_FOURCC_422H: case VA_FOURCC_YV16: *n = 3; break;
    case VA_FOURCC_P010: *bpp = 2; *n = 2; break;
    case VA_FOURCC_YUY2: case VA_FOURCC_UYVY: case VA_FOURCC_VYUY: case VA_FOURCC_YVYU: *bpp = 2; pair = 1; break;
    case VA_FOURCC_Y210: *bpp = 4; pair = 1; break;
    case VA_FOURCC_BGR565: case VA_FOURCC_RGB565: *bpp = 2; break;
    case VA_FOURCC_RGBA: case VA_FOURCC_RGBX:
    case VA_FOURCC_XRGB: case VA_FOURCC_ABGR: case VA_FOURCC_XBGR:
    case VA_FOURCC_BGRA: case VA_FOURCC_BGRX: case VA_FOURCC_ARGB: *bpp = 4; break;
    default: return 0;
    }
    uint64_t row = (uint64_t)v->width * *bpp;
    if (pair || *n == 2) row = (((uint64_t)v->width + 1) & ~UINT64_C(1)) * *bpp;
    uint32_t size = vav4l2_va_image_size(v->fourcc, v->stride, rows);
    if (!v->data || !v->width || !v->height || rows < v->height || !size ||
        row > v->stride || size > v->size)
        return 0;
    planes[0] = v->data; pitches[0] = v->stride;
    if (*n > 1) {
        planes[1] = planes[0] + (size_t)v->stride * rows;
        pitches[1] = *n == 3 ? (v->stride + 1) / 2 : v->stride;
    }
    if (*n == 3) {
        pitches[2] = pitches[1];
        planes[2] = planes[1] + (size_t)pitches[1] * (image_422_8bit(v->fourcc) ? rows : (rows + 1) / 2);
    }
    return 1;
}

VAStatus vav4l2_copy_image_rect(const struct vav4l2_image_view *dst, unsigned dx, unsigned dy,
                               const struct vav4l2_image_view *src, unsigned sx, unsigned sy,
                               unsigned width, unsigned height)
{
    uint8_t *sp[3], *dp[3];
    uint32_t ss[3], ds[3];
    unsigned sb, db, sn, dn;
    if (!src || !dst || !width || !height || sx > src->width || sy > src->height ||
        dx > dst->width || dy > dst->height || width > src->width - sx ||
        height > src->height - sy || width > dst->width - dx || height > dst->height - dy)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    int planar420 = image_420_8bit(src->fourcc) && image_420_8bit(dst->fourcc);
    int planar422 = image_422_8bit(src->fourcc) && image_422_8bit(dst->fourcc);
    if (src->fourcc != dst->fourcc && !planar420 && !planar422)
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    if (!image_planes(src, sp, ss, &sb, &sn) || !image_planes(dst, dp, ds, &db, &dn))
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    unsigned subsampled = sn > 1 || src->fourcc == VA_FOURCC_YUY2 || src->fourcc == VA_FOURCC_UYVY ||
        src->fourcc == VA_FOURCC_VYUY || src->fourcc == VA_FOURCC_YVYU || src->fourcc == VA_FOURCC_Y210;
    if (subsampled && (((sx | dx) & 1) || ((width & 1) &&
        (sx + width != src->width || dx + width != dst->width))))
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (sn > 1 && !planar422 && (((sy | dy) & 1) || ((height & 1) &&
        (sy + height != src->height || dy + height != dst->height))))
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    uint32_t source_size = vav4l2_va_image_size(src->fourcc, src->stride, src->alloc_height ? src->alloc_height : src->height);
    uint32_t target_size = vav4l2_va_image_size(dst->fourcc, dst->stride, dst->alloc_height ? dst->alloc_height : dst->height);
    uintptr_t source_address = (uintptr_t)src->data, target_address = (uintptr_t)dst->data;
    if ((target_address >= source_address && target_address - source_address < source_size) ||
        (source_address > target_address && source_address - target_address < target_size)) {
        /* Derived images can share surface storage. Snapshot before any
         * row or chroma-plane write, including planar/interleaved repacks. */
        void *copy = malloc(source_size);
        if (!copy) return VA_STATUS_ERROR_ALLOCATION_FAILED;
        memcpy(copy, src->data, source_size);
        struct vav4l2_image_view snapshot = *src;
        snapshot.data = copy; snapshot.size = source_size;
        VAStatus st = vav4l2_copy_image_rect(dst, dx, dy, &snapshot, sx, sy, width, height);
        free(copy);
        return st;
    }
    unsigned row = width * sb;
    if (sn == 1 && subsampled) row = ((width + 1) & ~1u) * sb;
    for (unsigned y = 0; y < height; y++)
        memcpy(dp[0] + (size_t)(dy + y) * ds[0] + dx * db,
               sp[0] + (size_t)(sy + y) * ss[0] + sx * sb, row);
    if (sn == 1) return VA_STATUS_SUCCESS;
    unsigned vertical = planar422 ? 1 : 2;
    unsigned cw = (width + 1) / 2, ch = (height + vertical - 1) / vertical;
    if (src->fourcc == dst->fourcc) {
        unsigned cb = sn == 3 ? 1 : sb * 2;
        for (unsigned p = 1; p < sn; p++)
            for (unsigned y = 0; y < ch; y++)
                memcpy(dp[p] + (size_t)(dy / vertical + y) * ds[p] + (dx / 2) * cb,
                       sp[p] + (size_t)(sy / vertical + y) * ss[p] + (sx / 2) * cb, cw * cb);
    } else {
        for (unsigned y = 0; y < ch; y++) {
            uint8_t *su = sp[1] + (size_t)(sy / vertical + y) * ss[1] + (sx / 2) * (sn == 2 ? 2 : 1);
            uint8_t *sv = sn == 2 ? su + 1 : sp[2] + (size_t)(sy / vertical + y) * ss[2] + sx / 2;
            uint8_t *du = dp[1] + (size_t)(dy / vertical + y) * ds[1] + (dx / 2) * (dn == 2 ? 2 : 1);
            uint8_t *dv = dn == 2 ? du + 1 : dp[2] + (size_t)(dy / vertical + y) * ds[2] + dx / 2;
            if (src->fourcc == VA_FOURCC_NV21 || src->fourcc == VA_FOURCC_YV12 ||
                src->fourcc == VA_FOURCC_YV16 || src->fourcc == V4L2_PIX_FMT_NV61) {
                uint8_t *swap = su; su = sv; sv = swap;
            }
            if (dst->fourcc == VA_FOURCC_NV21 || dst->fourcc == VA_FOURCC_YV12 ||
                dst->fourcc == VA_FOURCC_YV16 || dst->fourcc == V4L2_PIX_FMT_NV61) {
                uint8_t *swap = du; du = dv; dv = swap;
            }
            for (unsigned x = 0; x < cw; x++) {
                du[x * (dn == 2 ? 2 : 1)] = su[x * (sn == 2 ? 2 : 1)];
                dv[x * (dn == 2 ? 2 : 1)] = sv[x * (sn == 2 ? 2 : 1)];
            }
        }
    }
    return VA_STATUS_SUCCESS;
}

/* Iterate the actual Annex-B NAL units after range/fragment packetization. */
int vav4l2_annexb_next(const uint8_t *data, size_t size, size_t *offset, const uint8_t **nal, size_t *length)
{
    if (!data || !offset || !nal || !length || size > UINT32_MAX || *offset > size) return -1;
    if (*offset == size) return 0;
    size_t start = *offset;
    while (size - start >= 3) {
        if (!annexb_missing_prefix(data + start, size - start)) break;
        if (data[start++]) return -1;
    }
    if (size - start < 3) return -1;
    unsigned prefix = data[start + 2] == 1 ? 3 : 4;
    if (size - start <= prefix) return -1;
    size_t end = start + prefix + 1;
    while (size - end >= 3 && annexb_missing_prefix(data + end, size - end)) end++;
    if (size - end < 3) end = size;
    *nal = data + start; *length = end - start; *offset = end;
    return 1;
}
