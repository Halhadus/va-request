/*
 * VPP via discovered raw M2M nodes: scale, CSC, rotate, flip.
 * Stateful M2M. Filters the kernel does not expose (denoise, deinterlace)
 * are reported as unsupported.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <drm_fourcc.h>
#include <linux/videodev2.h>
#include <linux/dma-buf.h>
#include <linux/v4l2-controls.h>
#include <va/va.h>
#include <va/va_vpp.h>

#include "va_v4l2.h"
#include "probe.h"
#include "compat/compat.h"

#define xioctl(fd, req, arg) vav4l2_xioctl((fd), (req), (arg))

static uint32_t va_to_v4l2_raw(uint32_t fourcc)
{
    switch (fourcc) {
    case VA_FOURCC_NV12:
        return V4L2_PIX_FMT_NV12;
    case VA_FOURCC_NV21:
        return V4L2_PIX_FMT_NV21;
    case VA_FOURCC_YV12:
        /* Keep the kernel transport in canonical Y/U/V order. The raw
         * queue boundary repacks VA Y/V/U without changing public layout. */
        return V4L2_PIX_FMT_YUV420;
    case VA_FOURCC_422H:
        return V4L2_PIX_FMT_NV16;
    case VA_FOURCC_YV16:
        return V4L2_PIX_FMT_NV61;
    case VA_FOURCC_YUY2:
        return V4L2_PIX_FMT_YUYV;
    case VA_FOURCC_UYVY:
        return V4L2_PIX_FMT_UYVY;
    case VA_FOURCC_VYUY:
        return V4L2_PIX_FMT_VYUY;
    case VA_FOURCC_YVYU:
        return V4L2_PIX_FMT_YVYU;
    case VA_FOURCC_I420:
        return V4L2_PIX_FMT_YUV420;
    case VA_FOURCC_RGB565:
        return V4L2_PIX_FMT_RGB565;
    case VA_FOURCC_RGBA:
        return V4L2_PIX_FMT_RGBA32;
    case VA_FOURCC_RGBX:
        return V4L2_PIX_FMT_RGBX32;
    case VA_FOURCC_ARGB:
        return V4L2_PIX_FMT_ARGB32;
    case VA_FOURCC_BGRA:
        return V4L2_PIX_FMT_ABGR32;
    case VA_FOURCC_BGRX:
        return V4L2_PIX_FMT_XBGR32;
    default:
        return 0;
    }
}

/* Planar public 422 layouts use native semiplanar queue transport. */
static uint32_t queue_image_fourcc(uint32_t fourcc)
{
    if (fourcc == VA_FOURCC_422H || fourcc == VA_FOURCC_YV16) return va_to_v4l2_raw(fourcc);
    return fourcc == VA_FOURCC_YV12 ? VA_FOURCC_I420 : fourcc;
}

static int has_format(const uint32_t *formats, unsigned count, uint32_t wanted)
{
    for (unsigned i = 0; i < count; i++) if (formats[i] == wanted) return 1;
    return 0;
}

static int raw_rgb(uint32_t fcc)
{
    return fcc == V4L2_PIX_FMT_ARGB32 || fcc == V4L2_PIX_FMT_ABGR32 || fcc == V4L2_PIX_FMT_XBGR32 ||
        fcc == V4L2_PIX_FMT_RGBA32 || fcc == V4L2_PIX_FMT_RGBX32 || fcc == V4L2_PIX_FMT_RGB565;
}

int vav4l2_vpp_device_score_job(const struct vav4l2_vpp_device *d, const struct vav4l2_compat_job *request)
{
    if (!has_format(d->input, d->num_input, request->src) || !has_format(d->output, d->num_output, request->dst) ||
        (request->rotate && !(d->controls & VAV4L2_VPP_ROTATE)) ||
        (request->hflip && !(d->controls & VAV4L2_VPP_HFLIP)) || (request->vflip && !(d->controls & VAV4L2_VPP_VFLIP))) return -1;
    struct vav4l2_compat compat;
    if (vav4l2_compat_resolve(&d->identity, &compat) < 0) return -1;
    struct vav4l2_compat_job job = *request;
    job.src_rgb = raw_rgb(job.src); job.dst_rgb = raw_rgb(job.dst);
    return vav4l2_compat_vpp_score(&compat, &job);
}
int vav4l2_vpp_device_score(const struct vav4l2_vpp_device *d, uint32_t src, uint32_t dst,
                          int rotate, int hflip, int vflip, int crop_offset, int compose)
{
    struct vav4l2_compat_job job = {.src = src, .dst = dst, .rotate = rotate, .hflip = hflip, .vflip = vflip,
        .crop_offset = crop_offset, .compose = compose};
    return vav4l2_vpp_device_score_job(d, &job);
}

void vav4l2_vpp_transform_controls(unsigned behaviors, int *rotate, int *hflip, int *vflip)
{
    if (*hflip && *vflip) { *rotate = (*rotate + 180) % 360; *hflip = *vflip = 0; }
    if (behaviors & VAV4L2_COMPAT_SWAP_MIRROR_AXES) { int value = *hflip; *hflip = *vflip; *vflip = value; }
}

static unsigned raw_planes(uint32_t fourcc)
{
    return fourcc == V4L2_PIX_FMT_NV12M || fourcc == V4L2_PIX_FMT_NV21M ? 2 : 1;
}

static int job_dimensions(int fd, unsigned sw, unsigned sh, uint32_t src,
                          unsigned dw, unsigned dh, uint32_t dst)
{
    for (unsigned cap = 0; cap < 2; cap++) {
        unsigned w = cap ? dw : sw, h = cap ? dh : sh;
        uint32_t fcc = cap ? dst : src;
        struct v4l2_format fmt = {.type = cap ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE : V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE};
        fmt.fmt.pix_mp.width = w; fmt.fmt.pix_mp.height = h;
        fmt.fmt.pix_mp.pixelformat = fcc; fmt.fmt.pix_mp.num_planes = raw_planes(fcc);
        fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
        if (xioctl(fd, VIDIOC_TRY_FMT, &fmt) < 0 || fmt.fmt.pix_mp.width != w || fmt.fmt.pix_mp.height != h ||
            fmt.fmt.pix_mp.pixelformat != fcc || fmt.fmt.pix_mp.num_planes != raw_planes(fcc)) return 0;
    }
    return 1;
}

