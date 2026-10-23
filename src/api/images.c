/* SPDX-License-Identifier: MIT */
/* Derived from the MIT-noticed vaapi-v4l2-bridge frontend. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <drm_fourcc.h>
#include "internal.h"

static const void *
expbuf_capture_src(struct vareq_surface *surf)
{
    if (!vareq_expbuf_export_wanted() || !surf || surf->gbm_imported || surf->gbm_writable || surf->buf_index < 0 ||
        surf->last_writer == VAREQ_WRITER_CPU)
        return NULL;
    return surf->cap_view;
}

/*
 * Context — one per decode session
 */


static void image_format_metadata(VAImageFormat *f)
{
    f->byte_order = VA_LSB_FIRST;
    f->red_mask = f->green_mask = f->blue_mask = f->alpha_mask = 0;
    switch (f->fourcc) {
    case VA_FOURCC_NV12: case VA_FOURCC_NV21: case VA_FOURCC_I420: case VA_FOURCC_YV12: f->depth = f->bits_per_pixel = 12; break;
    case VA_FOURCC_P010: f->depth = f->bits_per_pixel = 24; break;
    case VA_FOURCC_YUY2: case VA_FOURCC_UYVY: case VA_FOURCC_VYUY: case VA_FOURCC_YVYU:
    case VA_FOURCC_422H: case VA_FOURCC_YV16: f->depth = f->bits_per_pixel = 16; break;
    case VA_FOURCC_Y210: f->depth = 20; f->bits_per_pixel = 32; break;
    }
    if (f->fourcc == VA_FOURCC_RGB565) {
        f->depth = f->bits_per_pixel = 16; f->red_mask = 0xf800; f->green_mask = 0x07e0; f->blue_mask = 0x001f;
    } else if (f->fourcc == VA_FOURCC_RGBA || f->fourcc == VA_FOURCC_RGBX) {
        f->depth = f->fourcc == VA_FOURCC_RGBA ? 32 : 24; f->bits_per_pixel = 32;
        f->red_mask = 0xff; f->green_mask = 0xff00; f->blue_mask = 0xff0000;
        f->alpha_mask = f->fourcc == VA_FOURCC_RGBA ? 0xff000000 : 0;
    } else if (f->fourcc == VA_FOURCC_BGRA || f->fourcc == VA_FOURCC_BGRX) {
        f->depth = f->fourcc == VA_FOURCC_BGRA ? 32 : 24; f->bits_per_pixel = 32;
        f->red_mask = 0xff0000; f->green_mask = 0xff00; f->blue_mask = 0xff;
        f->alpha_mask = f->fourcc == VA_FOURCC_BGRA ? 0xff000000 : 0;
    } else if (f->fourcc == VA_FOURCC_ARGB) {
        f->depth = f->bits_per_pixel = 32;
        f->red_mask = 0xff00; f->green_mask = 0xff0000; f->blue_mask = 0xff000000; f->alpha_mask = 0xff;
    }
}

VAStatus vareq_query_image_formats(VADriverContextP ctx,
                           VAImageFormat *format_list,
                           int *num_formats)
{
    (void)ctx;
    static const struct {
        uint32_t fourcc;
        int depth;
        int bpp;
    } fmts[] = {
        { VA_FOURCC_NV12, 12, 12 },
        { VA_FOURCC_P010, 24, 24 },
        { VA_FOURCC_YUY2, 16, 16 },
        { VA_FOURCC_Y210, 20, 32 },
        { VA_FOURCC_I420, 12, 12 },
        { VA_FOURCC_BGRX, 32, 32 },
        { VA_FOURCC_BGRA, 32, 32 },
        { VA_FOURCC_ARGB, 32, 32 },
        { VA_FOURCC_NV21, 12, 12 },
        { VA_FOURCC_YV12, 12, 12 },
        { VA_FOURCC_UYVY, 16, 16 },
        { VA_FOURCC_VYUY, 16, 16 },
        { VA_FOURCC_YVYU, 16, 16 },
        { VA_FOURCC_422H, 16, 16 },
        { VA_FOURCC_YV16, 16, 16 },
        { VA_FOURCC_RGB565, 16, 16 },
        { VA_FOURCC_RGBA, 32, 32 },
        { VA_FOURCC_RGBX, 24, 32 },
    };
    int count = 0;
    unsigned i;

    for (i = 0; i < sizeof(fmts) / sizeof(fmts[0]); i++) {
        if (format_list) {
            memset(&format_list[count], 0, sizeof(VAImageFormat));
            format_list[count].fourcc = fmts[i].fourcc;
            format_list[count].depth = fmts[i].depth;
            format_list[count].bits_per_pixel = fmts[i].bpp;
            image_format_metadata(&format_list[count]);
        }
        count++;
    }

    if (num_formats)
        *num_formats = count;
    return VA_STATUS_SUCCESS;
}