static int select_device(struct vav4l2_context *ctx, uint32_t src, uint32_t dst,
                         unsigned sw, unsigned sh, unsigned dw, unsigned dh,
                         int rotate, int hflip, int vflip, int crop_offset, int compose, int warm, unsigned excluded, unsigned src_quantization, unsigned dst_quantization)
{
    /* Unit contexts can supply their own fd without a discovered path. */
    if (!ctx->device_path) { ctx->vpp_controls = 7; return 0; }
    struct vav4l2_driver_data *dd = ctx->driver_data;
    vav4l2_scan_vpp_devices(dd);
    unsigned order[VAV4L2_VPP_MAX_DEVICES], count = 0;
    int scores[VAV4L2_VPP_MAX_DEVICES];
    for (unsigned i = 0; i < dd->num_vpp_devices; i++) {
        if (excluded & (1u << i)) continue;
        const struct vav4l2_vpp_device *d = &dd->vpp_devices[i];
        struct vav4l2_compat_job job = {.src = src, .dst = dst, .rotate = rotate, .hflip = hflip, .vflip = vflip,
            .crop_offset = crop_offset, .compose = compose, .src_quantization = src_quantization, .dst_quantization = dst_quantization};
        scores[i] = vav4l2_vpp_device_score_job(d, &job);
        if (scores[i] < 0) continue;
        if (warm && !strcmp(ctx->device_path, d->path)) return 0;
        unsigned at = count++;
        while (at && scores[order[at - 1]] < scores[i]) { order[at] = order[at - 1]; at--; }
        order[at] = i;
    }
    for (unsigned i = 0; i < count; i++) {
        const struct vav4l2_vpp_device *d = &dd->vpp_devices[order[i]];
        int same = !strcmp(ctx->device_path, d->path);
        int fd = same ? ctx->v4l2_fd : open(d->path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        if (!job_dimensions(fd, sw, sh, src, dw, dh, dst)) { if (!same) close(fd); continue; }
        if (!same) {
            char *path = strdup(d->path);
            if (!path) { close(fd); return -1; }
            vav4l2_m2m_teardown(ctx, &ctx->vpp_q); close(ctx->v4l2_fd);
            free(ctx->device_path); ctx->device_path = path; ctx->v4l2_fd = fd;
        }
        struct vav4l2_compat compat;
        if (vav4l2_compat_resolve(&d->identity, &compat) < 0) return -1;
        ctx->vpp_controls = d->controls; ctx->vpp_behaviors = compat.behaviors;
        if (vav4l2_debug) for (unsigned j = 0; j < compat.count; j++)
            fprintf(stderr, "v4l2: compatibility %s on %s\n", compat.modules[j]->name, d->path);
        if (vav4l2_debug) fprintf(stderr, "v4l2: VPP selected %s card=%s controls=%u\n", d->path, d->identity.card, d->controls);
        return 0;
    }
    fprintf(stderr, "v4l2: no VPP node accepts formats/geometry/transform\n");
    return -1;
}

static int set_transform(int fd, unsigned supported, unsigned bit, unsigned id, int value)
{
    if (!(supported & bit)) return value ? -1 : 0;
    struct v4l2_control ctrl = {.id = id, .value = value};
    return xioctl(fd, VIDIOC_S_CTRL, &ctrl);
}

static const uint32_t vpp_pixel_formats[] = {
    VA_FOURCC_NV12,
    VA_FOURCC_YUY2,
    VA_FOURCC_I420,
    VA_FOURCC_BGRX,
    VA_FOURCC_BGRA,
    VA_FOURCC_ARGB,
    VA_FOURCC_NV21,
    VA_FOURCC_YV12,
    VA_FOURCC_UYVY,
    VA_FOURCC_VYUY,
    VA_FOURCC_YVYU,
    VA_FOURCC_422H,
    VA_FOURCC_YV16,
    VA_FOURCC_RGB565,
    VA_FOURCC_RGBA,
    VA_FOURCC_RGBX,
};

static const VAProcColorStandardType vpp_color_in[] = {
    VAProcColorStandardBT601,
    VAProcColorStandardBT709,
};
static const VAProcColorStandardType vpp_color_out[] = {
    VAProcColorStandardBT601,
    VAProcColorStandardBT709,
};

VAStatus vav4l2_vpp_query_filters(VAProcFilterType *filters, unsigned int *n)
{
    /* Rotation and mirroring are pipeline controls, not VAProcFilter types. */
    if (!n)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    (void)filters;
    *n = 0;
    return VA_STATUS_SUCCESS;
}

VAStatus vav4l2_vpp_query_filter_caps(VAProcFilterType type, void *caps,
                                      unsigned int *n)
{
    (void)type;
    (void)caps;
    if (n)
        *n = 0;
    return VA_STATUS_ERROR_UNSUPPORTED_FILTER;
}

VAStatus vav4l2_vpp_query_pipeline_caps(VAProcPipelineCaps *caps)
{
    if (!caps)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    memset(caps, 0, sizeof(*caps));
    caps->rotation_flags = (1u << VA_ROTATION_NONE) | (1u << VA_ROTATION_90) |
                           (1u << VA_ROTATION_180) | (1u << VA_ROTATION_270);
    caps->mirror_flags = VA_MIRROR_HORIZONTAL | VA_MIRROR_VERTICAL;
    caps->num_input_color_standards = 2;
    caps->input_color_standards = (VAProcColorStandardType *)vpp_color_in;
    caps->num_output_color_standards = 2;
    caps->output_color_standards = (VAProcColorStandardType *)vpp_color_out;
    caps->num_input_pixel_formats = (uint32_t)(sizeof(vpp_pixel_formats) / sizeof(vpp_pixel_formats[0]));
    caps->input_pixel_format = (uint32_t *)vpp_pixel_formats;
    caps->num_output_pixel_formats = caps->num_input_pixel_formats;
    caps->output_pixel_format = (uint32_t *)vpp_pixel_formats;
    caps->min_input_width = 2;
    caps->min_input_height = 2;
    caps->max_input_width = 8192;
    caps->max_input_height = 8192;
    caps->min_output_width = 2;
    caps->min_output_height = 2;
    caps->max_output_width = 8192;
    caps->max_output_height = 8192;
    return VA_STATUS_SUCCESS;
}

VAStatus vav4l2_vpp_query_device_caps(struct vav4l2_driver_data *dd, VAProcPipelineCaps *caps)
{
    if (!caps) return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (dd->vpp_caps_ready) { *caps = dd->vpp_caps; return VA_STATUS_SUCCESS; }
    VAStatus st = vav4l2_vpp_query_pipeline_caps(caps);
    if (st != VA_STATUS_SUCCESS) return st;
    vav4l2_scan_vpp_devices(dd);
    if (!dd->num_vpp_devices) return VA_STATUS_ERROR_OPERATION_FAILED;
    caps->rotation_flags = 1u << VA_ROTATION_NONE; caps->mirror_flags = 0;
    caps->num_input_pixel_formats = caps->num_output_pixel_formats = 0;
    caps->input_pixel_format = dd->vpp_input_formats; caps->output_pixel_format = dd->vpp_output_formats;
    caps->min_input_width = caps->min_input_height = caps->min_output_width = caps->min_output_height = UINT32_MAX;
    caps->max_input_width = caps->max_input_height = caps->max_output_width = caps->max_output_height = 0;
    for (unsigned i = 0; i < dd->num_vpp_devices; i++) {
        const struct vav4l2_vpp_device *d = &dd->vpp_devices[i];
        if (d->controls & VAV4L2_VPP_ROTATE) caps->rotation_flags |= (1u << VA_ROTATION_90) | (1u << VA_ROTATION_180) | (1u << VA_ROTATION_270);
        if (d->controls & VAV4L2_VPP_HFLIP) caps->mirror_flags |= VA_MIRROR_HORIZONTAL;
        if (d->controls & VAV4L2_VPP_VFLIP) caps->mirror_flags |= VA_MIRROR_VERTICAL;
        if (d->min_width[0] < caps->min_input_width) caps->min_input_width = d->min_width[0];
        if (d->min_height[0] < caps->min_input_height) caps->min_input_height = d->min_height[0];
        if (d->min_width[1] < caps->min_output_width) caps->min_output_width = d->min_width[1];
        if (d->min_height[1] < caps->min_output_height) caps->min_output_height = d->min_height[1];
        if (d->max_width[0] > caps->max_input_width) caps->max_input_width = d->max_width[0];
        if (d->max_height[0] > caps->max_input_height) caps->max_input_height = d->max_height[0];
        if (d->max_width[1] > caps->max_output_width) caps->max_output_width = d->max_width[1];
        if (d->max_height[1] > caps->max_output_height) caps->max_output_height = d->max_height[1];
    }
    for (unsigned i = 0; i < sizeof(vpp_pixel_formats) / sizeof(vpp_pixel_formats[0]); i++) {
        int in = 0, out = 0;
        for (unsigned j = 0; j < dd->num_vpp_devices; j++) {
            const struct vav4l2_vpp_device *d = &dd->vpp_devices[j];
            uint32_t fcc = va_to_v4l2_raw(vpp_pixel_formats[i]);
            in |= has_format(d->input, d->num_input, fcc); out |= has_format(d->output, d->num_output, fcc);
        }
        if (in) dd->vpp_input_formats[caps->num_input_pixel_formats++] = vpp_pixel_formats[i];
        if (out) dd->vpp_output_formats[caps->num_output_pixel_formats++] = vpp_pixel_formats[i];
    }
    dd->vpp_caps = *caps; dd->vpp_caps_ready = 1;
    return VA_STATUS_SUCCESS;
}

/* Copy each plane at its own allocation pitch and chroma offset. */
static void copy_raw(uint8_t *dst, uint32_t dst_stride, uint32_t dst_h,
                     const uint8_t *src, uint32_t src_stride, uint32_t src_h,
                     uint32_t fourcc, int width, int height, int bpp)
{
    if (fourcc == VA_FOURCC_NV12 || fourcc == VA_FOURCC_NV21) {
        vav4l2_copy_nv12(dst, dst_stride, dst_h, src, src_stride, src_h, width, height);
        return;
    }
    for (int y = 0; y < height; y++)
        memcpy(dst + (size_t)y * dst_stride, src + (size_t)y * src_stride, (size_t)width * bpp);
    if (fourcc == VA_FOURCC_I420 || fourcc == VA_FOURCC_YV12) {
        dst += (size_t)dst_stride * dst_h;
        src += (size_t)src_stride * src_h;
        dst_stride = (dst_stride + 1) / 2;
        src_stride = (src_stride + 1) / 2;
        for (int plane = 0; plane < 2; plane++) {
            for (int y = 0; y < (height + 1) / 2; y++)
                memcpy(dst + (size_t)y * dst_stride, src + (size_t)y * src_stride, (size_t)(width + 1) / 2);
            dst += (size_t)dst_stride * ((dst_h + 1) / 2);
            src += (size_t)src_stride * ((src_h + 1) / 2);
        }
    }
}

static int valid_region(const VARectangle *r, unsigned width, unsigned height, uint32_t fcc)
{
    if (r->x < 0 || r->y < 0 || !r->width || !r->height ||
        (unsigned)r->x > width || (unsigned)r->y > height ||
        r->width > width - (unsigned)r->x || r->height > height - (unsigned)r->y)
        return 0;
    if ((fcc == VA_FOURCC_NV12 || fcc == VA_FOURCC_NV21 || fcc == VA_FOURCC_I420 || fcc == VA_FOURCC_YV12) &&
        ((r->x | r->y | r->width | r->height) & 1)) return 0;
    if ((fcc == VA_FOURCC_YUY2 || fcc == VA_FOURCC_UYVY || fcc == VA_FOURCC_VYUY || fcc == VA_FOURCC_YVYU ||
         fcc == VA_FOURCC_422H || fcc == VA_FOURCC_YV16) && ((r->x | r->width) & 1)) return 0;
    return 1;
}

static int set_regions(int fd, struct vav4l2_m2m_state *q, const VARectangle *src,
                       const VARectangle *dst, int partial)
{
    if (!partial && !q->selection_active) return 0;
    const VARectangle *regions[] = {src, dst};
    for (unsigned i = 0; i < 2; i++) {
        struct v4l2_selection selection = {.type = i ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE :
            V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, .target = i ? V4L2_SEL_TGT_COMPOSE : V4L2_SEL_TGT_CROP,
            .flags = V4L2_SEL_FLAG_GE | V4L2_SEL_FLAG_LE,
            .r = {.left = regions[i]->x, .top = regions[i]->y,
                  .width = regions[i]->width, .height = regions[i]->height}};
        struct v4l2_rect requested = selection.r;
        if (xioctl(fd, VIDIOC_S_SELECTION, &selection) < 0 ||
            memcmp(&requested, &selection.r, sizeof(requested))) {
            fprintf(stderr, "v4l2: VPP crop/compose not accepted exactly\n");
            return -1;
        }
    }
    q->selection_active = partial;
    return 0;
}

static int color_format(struct v4l2_pix_format_mplane *pix, VAProcColorStandardType standard,
                        const VAProcColorProperties *properties, int capture)
{
    if (standard != VAProcColorStandardNone && standard != VAProcColorStandardBT601 &&
        standard != VAProcColorStandardBT709) return -1;
    if (properties->color_range > VA_SOURCE_RANGE_FULL)
        return -1;
    if (standard != VAProcColorStandardNone) {
        pix->colorspace = standard == VAProcColorStandardBT709 ? V4L2_COLORSPACE_REC709 : V4L2_COLORSPACE_SMPTE170M;
        pix->ycbcr_enc = standard == VAProcColorStandardBT709 ? V4L2_YCBCR_ENC_709 : V4L2_YCBCR_ENC_601;
        pix->xfer_func = V4L2_XFER_FUNC_709;
    }
    if (properties->color_range)
        pix->quantization = properties->color_range == VA_SOURCE_RANGE_FULL ?
            V4L2_QUANTIZATION_FULL_RANGE : V4L2_QUANTIZATION_LIM_RANGE;
    if (capture && (standard != VAProcColorStandardNone || properties->color_range))
        pix->flags |= V4L2_PIX_FMT_FLAG_SET_CSC;
    return 0;
}

static int color_matches(const struct v4l2_pix_format_mplane *actual,
                         const struct v4l2_pix_format_mplane *wanted)
{
    unsigned ycbcr = actual->ycbcr_enc ? actual->ycbcr_enc : V4L2_MAP_YCBCR_ENC_DEFAULT(actual->colorspace);
    int rgb = raw_rgb(actual->pixelformat);
    unsigned range = actual->quantization ? actual->quantization :
        V4L2_MAP_QUANTIZATION_DEFAULT(rgb, actual->colorspace, ycbcr);
    return (!wanted->colorspace || wanted->colorspace == actual->colorspace) &&
           (!wanted->ycbcr_enc || wanted->ycbcr_enc == ycbcr) &&
           (!wanted->quantization || wanted->quantization == range);
}

static unsigned clip_byte(int value)
{
    return value < 0 ? 0 : value > 255 ? 255 : (unsigned)value;
}

/* Fill the destination backing after successful processing. Alpha zero keeps
 * existing pixels outside output_region; fractional background blending is
 * not implemented. YUV uses the negotiated matrix and range. */
static void fill_background(struct vav4l2_surface *dst, uint32_t argb,
                            const struct v4l2_pix_format_mplane *fmt)
{
    if (!(argb >> 24)) return;
    unsigned r = (argb >> 16) & 255, g = (argb >> 8) & 255, b = argb & 255;
    unsigned matrix709 = fmt->ycbcr_enc == V4L2_YCBCR_ENC_709 ||
        (!fmt->ycbcr_enc && fmt->colorspace == V4L2_COLORSPACE_REC709);
    int y, u, v;
    if (fmt->quantization == V4L2_QUANTIZATION_FULL_RANGE) {
        y = ((matrix709 ? 54 : 77) * (int)r + (matrix709 ? 183 : 150) * (int)g + (matrix709 ? 19 : 29) * (int)b + 128) >> 8;
        u = 128 + ((-(matrix709 ? 29 : 43) * (int)r - (matrix709 ? 99 : 85) * (int)g + 128 * (int)b + 128) >> 8);
        v = 128 + ((128 * (int)r - (matrix709 ? 116 : 107) * (int)g - (matrix709 ? 12 : 21) * (int)b + 128) >> 8);
    } else {
        y = 16 + (((matrix709 ? 47 : 66) * (int)r + (matrix709 ? 157 : 129) * (int)g + (matrix709 ? 16 : 25) * (int)b + 128) >> 8);
        u = 128 + ((-(matrix709 ? 26 : 38) * (int)r - (matrix709 ? 87 : 74) * (int)g + 112 * (int)b + 128) >> 8);
        v = 128 + ((112 * (int)r - (matrix709 ? 102 : 94) * (int)g - (matrix709 ? 10 : 18) * (int)b + 128) >> 8);
    }
    uint8_t *base = dst->cpu_ptr;
    for (unsigned row = 0; row < dst->height; row++) {
        uint8_t *p = base + (size_t)row * dst->cpu_stride;
        for (unsigned x = 0; x < dst->width; x++) {
            if (dst->format == VA_FOURCC_ARGB) { p[0] = 255; p[1] = r; p[2] = g; p[3] = b; p += 4; }
            else if (dst->format == VA_FOURCC_BGRA || dst->format == VA_FOURCC_BGRX) {
                p[0] = b; p[1] = g; p[2] = r; p[3] = 255; p += 4;
            } else if (dst->format == VA_FOURCC_RGBA || dst->format == VA_FOURCC_RGBX) {
                p[0] = r; p[1] = g; p[2] = b; p[3] = 255; p += 4;
            } else if (dst->format == VA_FOURCC_RGB565) {
                unsigned value = (r >> 3) << 11 | (g >> 2) << 5 | (b >> 3);
                p[0] = value; p[1] = value >> 8; p += 2;
            } else if (dst->format == VA_FOURCC_VYUY) {
                p[0] = clip_byte((x & 1) ? u : v); p[1] = clip_byte(y); p += 2;
            } else if (dst->format == VA_FOURCC_YVYU) {
                p[0] = clip_byte(y); p[1] = clip_byte((x & 1) ? u : v); p += 2;
            } else if (dst->format == VA_FOURCC_YUY2) {
                p[0] = clip_byte(y); p[1] = clip_byte((x & 1) ? v : u); p += 2;
            } else if (dst->format == VA_FOURCC_UYVY) {
                p[0] = clip_byte((x & 1) ? v : u); p[1] = clip_byte(y); p += 2;
            } else *p++ = clip_byte(y);
        }
    }
    uint8_t *uv = base + (size_t)dst->cpu_stride * dst->height;
    if (dst->format == VA_FOURCC_NV12 || dst->format == VA_FOURCC_NV21) {
        for (unsigned row = 0; row < (dst->height + 1) / 2; row++)
            for (unsigned x = 0; x < dst->width; x += 2) {
                uv[(size_t)row * dst->cpu_stride + x] = clip_byte(dst->format == VA_FOURCC_NV21 ? v : u);
                uv[(size_t)row * dst->cpu_stride + x + 1] = clip_byte(dst->format == VA_FOURCC_NV21 ? u : v);
            }
    } else if (dst->format == VA_FOURCC_I420 || dst->format == VA_FOURCC_YV12 ||
               dst->format == VA_FOURCC_422H || dst->format == VA_FOURCC_YV16) {
        unsigned stride = (dst->cpu_stride + 1) / 2;
        unsigned height = dst->format == VA_FOURCC_422H || dst->format == VA_FOURCC_YV16 ? dst->height : (dst->height + 1) / 2;
        for (unsigned row = 0; row < height; row++) {
            memset(uv + (size_t)row * stride, clip_byte((dst->format == VA_FOURCC_YV12 || dst->format == VA_FOURCC_YV16) ? v : u), (dst->width + 1) / 2);
            memset(uv + (size_t)(height + row) * stride, clip_byte((dst->format == VA_FOURCC_YV12 || dst->format == VA_FOURCC_YV16) ? u : v), (dst->width + 1) / 2);
        }
    }
}

#include "dmabuf.h"

static VAStatus run_pipeline(struct vav4l2_context *ctx,
                        struct vav4l2_buffer **buffers, int num_buffers)
{
    VAProcPipelineParameterBuffer *pipe = NULL;
    struct vav4l2_surface *src, *dst;
    int fd = ctx->v4l2_fd;
    int i, rotate = 0, hflip = 0, vflip = 0;
    int sw, sh, dw, dh;
    uint32_t src_fcc, dst_fcc;
    struct v4l2_format ofmt, cfmt;
    struct v4l2_requestbuffers oreq, creq;
    struct v4l2_buffer obuf, cbuf;
    struct v4l2_plane opl[1], cpl[1];
    struct pollfd pfd;
    const uint8_t *srcp = NULL;
    void *src_map = NULL;
    size_t src_map_sz = 0;
    uint32_t src_stride, src_alh;
    uint8_t *dstp;
    int bpp_src = 1, bpp_dst = 1;
    VAStatus st;

    for (i = 0; i < num_buffers; i++) {
        if (buffers[i] && buffers[i]->type == VAProcPipelineParameterBufferType)
            pipe = buffers[i]->data;
    }
    if (!pipe)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    src = vav4l2_surface_by_id(ctx->driver_data, pipe->surface);
    dst = ctx->current_surface;
    if (!src || !dst)
        return VA_STATUS_ERROR_INVALID_SURFACE;
    if (src != dst && src->status == VASurfaceRendering)
        return VA_STATUS_ERROR_SURFACE_BUSY;

    sw = src->width; sh = src->height; dw = dst->width; dh = dst->height;
    VARectangle input = {0, 0, sw, sh}, output = {0, 0, dw, dh};
    if (pipe->surface_region) input = *pipe->surface_region;
    if (pipe->output_region) output = *pipe->output_region;
    int partial_input = input.x || input.y || input.width != sw || input.height != sh;
    int partial_output = output.x || output.y || output.width != dw || output.height != dh;
    if (dst->memfd_borrows) return VA_STATUS_ERROR_SURFACE_BUSY;
    if (pipe->rotation_state > VA_ROTATION_270 || (pipe->mirror_state & ~(VA_MIRROR_HORIZONTAL | VA_MIRROR_VERTICAL)))
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (pipe->num_additional_outputs || pipe->input_surface_flag || pipe->output_surface_flag ||
        pipe->blend_state || (pipe->pipeline_flags & ~VA_PROC_PIPELINE_FAST))
        return VA_STATUS_ERROR_UNIMPLEMENTED;
    if (partial_output && (pipe->output_background_color >> 24) != 0 && (pipe->output_background_color >> 24) != 255)
        return VA_STATUS_ERROR_UNIMPLEMENTED;
    unsigned filter_color = pipe->filter_flags & VA_SRC_COLOR_MASK;
    VAProcColorStandardType input_standard = pipe->surface_color_standard;
    if (filter_color) {
        VAProcColorStandardType requested = filter_color == VA_SRC_BT601 ? VAProcColorStandardBT601 :
            filter_color == VA_SRC_BT709 ? VAProcColorStandardBT709 : VAProcColorStandardNone;
        if (!requested || (input_standard && input_standard != requested)) return VA_STATUS_ERROR_INVALID_PARAMETER;
        input_standard = requested;
    }
    /* Scaling quality is a preference within the available processing path. */
    unsigned scaling = pipe->filter_flags & VA_FILTER_SCALING_MASK;
    if (scaling != VA_FILTER_SCALING_DEFAULT && scaling != VA_FILTER_SCALING_FAST && scaling != VA_FILTER_SCALING_HQ)
        return VA_STATUS_ERROR_UNIMPLEMENTED;
    if (pipe->filter_flags & ~(VA_SRC_COLOR_MASK | VA_FILTER_SCALING_MASK))
        return VA_STATUS_ERROR_UNIMPLEMENTED;
    if (pipe->num_filters || pipe->num_forward_references || pipe->num_backward_references)
        return VA_STATUS_ERROR_UNIMPLEMENTED;
    if (pipe->rotation_state == VA_ROTATION_90 ||
        pipe->rotation_state == VA_ROTATION_270)
        rotate = (pipe->rotation_state == VA_ROTATION_90) ? 90 : 270;
    else if (pipe->rotation_state == VA_ROTATION_180)
        rotate = 180;
    if (pipe->mirror_state & VA_MIRROR_HORIZONTAL)
        hflip = 1;
    if (pipe->mirror_state & VA_MIRROR_VERTICAL)
        vflip = 1;

    src_fcc = src->format ? src->format : VA_FOURCC_NV12;
    dst_fcc = dst->format ? dst->format : VA_FOURCC_NV12;
    int native_capture = !src->prime && !src->gbm_bo;
    int decoded_nv16 = native_capture && src->has_pic && src->last_writer != VAV4L2_WRITER_CPU && src->cap_fourcc == V4L2_PIX_FMT_NV16;
    if (native_capture && src->has_pic && src->last_writer != VAV4L2_WRITER_CPU && src->cap_fourcc == V4L2_PIX_FMT_NV12)
        src_fcc = VA_FOURCC_NV12;
    if (decoded_nv16) src_fcc = VA_FOURCC_YUY2;
    int src_ok = 0, dst_ok = 0;
    for (unsigned j = 0; j < sizeof(vpp_pixel_formats) / sizeof(vpp_pixel_formats[0]); j++) {
        src_ok |= src_fcc == vpp_pixel_formats[j];
        dst_ok |= dst_fcc == vpp_pixel_formats[j];
    }
    if (!src_ok || !dst_ok || (native_capture && src->has_pic && src->last_writer != VAV4L2_WRITER_CPU &&
        src->cap_fourcc != V4L2_PIX_FMT_NV12 && !decoded_nv16))
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    if (!valid_region(&input, src->width, src->height, src_fcc) ||
        !valid_region(&output, dst->width, dst->height, dst_fcc)) return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (src->status == VASurfaceSkipped || (!src->has_pic && !src->cpu_ptr))
        return VA_STATUS_ERROR_INVALID_SURFACE;
    bpp_src = (src_fcc == VA_FOURCC_YUY2 || src_fcc == VA_FOURCC_UYVY || src_fcc == VA_FOURCC_VYUY || src_fcc == VA_FOURCC_YVYU || src_fcc == VA_FOURCC_RGB565) ? 2 :
              (src_fcc == VA_FOURCC_ARGB || src_fcc == VA_FOURCC_BGRA ||
               src_fcc == VA_FOURCC_BGRX || src_fcc == VA_FOURCC_RGBA || src_fcc == VA_FOURCC_RGBX) ? 4 : 1;
    bpp_dst = (dst_fcc == VA_FOURCC_YUY2 || dst_fcc == VA_FOURCC_UYVY || dst_fcc == VA_FOURCC_VYUY || dst_fcc == VA_FOURCC_YVYU || dst_fcc == VA_FOURCC_RGB565) ? 2 :
              (dst_fcc == VA_FOURCC_ARGB || dst_fcc == VA_FOURCC_BGRA ||
               dst_fcc == VA_FOURCC_BGRX || dst_fcc == VA_FOURCC_RGBA || dst_fcc == VA_FOURCC_RGBX) ? 4 : 1;

    int src_rgb = raw_rgb(va_to_v4l2_raw(src_fcc)), dst_rgb = raw_rgb(va_to_v4l2_raw(dst_fcc));
    unsigned input_siting = pipe->input_color_properties.chroma_sample_location;
    unsigned output_siting = pipe->output_color_properties.chroma_sample_location;
    /* Identical siting on like YUV layouts describes the samples; it does
     * not request a phase conversion. No phase-changing control is exposed. */
    if ((input_siting || output_siting) &&
        (src_rgb || dst_rgb || src_fcc != dst_fcc || input_siting != output_siting ||
         (input_siting & ~0x0fu) || (input_siting & 0x0cu) == 0x0cu))
        return VA_STATUS_ERROR_UNIMPLEMENTED;
    VAProcColorStandardType output_standard = pipe->output_color_standard;
    if (!src_rgb && !dst_rgb && output_standard && output_standard != input_standard)
        return VA_STATUS_ERROR_UNIMPLEMENTED; /* no YUV-to-YUV matrix transform */
    if (src_rgb && !dst_rgb && output_standard) {
        if (input_standard && input_standard != output_standard) return VA_STATUS_ERROR_UNIMPLEMENTED;
        input_standard = output_standard; /* An unspecified RGB matrix follows the requested output standard. */
    }
    struct v4l2_pix_format_mplane input_color = {0}, output_color = {0};
    if (color_format(&input_color, input_standard, &pipe->input_color_properties, 0) < 0 ||
        color_format(&output_color, output_standard, &pipe->output_color_properties, 1) < 0)
        return VA_STATUS_ERROR_UNIMPLEMENTED;
    if ((!partial_output || (dst->has_pic && !(pipe->output_background_color >> 24))) && src != dst) {
        int direct = run_dmabuf(ctx, src, dst, rotate, hflip, vflip, &input_color, &output_color, &input, &output);
        if (direct) return direct > 0 ? VA_STATUS_SUCCESS : VA_STATUS_ERROR_OPERATION_FAILED;
    }
    if (vav4l2_import_refresh(src) < 0) return VA_STATUS_ERROR_OPERATION_FAILED;
    if (partial_output && dst->has_pic) {
        if (vav4l2_import_refresh(dst) < 0 ||
            (dst->gbm_bo && dst->last_writer == VAV4L2_WRITER_BO && vav4l2_gbm_read_cpu(dst) < 0))
            return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    if (src->last_writer == VAV4L2_WRITER_CPU && src->cpu_ptr) {
        srcp = src->cpu_ptr;
        src_stride = src->cpu_stride;
        src_alh = src->height;
    } else if (vav4l2_expbuf_export_wanted() && src->cap_view &&
        src->buf_index >= 0 && src->stride) {
        /* Decode snapshot lives in the capture mmap; memfd was never filled. */
        srcp = src->cap_view;
        src_stride = src->stride;
        src_alh = src->aligned_h ? src->aligned_h : src->height;
    } else if (src->memfd_fd >= 0 && src->has_pic && src->stride) {
        if (vav4l2_surface_ensure_memfd(src) < 0)
            return VA_STATUS_ERROR_OPERATION_FAILED;
        src_map_sz = vav4l2_capture_plane_size(
            src->cap_fourcc ? src->cap_fourcc : V4L2_PIX_FMT_NV12,
            src->stride, src->aligned_h ? src->aligned_h : src->height);
        if (!src_map_sz) return VA_STATUS_ERROR_INVALID_PARAMETER;
        src_map = vav4l2_surface_map_memfd(src, src_map_sz);
        if (!src_map)
            return VA_STATUS_ERROR_OPERATION_FAILED;
        srcp = src_map;
        src_stride = src->stride;
        src_alh = src->aligned_h ? src->aligned_h : src->height;
    } else if (src->cpu_ptr) {
        if (decoded_nv16) return VA_STATUS_ERROR_INVALID_SURFACE;
        srcp = src->cpu_ptr;
        src_stride = src->cpu_stride ? src->cpu_stride : src->width * bpp_src;
        src_alh = src->height;
    } else {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    if (src_stride < (uint32_t)sw * (decoded_nv16 ? 1 : bpp_src) || src_alh < (uint32_t)sh)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    if (src_fcc == VA_FOURCC_ARGB || src_fcc == VA_FOURCC_BGRA || src_fcc == VA_FOURCC_RGBA) {
        unsigned alpha = src_fcc == VA_FOURCC_ARGB ? 0 : 3;
        for (unsigned y = 0; y < input.height; y++)
            for (unsigned x = 0; x < input.width; x++)
                if (srcp[(size_t)(input.y + y) * src_stride + (input.x + x) * 4 + alpha] != 255)
                    return VA_STATUS_ERROR_UNIMPLEMENTED; /* Fractional-alpha processing is not implemented. */
    }
    if (!dst->cpu_ptr) {
        dst->cpu_stride = vav4l2_default_image_stride(dst_fcc, dst->width);
        dst->cpu_size = vav4l2_va_image_size(dst_fcc, dst->cpu_stride, dst->height);
        if (vav4l2_surface_ensure_cpu(dst) < 0)
            dst->cpu_ptr = NULL;
        if (!dst->cpu_ptr)
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        dst->format = dst_fcc;
    }
    dstp = dst->cpu_ptr;

    {
        struct vav4l2_m2m_state *q = &ctx->vpp_q;
        uint32_t key[8] = {
            src->width, src->height, src_fcc, dw, dh, dst_fcc,
            (uint32_t)(rotate / 90) | ((uint32_t)hflip << 8) | ((uint32_t)vflip << 16),
            input_color.ycbcr_enc | (input_color.quantization << 4) |
            (output_color.ycbcr_enc << 8) | (output_color.quantization << 12)
        };
        int renegot = !q->valid || q->out_memory == V4L2_MEMORY_DMABUF ||
            q->cap_memory == V4L2_MEMORY_DMABUF || memcmp(q->key, key, sizeof(key));

        if (select_device(ctx, va_to_v4l2_raw(src_fcc), va_to_v4l2_raw(dst_fcc), sw, sh, dw, dh,
                          rotate, hflip, vflip, input.x || input.y, partial_output, !renegot, 0, input_color.quantization, output_color.quantization) < 0) {
            st = VA_STATUS_ERROR_UNIMPLEMENTED;
            goto fail;
        }
        fd = ctx->v4l2_fd;
        renegot |= !q->valid;
        vav4l2_vpp_transform_controls(ctx->vpp_behaviors, &rotate, &hflip, &vflip);

        if (renegot) {
            vav4l2_m2m_teardown(ctx, q);
            if (ctx->v4l2_fd < 0) { st = VA_STATUS_ERROR_OPERATION_FAILED; goto fail; }
            q->valid = 1; /* teardown also owns partial setup on failure */

            memset(&ofmt, 0, sizeof(ofmt));
            ofmt.fmt.pix_mp = input_color;
            ofmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
            ofmt.fmt.pix_mp.width = src->width;
            ofmt.fmt.pix_mp.height = src->height;
            ofmt.fmt.pix_mp.pixelformat = va_to_v4l2_raw(src_fcc);
            ofmt.fmt.pix_mp.num_planes = 1;
            ofmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
            if (xioctl(fd, VIDIOC_S_FMT, &ofmt) < 0) {
                fprintf(stderr, "v4l2: VPP S_FMT out: %s\n", strerror(errno));
                st = VA_STATUS_ERROR_OPERATION_FAILED;
                goto fail;
            }
            memset(&cfmt, 0, sizeof(cfmt));
            cfmt.fmt.pix_mp = output_color;
            cfmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            cfmt.fmt.pix_mp.width = dw;
            cfmt.fmt.pix_mp.height = dh;
            cfmt.fmt.pix_mp.pixelformat = va_to_v4l2_raw(dst_fcc);
            cfmt.fmt.pix_mp.num_planes = 1;
            cfmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
            if (xioctl(fd, VIDIOC_S_FMT, &cfmt) < 0) {
                fprintf(stderr, "v4l2: VPP S_FMT cap: %s\n", strerror(errno));
                st = VA_STATUS_ERROR_OPERATION_FAILED;
                goto fail;
            }

            if (!color_matches(&ofmt.fmt.pix_mp, &input_color) || !color_matches(&cfmt.fmt.pix_mp, &output_color) ||
                ofmt.fmt.pix_mp.num_planes != 1 || cfmt.fmt.pix_mp.num_planes != 1 ||
                ofmt.fmt.pix_mp.pixelformat != va_to_v4l2_raw(src_fcc) ||
                cfmt.fmt.pix_mp.pixelformat != va_to_v4l2_raw(dst_fcc) ||
                ofmt.fmt.pix_mp.width != (uint32_t)sw || ofmt.fmt.pix_mp.height != (uint32_t)sh ||
                cfmt.fmt.pix_mp.width != (uint32_t)dw || cfmt.fmt.pix_mp.height != (uint32_t)dh) {
                st = VA_STATUS_ERROR_OPERATION_FAILED;
                goto fail;
            }
            q->ofmt = ofmt;
            q->cfmt = cfmt;
            if (set_regions(fd, q, &input, &output, partial_input || partial_output) < 0) {
                st = VA_STATUS_ERROR_OPERATION_FAILED;
                goto fail;
            }
            if (set_transform(fd, ctx->vpp_controls, VAV4L2_VPP_ROTATE, V4L2_CID_ROTATE, rotate) < 0 ||
                set_transform(fd, ctx->vpp_controls, VAV4L2_VPP_HFLIP, V4L2_CID_HFLIP, hflip) < 0 ||
                set_transform(fd, ctx->vpp_controls, VAV4L2_VPP_VFLIP, V4L2_CID_VFLIP, vflip) < 0) {
                st = VA_STATUS_ERROR_OPERATION_FAILED;
                goto fail;
            }

            memset(&oreq, 0, sizeof(oreq));
            oreq.count = 1;
            oreq.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
            oreq.memory = V4L2_MEMORY_MMAP;
            if (xioctl(fd, VIDIOC_REQBUFS, &oreq) < 0) {
                st = VA_STATUS_ERROR_OPERATION_FAILED;
                goto fail;
            }
            memset(&creq, 0, sizeof(creq));
            creq.count = 1;
            creq.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            creq.memory = V4L2_MEMORY_MMAP;
            if (xioctl(fd, VIDIOC_REQBUFS, &creq) < 0) {
                st = VA_STATUS_ERROR_OPERATION_FAILED;
                goto fail;
            }

            memset(&obuf, 0, sizeof(obuf));
            memset(opl, 0, sizeof(opl));
            obuf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
            obuf.memory = V4L2_MEMORY_MMAP;
            obuf.index = 0;
            obuf.length = 1;
            obuf.m.planes = opl;
            if (xioctl(fd, VIDIOC_QUERYBUF, &obuf) < 0) {
                st = VA_STATUS_ERROR_OPERATION_FAILED;
                goto fail;
            }
            q->out_len[0] = opl[0].length;
            q->out_map[0] = mmap(NULL, opl[0].length,
                                 PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                                 opl[0].m.mem_offset);

            memset(&cbuf, 0, sizeof(cbuf));
            memset(cpl, 0, sizeof(cpl));
            cbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            cbuf.memory = V4L2_MEMORY_MMAP;
            cbuf.index = 0;
            cbuf.length = 1;
            cbuf.m.planes = cpl;
            if (xioctl(fd, VIDIOC_QUERYBUF, &cbuf) < 0) {
                st = VA_STATUS_ERROR_OPERATION_FAILED;
                goto fail;
            }
            q->cap_len = cpl[0].length;
            q->cap_map = mmap(NULL, cpl[0].length,
                              PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                              cpl[0].m.mem_offset);

            if (q->out_map[0] == MAP_FAILED || q->cap_map == MAP_FAILED) {
                st = VA_STATUS_ERROR_OPERATION_FAILED;
                goto fail;
            }
            q->out_len[0] = opl[0].length;
            q->cap_len = cpl[0].length;
            q->out_planes = 1;
            memcpy(q->key, key, sizeof(key));
            q->valid = 1;
        }

        if (!renegot && set_regions(fd, q, &input, &output, partial_input || partial_output) < 0) {
            st = VA_STATUS_ERROR_OPERATION_FAILED;
            goto fail;
        }

        uint32_t out_stride = q->ofmt.fmt.pix_mp.plane_fmt[0].bytesperline;
        uint32_t cap_stride = q->cfmt.fmt.pix_mp.plane_fmt[0].bytesperline;
        if (!out_stride) out_stride = (uint32_t)sw * bpp_src;
        if (!cap_stride) cap_stride = (uint32_t)dw * bpp_dst;
        uint32_t out_size = vav4l2_va_image_size(queue_image_fourcc(src_fcc), out_stride, sh);
        uint32_t cap_size = vav4l2_va_image_size(queue_image_fourcc(dst_fcc), cap_stride, dh);
        if (out_stride < (uint32_t)sw * bpp_src || cap_stride < (uint32_t)dw * bpp_dst ||
            !out_size || !cap_size || out_size > q->out_len[0] || cap_size > q->cap_len) {
            st = VA_STATUS_ERROR_OPERATION_FAILED;
            goto fail;
        }
        if (decoded_nv16) {
            /* Lossless 8-bit 4:2:2 staging. Capture chroma is full-height;
             * the VPP queue receives the already supported packed YUY2. */
            vav4l2_nv16_to_yuy2(q->out_map[0], out_stride, srcp, src_stride, src_alh, sw, sh);
        } else if (src_fcc == VA_FOURCC_YV12 || src_fcc == VA_FOURCC_422H || src_fcc == VA_FOURCC_YV16) {
            struct vav4l2_image_view source = {.data = (void *)srcp, .fourcc = src_fcc,
                .stride = src_stride, .width = sw, .height = sh, .alloc_height = src_alh, .size = src->cpu_size};
            struct vav4l2_image_view target = {.data = q->out_map[0], .fourcc = queue_image_fourcc(src_fcc),
                .stride = out_stride, .width = sw, .height = sh, .alloc_height = sh, .size = q->out_len[0]};
            st = vav4l2_copy_image_rect(&target, 0, 0, &source, 0, 0, sw, sh);
            if (st != VA_STATUS_SUCCESS) goto fail;
        } else copy_raw(q->out_map[0], out_stride, sh, srcp, src_stride, src_alh, src_fcc, sw, sh, bpp_src);

        {
            enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
            if (xioctl(fd, VIDIOC_STREAMON, &t) < 0) {
                st = VA_STATUS_ERROR_OPERATION_FAILED;
                goto fail;
            }
            q->streaming |= 1;
            t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            if (xioctl(fd, VIDIOC_STREAMON, &t) < 0) {
                st = VA_STATUS_ERROR_OPERATION_FAILED;
                goto fail;
            }
            q->streaming |= 2;
        }
        memset(&cbuf, 0, sizeof(cbuf));
        memset(cpl, 0, sizeof(cpl));
        cbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        cbuf.memory = V4L2_MEMORY_MMAP;
        cbuf.index = 0;
        cbuf.length = 1;
        cbuf.m.planes = cpl;
        if (xioctl(fd, VIDIOC_QBUF, &cbuf) < 0) {
            st = VA_STATUS_ERROR_OPERATION_FAILED;
            goto fail;
        }
        memset(&obuf, 0, sizeof(obuf));
        memset(opl, 0, sizeof(opl));
        obuf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        obuf.memory = V4L2_MEMORY_MMAP;
        obuf.index = 0;
        obuf.length = 1;
        obuf.m.planes = opl;
        opl[0].length = q->out_len[0];
        opl[0].bytesused = out_size;
        if (xioctl(fd, VIDIOC_QBUF, &obuf) < 0) {
            st = VA_STATUS_ERROR_OPERATION_FAILED;
            goto fail;
        }

        pfd.fd = fd;
        pfd.events = POLLIN;
        if (vav4l2_poll_intr(&pfd, 1, 2000) <= 0) {
            fprintf(stderr, "v4l2: VPP timeout\n");
            st = VA_STATUS_ERROR_OPERATION_FAILED;
            goto fail;
        }
        memset(&cbuf, 0, sizeof(cbuf));
        memset(cpl, 0, sizeof(cpl));
        cbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        cbuf.memory = V4L2_MEMORY_MMAP;
        cbuf.length = 1;
        cbuf.m.planes = cpl;
        if (xioctl(fd, VIDIOC_DQBUF, &cbuf) < 0) {
            fprintf(stderr, "v4l2: VPP DQBUF: %s\n", strerror(errno));
            st = VA_STATUS_ERROR_OPERATION_FAILED;
            goto fail;
        }
        if (cbuf.index != 0 || cbuf.length != 1 || (cbuf.flags & V4L2_BUF_FLAG_ERROR) || cpl[0].data_offset > cpl[0].bytesused ||
            cpl[0].bytesused > q->cap_len || cpl[0].bytesused - cpl[0].data_offset < cap_size) {
            st = VA_STATUS_ERROR_OPERATION_FAILED;
            goto fail;
        }
        if (vav4l2_m2m_finish_output(ctx, q) < 0) {
            st = VA_STATUS_ERROR_OPERATION_FAILED;
            goto fail;
        }
        if (partial_output) fill_background(dst, pipe->output_background_color, &q->cfmt.fmt.pix_mp);
        struct vav4l2_image_view source = {.data = (uint8_t *)q->cap_map + cpl[0].data_offset,
            .fourcc = queue_image_fourcc(dst_fcc),
            .stride = cap_stride, .width = dst->width, .height = dst->height,
            .alloc_height = dh, .size = cap_size};
        struct vav4l2_image_view target = {.data = dstp, .fourcc = dst_fcc, .stride = dst->cpu_stride,
            .width = dst->width, .height = dst->height, .size = dst->cpu_size};
        st = vav4l2_copy_image_rect(&target, output.x, output.y, &source, output.x, output.y, output.width, output.height);
        if (st != VA_STATUS_SUCCESS) goto fail;
    }

    st = VA_STATUS_SUCCESS;
    dst->has_pic = 1;
    dst->last_writer = VAV4L2_WRITER_CPU;
    dst->prime_capture_valid = 0;
    if ((dst->prime && vav4l2_prime_write(dst) < 0) ||
        (dst->gbm_bo && vav4l2_gbm_surface_upload(dst, dst->cpu_ptr, dst->cpu_stride, dst->height) < 0)) {
        st = VA_STATUS_ERROR_OPERATION_FAILED;
        goto fail;
    }
    /* Keep the queue: a per-frame STREAMOFF returns the buffers to
     * userspace while the allocation and mappings stay valid. */
    {
        enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        int output_stopped = xioctl(fd, VIDIOC_STREAMOFF, &t);
        if (output_stopped == 0) ctx->vpp_q.streaming &= ~1u;
        t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        int capture_stopped = xioctl(fd, VIDIOC_STREAMOFF, &t);
        if (capture_stopped == 0) ctx->vpp_q.streaming &= ~2u;
        if (output_stopped < 0 || capture_stopped < 0) {
            st = VA_STATUS_ERROR_OPERATION_FAILED;
            goto fail;
        }
    }
    if (vav4l2_debug)
        fprintf(stderr, "v4l2: VPP %dx%d -> %dx%d rot=%d\n",
                src->width, src->height, dw, dh, rotate);
    return st;

fail:
    vav4l2_m2m_teardown(ctx, &ctx->vpp_q);
    return st;
}

static VAStatus run_and_trace(struct vav4l2_context *ctx, struct vav4l2_buffer **buffers, int num_buffers)
{
    VAStatus st = run_pipeline(ctx, buffers, num_buffers);
    const char *path = getenv("VAV4L2_VPP_TRACE");
    if (!path || !path[0]) return st;
    VAProcPipelineParameterBuffer *pipe = NULL;
    for (int i = 0; i < num_buffers; i++) if (buffers[i] && buffers[i]->type == VAProcPipelineParameterBufferType) pipe = buffers[i]->data;
    if (!pipe) return st;
    FILE *file = fopen(path, "a");
    if (!file) { perror("VPP trace"); return st; }
    if (ftell(file) == 0) fprintf(file, "STATUS\tNODE\tCARD\tSRC_FOURCC\tDST_FOURCC\tSOURCE_SIZE\tTARGET_SIZE\tCROP\tCOMPOSE\tROTATION\tMIRROR\tFILTER_FLAGS\tINPUT_COLOR\tOUTPUT_COLOR\tREQUEST_SITING\tQUEUE_VALID\tOUTPUT_MEMORY\tCAPTURE_MEMORY\tOUTPUT_PLANES\tCAPTURE_PLANES\tCONTEXT\n");
    const struct v4l2_pix_format_mplane *in = &ctx->vpp_q.ofmt.fmt.pix_mp, *out = &ctx->vpp_q.cfmt.fmt.pix_mp;
    struct vav4l2_surface *src = vav4l2_surface_by_id(ctx->driver_data, pipe->surface), *dst = ctx->current_surface;
    const char *card = "unknown";
    for (unsigned i = 0; i < ctx->driver_data->num_vpp_devices; i++)
        if (ctx->device_path && !strcmp(ctx->device_path, ctx->driver_data->vpp_devices[i].path)) card = ctx->driver_data->vpp_devices[i].identity.card;
    VARectangle crop = {0}, compose = {0};
    if (src) { crop.width = src->width; crop.height = src->height; }
    if (dst) { compose.width = dst->width; compose.height = dst->height; }
    if (pipe->surface_region) crop = *pipe->surface_region;
    if (pipe->output_region) compose = *pipe->output_region;
    fprintf(file, "%u\t%s\t%s\t%08x\t%08x\t%ux%u\t%ux%u\t%d,%d,%u,%u\t%d,%d,%u,%u\t%u\t%u\t%x\t%u,%u,%u\t%u,%u,%u\t%u,%u\t%u\t%u\t%u\t%u\t%u\t%u\n",
        st, ctx->device_path ? ctx->device_path : "-", card, src ? src->format : 0, dst ? dst->format : 0,
        src ? src->width : 0, src ? src->height : 0, dst ? dst->width : 0, dst ? dst->height : 0,
        crop.x, crop.y, crop.width, crop.height, compose.x, compose.y, compose.width, compose.height,
        pipe->rotation_state, pipe->mirror_state, pipe->filter_flags, in->colorspace, in->ycbcr_enc, in->quantization,
        out->colorspace, out->ycbcr_enc, out->quantization, pipe->input_color_properties.chroma_sample_location,
        pipe->output_color_properties.chroma_sample_location, ctx->vpp_q.valid,
        ctx->vpp_q.valid ? (ctx->vpp_q.out_memory ? ctx->vpp_q.out_memory : V4L2_MEMORY_MMAP) : 0,
        ctx->vpp_q.valid ? (ctx->vpp_q.cap_memory ? ctx->vpp_q.cap_memory : V4L2_MEMORY_MMAP) : 0,
        in->num_planes, out->num_planes, ctx->context_id);
    if (fclose(file)) perror("close VPP trace");
    return st;
}

VAStatus vav4l2_vpp_run(struct vav4l2_context *ctx, struct vav4l2_buffer **buffers, int num_buffers)
{
    if (num_buffers <= 0 || !buffers) return VA_STATUS_ERROR_INVALID_PARAMETER;
    /* Validate the whole buffer list before the first composition pass. */
    for (int i = 0; i < num_buffers; i++) {
        const struct vav4l2_buffer *b = buffers[i];
        if (!b || !b->data || b->type != VAProcPipelineParameterBufferType ||
            b->size < sizeof(VAProcPipelineParameterBuffer) ||
            (b->allocated_elements && b->num_elements > b->allocated_elements))
            return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    for (int i = 0; i < num_buffers; i++) {
        const struct vav4l2_buffer *b = buffers[i];
        unsigned count = b->num_elements ? b->num_elements : 1;
        for (unsigned j = 0; j < count; j++) {
            struct vav4l2_buffer element = *b;
            element.data = (uint8_t *)b->data + (size_t)j * b->size;
            element.num_elements = element.allocated_elements = 1;
            struct vav4l2_buffer *one[] = {&element};
            VAStatus st = run_and_trace(ctx, one, 1);
            if (st != VA_STATUS_SUCCESS) return st;
        }
    }
    return VA_STATUS_SUCCESS;
}

const int vav4l2_has_vpp = 1;