static void image_layout(VAImage *image, uint32_t fourcc, uint32_t stride, uint32_t height)
{
    image->pitches[0] = stride;
    if (fourcc == VA_FOURCC_NV12 || fourcc == VA_FOURCC_NV21 || fourcc == VA_FOURCC_P010) {
        image->num_planes = 2;
        image->pitches[1] = stride;
        image->offsets[1] = stride * height;
    } else if (fourcc == VA_FOURCC_I420 || fourcc == VA_FOURCC_YV12 ||
               fourcc == VA_FOURCC_422H || fourcc == VA_FOURCC_YV16) {
        image->num_planes = 3;
        image->pitches[1] = image->pitches[2] = (stride + 1) / 2;
        image->offsets[1] = stride * height;
        image->offsets[2] = image->offsets[1] + image->pitches[1] *
            ((fourcc == VA_FOURCC_422H || fourcc == VA_FOURCC_YV16) ? height : (height + 1) / 2);
    } else {
        image->num_planes = 1;
    }
}

VAStatus vareq_derive_image(VADriverContextP ctx,
                    VASurfaceID surface,
                    VAImage *image)
{
    if (!image)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    struct vareq_driver_data *driver_data = ctx->pDriverData;
    struct vareq_surface *surf;

    pthread_mutex_lock(&vareq_api_lock);
    surf = vareq_surface_by_id(driver_data, surface);

    if (!surf) {
        if (vareq_debug)
            fprintf(stderr, "v4l2request: derive lookup fail %#x\n",
                    surface);
        pthread_mutex_unlock(&vareq_api_lock);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    if (surf->status == VASurfaceRendering) {
        pthread_mutex_unlock(&vareq_api_lock);
        return VA_STATUS_ERROR_SURFACE_BUSY;
    }
    if (surf->status == VASurfaceSkipped || (!surf->has_pic && !surf->cpu_ptr)) {
        fprintf(stderr, "v4l2request: derive_image: surface %d has no decoded frame\n",
                surface);
        pthread_mutex_unlock(&vareq_api_lock);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    /* A copied capture/GBM snapshot is not a direct image: CPU writes
     * would be lost to the live decoder/display backing. Let clients use
     * CreateImage + GetImage/PutImage for those surfaces. */
    if (surf->prime || surf->gbm_bo || (surf->buf_index >= 0 &&
        !(surf->cpu_ptr && surf->last_writer == VAREQ_WRITER_CPU))) {
        pthread_mutex_unlock(&vareq_api_lock);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    /* Owning context gives the negotiated capture geometry — the stride
     * can be padded well beyond the display width. */
    struct vareq_context *c = vareq_context_for_surface(driver_data, surface);
    uint32_t stride = surf->stride ? surf->stride :
                      ((c && c->cap_stride) ? c->cap_stride : surf->width);
    uint32_t aligned_h = surf->aligned_h ? surf->aligned_h :
                          ((c && c->cap_height) ? c->cap_height : surf->height);
    uint32_t cap_fcc = surf->cap_fourcc ? surf->cap_fourcc :
                       ((c && c->cap_pixelformat) ? c->cap_pixelformat : V4L2_PIX_FMT_NV12);
    uint32_t va_fcc = vareq_va_fourcc_for_capture(cap_fcc);
    uint32_t data_size;
    void *map;
    int mmapped = 0;

    if (surf->cpu_ptr && (surf->last_writer == VAREQ_WRITER_CPU || !surf->has_pic)) {
        map = surf->cpu_ptr;
        stride = surf->cpu_stride;
        aligned_h = surf->height;
        va_fcc = surf->format ? surf->format : VA_FOURCC_NV12;
        data_size = surf->cpu_size;
        mmapped = 2; /* borrowed cpu backing — do not free on DestroyImage */
        surf->memfd_borrows++;
    } else if (cap_fcc != V4L2_PIX_FMT_NV12) {
        pthread_mutex_unlock(&vareq_api_lock);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    } else {
        if (vareq_surface_ensure_memfd(surf) < 0) {
            pthread_mutex_unlock(&vareq_api_lock);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        data_size = vareq_capture_plane_size(cap_fcc, stride, aligned_h);
        map = vareq_surface_map_memfd(surf, data_size);
        if (!map) {
            fprintf(stderr, "v4l2request: derive_image: map memfd failed: %s\n",
                    strerror(errno));
            pthread_mutex_unlock(&vareq_api_lock);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        /* Borrowed persistent mapping — the surface owns it; DestroyImage
         * releases the borrow (mmapped == 2). */
        mmapped = 2;
        surf->memfd_borrows++;
        va_fcc = VA_FOURCC_NV12;
    }

    struct vareq_buffer *ib = calloc(1, sizeof(*ib));
    if (!ib) {
        if (mmapped == 2 && surf->memfd_borrows)
            surf->memfd_borrows--;
        pthread_mutex_unlock(&vareq_api_lock);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    VABufferID id = ++driver_data->next_buffer_id;
    ib->buffer_id = id;
    ib->type = VAImageBufferType;
    ib->num_elements = 1;
    ib->size = data_size;
    ib->fourcc = va_fcc;
    ib->pitch = stride;
    ib->image_width = surf->width;
    ib->image_height = surf->height;
    ib->image_alloc_height = aligned_h;
    ib->data = map;
    ib->mmapped = mmapped;
    ib->borrow_surf = mmapped == 2 ? surf : NULL;
    ib->next = driver_data->orphan_buffers;
    driver_data->orphan_buffers = ib;

    memset(image, 0, sizeof(*image));
    image->image_id = id;
    image->buf = id;               /* vaMapBuffer(image.buf) returns the mapping */
    image->format.fourcc = va_fcc;
    image_format_metadata(&image->format);
    image->width = surf->width;    /* display size */
    image->height = surf->height;
    image_layout(image, va_fcc, stride, aligned_h);
    image->data_size = data_size;

    pthread_mutex_unlock(&vareq_api_lock);
    return VA_STATUS_SUCCESS;
}

VAStatus vareq_destroy_image(VADriverContextP ctx, VAImageID image_id)
{
    struct vareq_driver_data *driver_data = ctx->pDriverData;

    pthread_mutex_lock(&vareq_api_lock);
    struct vareq_buffer **pp = &driver_data->orphan_buffers;

    while (*pp) {
        if ((*pp)->buffer_id == image_id) {
            struct vareq_buffer *ib = *pp;
            if (ib->type != VAImageBufferType) {
                pthread_mutex_unlock(&vareq_api_lock);
                return VA_STATUS_ERROR_INVALID_IMAGE;
            }
            *pp = ib->next;
            vareq_free_buffer_locked(ib);
            pthread_mutex_unlock(&vareq_api_lock);
            return VA_STATUS_SUCCESS;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&vareq_api_lock);

    return VA_STATUS_ERROR_INVALID_IMAGE;
}

/*
 * CPU-download path used by clients whose derive probe failed (e.g. ffmpeg
 * probes vaDeriveImage with a fresh surface before any decode, then falls
 * back to vaCreateImage + vaGetImage + vaMapBuffer).
 */
VAStatus vareq_create_image(VADriverContextP ctx, VAImageFormat *format,
                    int width, int height, VAImage *image)
{
    struct vareq_driver_data *driver_data = ctx->pDriverData;
    struct vareq_buffer *ib;
    uint32_t stride, aligned_h, data_size;
    VABufferID id;

    if (!format)
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    if (format->fourcc != VA_FOURCC_NV12 && format->fourcc != VA_FOURCC_P010 &&
        format->fourcc != VA_FOURCC_YUY2 && format->fourcc != VA_FOURCC_UYVY &&
        format->fourcc != VA_FOURCC_VYUY && format->fourcc != VA_FOURCC_YVYU && format->fourcc != VA_FOURCC_Y210 &&
        format->fourcc != VA_FOURCC_I420 && format->fourcc != VA_FOURCC_YV12 &&
        format->fourcc != VA_FOURCC_NV21 && format->fourcc != VA_FOURCC_422H && format->fourcc != VA_FOURCC_YV16 &&
        format->fourcc != VA_FOURCC_BGRX && format->fourcc != VA_FOURCC_BGRA &&
        format->fourcc != VA_FOURCC_ARGB && format->fourcc != VA_FOURCC_RGB565 &&
        format->fourcc != VA_FOURCC_RGBA && format->fourcc != VA_FOURCC_RGBX)
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    if (!image || width <= 0 || height <= 0 || width > UINT16_MAX || height > UINT16_MAX)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    stride = vareq_default_image_stride(format->fourcc, width);
    aligned_h = height;
    data_size = vareq_va_image_size(format->fourcc, stride, aligned_h);
    if (!stride || !data_size)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    ib = calloc(1, sizeof(*ib));
    if (!ib)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    ib->data = malloc(data_size);
    if (!ib->data) {
        free(ib);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    pthread_mutex_lock(&vareq_api_lock);
    id = ++driver_data->next_buffer_id;
    ib->buffer_id = id;
    ib->type = VAImageBufferType;
    ib->num_elements = 1;
    ib->size = data_size;
    ib->fourcc = format->fourcc;
    ib->image_width = width;
    ib->image_height = height;
    ib->image_alloc_height = aligned_h;
    ib->pitch = stride;  /* allocation stride — get_image must write with it,
                          * not a stride recomputed from the blit size */
    ib->next = driver_data->orphan_buffers;
    driver_data->orphan_buffers = ib;
    pthread_mutex_unlock(&vareq_api_lock);

    memset(image, 0, sizeof(*image));
    image->image_id = id;
    image->buf = id;
    image->format = *format;
    image_format_metadata(&image->format);
    image->width = width;
    image->height = height;
    image_layout(image, format->fourcc, stride, aligned_h);
    image->data_size = data_size;

    return VA_STATUS_SUCCESS;
}

static struct vareq_image_view image_buffer_view(const struct vareq_buffer *ib)
{
    return (struct vareq_image_view){.data = ib->data, .fourcc = ib->fourcc, .stride = ib->pitch,
        .width = ib->image_width, .height = ib->image_height, .size = ib->size,
        .alloc_height = ib->image_alloc_height ? ib->image_alloc_height : ib->image_height};
}

static struct vareq_image_view surface_cpu_view(const struct vareq_surface *surf)
{
    return (struct vareq_image_view){.data = surf->cpu_ptr, .fourcc = surf->format ? surf->format : VA_FOURCC_NV12,
        .stride = surf->cpu_stride, .width = surf->width, .height = surf->height,
        .alloc_height = surf->height, .size = surf->cpu_size};
}

/* Caller holds vareq_api_lock. Used for GetImage and preserving pixels outside
 * a partial upload before switching a decoded surface to CPU backing. */
VAStatus vareq_surface_read_image_locked(struct vareq_driver_data *driver_data,
                                          struct vareq_surface *surf, struct vareq_buffer *ib,
                                          int x, int y, unsigned width, unsigned height)
{
    if (surf->status == VASurfaceRendering) return VA_STATUS_ERROR_SURFACE_BUSY;
    if (surf->status == VASurfaceSkipped) return VA_STATUS_ERROR_DECODING_ERROR;
    if (vareq_import_refresh(surf) < 0) return VA_STATUS_ERROR_OPERATION_FAILED;
    unsigned image_rows = ib->image_alloc_height ? ib->image_alloc_height : ib->image_height;
    if (surf->cpu_ptr && (surf->last_writer == VAREQ_WRITER_CPU || !surf->has_pic)) {
        struct vareq_image_view source = surface_cpu_view(surf), target = image_buffer_view(ib);
        return vareq_copy_image_rect(&target, 0, 0, &source, x, y, width, height);
    }
    struct vareq_context *c = vareq_context_for_surface(driver_data, surf->surface_id);
    uint32_t src_stride = surf->stride ? surf->stride :
                          ((c && c->cap_stride) ? c->cap_stride : surf->width);
    uint32_t src_alh = surf->aligned_h ? surf->aligned_h :
                       ((c && c->cap_height) ? c->cap_height : surf->height);
    uint32_t cap_fcc = surf->cap_fourcc ? surf->cap_fourcc :
                       ((c && c->cap_pixelformat) ? c->cap_pixelformat : V4L2_PIX_FMT_NV12);
    uint8_t *dst = ib->data;
    const uint8_t *src;
    void *mapped = NULL;
    size_t map_size = 0;
    int copy_w = (int)width;
    int copy_h = (int)height;
    uint32_t dst_fourcc = ib->fourcc;
    uint32_t dst_stride;

    if (!dst_fourcc) {
        /* Legacy default when the image was made without a recorded fourcc */
        if (cap_fcc == V4L2_PIX_FMT_NV15)
            dst_fourcc = VA_FOURCC_P010;
        else if (cap_fcc == V4L2_PIX_FMT_NV16 || cap_fcc == V4L2_PIX_FMT_NV20)
            dst_fourcc = VA_FOURCC_YUY2;
        else
            dst_fourcc = VA_FOURCC_NV12;
    }
    /* Write at the image's allocation stride (reported to the client via
     * VAImage.pitches at create time), never a stride recomputed from the
     * blit size — the client reads rows at the reported pitch. */
    dst_stride = ib->pitch ? ib->pitch
                           : vareq_default_image_stride(dst_fourcc, copy_w);

    /* Lazy memfd: bo-backed surfaces skip the per-frame memfd copy;
     * refill it from the bo now that someone is reading back. */
    if (expbuf_capture_src(surf)) {
        src = expbuf_capture_src(surf);
    } else if (surf->has_pic) {
        if (vareq_surface_ensure_memfd(surf) < 0) {
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        src = NULL;
    } else {
        src = NULL;
    }

    if (src) {
        /* capture mmap already set */
    } else if (surf->memfd_fd >= 0) {
        map_size = vareq_capture_plane_size(cap_fcc, src_stride, src_alh);
        mapped = vareq_surface_map_memfd(surf, map_size);
        if (!mapped) {
            fprintf(stderr, "v4l2request: get_image: map memfd failed: %s\n",
                    strerror(errno));
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        src = mapped;
    } else if (surf->cpu_ptr) {
        src = surf->cpu_ptr;
        src_stride = surf->cpu_stride;
        src_alh = surf->height;
        struct vareq_image_view source = surface_cpu_view(surf), target = image_buffer_view(ib);
        return vareq_copy_image_rect(&target, 0, 0, &source, x, y, width, height);
    } else {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    if (cap_fcc == V4L2_PIX_FMT_NV12) {
        struct vareq_image_view source = {.data = (void *)src, .fourcc = VA_FOURCC_NV12,
            .stride = src_stride, .width = surf->width, .height = surf->height, .alloc_height = src_alh,
            .size = vareq_capture_plane_size(cap_fcc, src_stride, src_alh)};
        struct vareq_image_view target = image_buffer_view(ib);
        return vareq_copy_image_rect(&target, 0, 0, &source, x, y, width, height);
    }
    if (cap_fcc == V4L2_PIX_FMT_NV16 && (dst_fourcc == VA_FOURCC_422H || dst_fourcc == VA_FOURCC_YV16)) {
        struct vareq_image_view source = {.data = (void *)src, .fourcc = V4L2_PIX_FMT_NV16,
            .stride = src_stride, .width = surf->width, .height = surf->height, .alloc_height = src_alh,
            .size = vareq_capture_plane_size(cap_fcc, src_stride, src_alh)};
        struct vareq_image_view target = image_buffer_view(ib);
        return vareq_copy_image_rect(&target, 0, 0, &source, x, y, width, height);
    }
    if (x || y) {
        /* Packed capture samples cannot be addressed at an arbitrary byte
         * offset. Convert at the original origin, then use the shared image
         * rectangle rules for chroma alignment and allocation pitches. */
        struct vareq_buffer converted = {.fourcc = dst_fourcc,
            .image_width = surf->width, .image_height = surf->height,
            .image_alloc_height = surf->height};
        converted.pitch = vareq_default_image_stride(dst_fourcc, surf->width);
        converted.size = vareq_va_image_size(dst_fourcc, converted.pitch, surf->height);
        if (!converted.pitch || !converted.size)
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        converted.data = malloc(converted.size);
        if (!converted.data) return VA_STATUS_ERROR_ALLOCATION_FAILED;
        VAStatus status = vareq_surface_read_image_locked(driver_data, surf, &converted,
                                                     0, 0, surf->width, surf->height);
        if (status == VA_STATUS_SUCCESS) {
            struct vareq_image_view source = image_buffer_view(&converted), target = image_buffer_view(ib);
            status = vareq_copy_image_rect(&target, 0, 0, &source, x, y, width, height);
        }
        free(converted.data);
        return status;
    }

    if ((width & 1) && (width != surf->width || width != ib->image_width))
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (cap_fcc == V4L2_PIX_FMT_NV15 && (height & 1) &&
        (height != surf->height || height != ib->image_height))
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    uint32_t src_row = (cap_fcc == V4L2_PIX_FMT_NV15 || cap_fcc == V4L2_PIX_FMT_NV20)
        ? ((uint32_t)copy_w + 3) / 4 * 5 : ((uint32_t)copy_w + 1) & ~1u;
    uint32_t dst_row = (uint32_t)copy_w *
        ((dst_fourcc == VA_FOURCC_P010 || dst_fourcc == VA_FOURCC_YUY2) ? 2u :
         (dst_fourcc == VA_FOURCC_Y210 || dst_fourcc == VA_FOURCC_ARGB ||
          dst_fourcc == VA_FOURCC_BGRA || dst_fourcc == VA_FOURCC_BGRX) ? 4u : 1u);
    if (copy_w <= 0 || copy_h <= 0 || (uint32_t)copy_h > src_alh || src_row > src_stride ||
        dst_row > dst_stride ||
        !vareq_va_image_size(dst_fourcc, dst_stride, image_rows) ||
        vareq_va_image_size(dst_fourcc, dst_stride, image_rows) > ib->size) {
        fprintf(stderr, "v4l2request: get_image: requested copy exceeds source or destination geometry\n");
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    if (dst_fourcc == VA_FOURCC_P010 && cap_fcc == V4L2_PIX_FMT_NV15)
        vareq_nv15_to_p010(dst, dst_stride, image_rows, src, src_stride, src_alh, copy_w, copy_h);
    else if (dst_fourcc == VA_FOURCC_Y210 && cap_fcc == V4L2_PIX_FMT_NV20)
        return vareq_nv20_to_y210(dst, dst_stride, src, src_stride, src_alh, copy_w, copy_h) < 0
            ? VA_STATUS_ERROR_ALLOCATION_FAILED : VA_STATUS_SUCCESS;
    else if (dst_fourcc == VA_FOURCC_YUY2 && cap_fcc == V4L2_PIX_FMT_NV20)
        return vareq_nv20_to_yuy2(dst, dst_stride, src, src_stride, src_alh, copy_w, copy_h) < 0
            ? VA_STATUS_ERROR_ALLOCATION_FAILED : VA_STATUS_SUCCESS;
    else if (dst_fourcc == VA_FOURCC_YUY2 && cap_fcc == V4L2_PIX_FMT_NV16)
        vareq_nv16_to_yuy2(dst, dst_stride, src, src_stride, src_alh, copy_w, copy_h);
    else {
        fprintf(stderr, "v4l2request: get_image: no path cap=%.4s -> %.4s\n",
                (char *)&cap_fcc, (char *)&dst_fourcc);
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }

    return VA_STATUS_SUCCESS;
}

VAStatus vareq_get_image(VADriverContextP ctx, VASurfaceID surface,
                 int x, int y, unsigned int width, unsigned int height,
                 VAImageID image_id)
{
    struct vareq_driver_data *driver_data = ctx->pDriverData;
    struct vareq_surface *surf;
    struct vareq_buffer *ib;

    pthread_mutex_lock(&vareq_api_lock);
    surf = vareq_surface_by_id(driver_data, surface);
    if (!surf) {
        pthread_mutex_unlock(&vareq_api_lock);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    if (surf->status == VASurfaceRendering) {
        pthread_mutex_unlock(&vareq_api_lock);
        return VA_STATUS_ERROR_SURFACE_BUSY;
    }
    if (surf->status == VASurfaceSkipped || (!surf->has_pic && !surf->cpu_ptr)) {
        pthread_mutex_unlock(&vareq_api_lock);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    if (surf->memfd_borrows) {
        pthread_mutex_unlock(&vareq_api_lock);
        return VA_STATUS_ERROR_SURFACE_BUSY;
    }
    if (x < 0 || y < 0 || (unsigned)x > surf->width || (unsigned)y > surf->height ||
        !width || !height || width > surf->width - (unsigned)x || height > surf->height - (unsigned)y) {
        pthread_mutex_unlock(&vareq_api_lock);
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    ib = driver_data->orphan_buffers;
    while (ib && ib->buffer_id != image_id)
        ib = ib->next;
    if (!ib || ib->type != VAImageBufferType) {
        pthread_mutex_unlock(&vareq_api_lock);
        return VA_STATUS_ERROR_INVALID_IMAGE;
    }
    if (width > ib->image_width || height > ib->image_height) {
        pthread_mutex_unlock(&vareq_api_lock);
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    VAStatus status = vareq_surface_read_image_locked(driver_data, surf, ib, x, y, width, height);
    vareq_decode_trace(vareq_context_for_surface(driver_data, surface), "get-image", (int)status, surface);
    pthread_mutex_unlock(&vareq_api_lock);
    return status;
}

VAStatus vareq_put_image(VADriverContextP ctx,
                 VASurfaceID surface,
                 VAImageID image,
                 int src_x, int src_y,
                 unsigned int src_width, unsigned int src_height,
                 int dest_x, int dest_y,
                 unsigned int dest_width, unsigned int dest_height)
{
    struct vareq_driver_data *driver_data = ctx->pDriverData;
    struct vareq_surface *surf;
    struct vareq_buffer *ib;

    pthread_mutex_lock(&vareq_api_lock);
    surf = vareq_surface_by_id(driver_data, surface);
    if (!surf) {
        pthread_mutex_unlock(&vareq_api_lock);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    if (surf->status == VASurfaceRendering) {
        pthread_mutex_unlock(&vareq_api_lock);
        return VA_STATUS_ERROR_SURFACE_BUSY;
    }
    ib = driver_data->orphan_buffers;
    while (ib && ib->buffer_id != image)
        ib = ib->next;
    if (!ib) {
        pthread_mutex_unlock(&vareq_api_lock);
        return VA_STATUS_ERROR_INVALID_IMAGE;
    }

    if (ib->type != VAImageBufferType) {
        pthread_mutex_unlock(&vareq_api_lock);
        return VA_STATUS_ERROR_INVALID_IMAGE;
    }
    if (surf->memfd_borrows) {
        pthread_mutex_unlock(&vareq_api_lock);
        return VA_STATUS_ERROR_SURFACE_BUSY;
    }
    if (src_x < 0 || src_y < 0 || dest_x < 0 || dest_y < 0 || !src_width || !src_height ||
        (unsigned)src_x > ib->image_width || (unsigned)src_y > ib->image_height ||
        src_width > ib->image_width - (unsigned)src_x || src_height > ib->image_height - (unsigned)src_y ||
        (unsigned)dest_x > surf->width || (unsigned)dest_y > surf->height ||
        dest_width > surf->width - (unsigned)dest_x || dest_height > surf->height - (unsigned)dest_y ||
        dest_width != src_width || dest_height != src_height) {
        pthread_mutex_unlock(&vareq_api_lock);
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    if (vareq_surface_ensure_cpu(surf) < 0) {
        pthread_mutex_unlock(&vareq_api_lock);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    if ((dest_x || dest_y || dest_width != surf->width || dest_height != surf->height) && vareq_import_refresh(surf) < 0) {
        pthread_mutex_unlock(&vareq_api_lock);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    if (surf->has_pic && surf->last_writer != VAREQ_WRITER_CPU &&
        (dest_x || dest_y || dest_width != surf->width || dest_height != surf->height)) {
        struct vareq_buffer backing = {.data = surf->cpu_ptr, .fourcc = surf->format,
            .pitch = surf->cpu_stride, .size = surf->cpu_size,
            .image_width = surf->width, .image_height = surf->height};
        VAStatus status = vareq_surface_read_image_locked(driver_data, surf, &backing, 0, 0, surf->width, surf->height);
        if (status != VA_STATUS_SUCCESS) {
            pthread_mutex_unlock(&vareq_api_lock);
            return status;
        }
    }
    struct vareq_image_view source = image_buffer_view(ib), target = surface_cpu_view(surf);
    VAStatus status = vareq_copy_image_rect(&target, dest_x, dest_y, &source, src_x, src_y, src_width, src_height);
    if (status != VA_STATUS_SUCCESS) {
        pthread_mutex_unlock(&vareq_api_lock);
        return status;
    }
    surf->last_writer = VAREQ_WRITER_CPU;
    surf->has_pic = 1;
    surf->status = VASurfaceReady;
    surf->prime_capture_valid = 0;
    if ((surf->prime && vareq_prime_write(surf) < 0) ||
        (surf->gbm_bo && vareq_gbm_surface_upload(surf, surf->cpu_ptr, surf->cpu_stride, surf->height) < 0)) {
        pthread_mutex_unlock(&vareq_api_lock);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    pthread_mutex_unlock(&vareq_api_lock);
    return VA_STATUS_SUCCESS;
}
