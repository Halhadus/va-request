/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Native planar storage with independent kernel objects and canonical readback. */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <drm_fourcc.h>
#include "va_v4l2.h"
struct vav4l2_capture_layout {
    uint32_t fourcc, canonical, planes, tile_width, tile_height;
    uint32_t stride[3], size[3], rows[3];
    struct { void *map; uint32_t length; } buffers[VAV4L2_MAX_CAPTURE_BUFS][3];
};
unsigned vav4l2_capture_layout_planes(uint32_t f)
{
    switch (f) {
    case V4L2_PIX_FMT_NV12M: case V4L2_PIX_FMT_NV21M:
    case V4L2_PIX_FMT_NV16M: case V4L2_PIX_FMT_NV61M:
    case V4L2_PIX_FMT_NV12MT_16X16: case V4L2_PIX_FMT_NV12M_8L128: return 2;
    case V4L2_PIX_FMT_YUV420M: case V4L2_PIX_FMT_YVU420M:
    case V4L2_PIX_FMT_YUV422M: case V4L2_PIX_FMT_YVU422M: return 3;
    default: return 0;
    }
}
uint32_t vav4l2_capture_layout_format(uint32_t f)
{
    if (!vav4l2_capture_layout_planes(f)) return 0;
    return f == V4L2_PIX_FMT_NV16M || f == V4L2_PIX_FMT_NV61M ||
        f == V4L2_PIX_FMT_YUV422M || f == V4L2_PIX_FMT_YVU422M ? V4L2_PIX_FMT_NV16 : V4L2_PIX_FMT_NV12;
}
int vav4l2_capture_layout_set(struct vav4l2_context *ctx, const struct v4l2_pix_format_mplane *f)
{
    unsigned planes = vav4l2_capture_layout_planes(f->pixelformat);
    if (!planes || f->num_planes != planes || f->field != V4L2_FIELD_NONE ||
        !f->width || !f->height || f->width > 8192 || f->height > 8192 || (f->width & 1)) { errno = ENOTSUP; return -1; }
    struct vav4l2_capture_layout layout = {.fourcc = f->pixelformat, .planes = planes,
        .canonical = vav4l2_capture_layout_format(f->pixelformat)};
    if (f->pixelformat == V4L2_PIX_FMT_NV12MT_16X16) { layout.tile_width = 16; layout.tile_height = 16; }
    if (f->pixelformat == V4L2_PIX_FMT_NV12M_8L128) { layout.tile_width = 8; layout.tile_height = 128; }
    for (unsigned i = 0; i < planes; i++) {
        layout.stride[i] = f->plane_fmt[i].bytesperline; layout.size[i] = f->plane_fmt[i].sizeimage;
        layout.rows[i] = i && layout.canonical == V4L2_PIX_FMT_NV12 ? (f->height + 1) / 2 : f->height;
        unsigned width = i && planes == 3 ? f->width / 2 : f->width;
        if (layout.tile_height) {
            if ((layout.stride[i] % layout.tile_width) || (f->height % layout.tile_height)) { errno = EINVAL; return -1; }
            layout.rows[i] = (layout.rows[i] + layout.tile_height - 1) / layout.tile_height * layout.tile_height;
        }
        if (layout.stride[i] < width || (uint64_t)layout.stride[i] * layout.rows[i] > layout.size[i]) { errno = EINVAL; return -1; }
    }
    uint32_t canonical_size = vav4l2_capture_plane_size(layout.canonical, layout.stride[0], f->height);
    if (!canonical_size) { errno = EINVAL; return -1; }
    if (ctx->capture_layout) { errno = EBUSY; return -1; }
    ctx->capture_layout = malloc(sizeof(layout)); if (!ctx->capture_layout) return -1;
    *ctx->capture_layout = layout;
    ctx->cap_width = f->width; ctx->cap_height = f->height; ctx->cap_stride = layout.stride[0];
    ctx->cap_pixelformat = layout.canonical;
    ctx->cap_sizeimage = canonical_size;
    ctx->capture_buf_size = ctx->cap_sizeimage;
    return 0;
}
void vav4l2_capture_layout_release(struct vav4l2_context *ctx)
{
    if (!ctx->capture_layout) return;
    for (unsigned b = 0; b < VAV4L2_MAX_CAPTURE_BUFS; b++) for (unsigned p = 0; p < ctx->capture_layout->planes; p++)
        if (ctx->capture_layout->buffers[b][p].map)
            munmap(ctx->capture_layout->buffers[b][p].map, ctx->capture_layout->buffers[b][p].length);
    free(ctx->capture_layout); ctx->capture_layout = NULL;
}
int vav4l2_decoder_queue_capture(struct vav4l2_context *ctx, int index)
{
    if (!ctx || index < 0 || index >= ctx->capture_bufs_allocd || index >= VAV4L2_MAX_CAPTURE_BUFS) { errno = EINVAL; return -1; }
    for (unsigned p = 0; p < ctx->capture_exports[index].count; p++)
        if (vav4l2_dmabuf_wait(ctx->capture_exports[index].fd[p], 1) < 0) return -1;
    if (!ctx->capture_layout) return vav4l2_queue_capture(ctx->v4l2_fd, index, -1);
    struct v4l2_plane planes[3] = {{0}};
    struct v4l2_buffer b = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, .memory = V4L2_MEMORY_MMAP,
        .index = index, .length = ctx->capture_layout->planes, .m.planes = planes};
    return vav4l2_xioctl(ctx->v4l2_fd, VIDIOC_QBUF, &b);
}
int vav4l2_decoder_dequeue_capture(struct vav4l2_context *ctx, struct v4l2_buffer *result)
{
    struct v4l2_plane planes[3] = {{0}};
    unsigned memory = ctx->capture_memory == V4L2_MEMORY_DMABUF ? V4L2_MEMORY_DMABUF : V4L2_MEMORY_MMAP;
    struct v4l2_buffer b = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, .memory = memory,
        .length = ctx->capture_layout ? ctx->capture_layout->planes : 1, .m.planes = planes};
    if (vav4l2_xioctl(ctx->v4l2_fd, VIDIOC_DQBUF, &b) < 0) return -1;
    if (b.memory != memory || b.type != V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE || b.index >= (unsigned)ctx->capture_bufs_allocd || b.index >= VAV4L2_MAX_CAPTURE_BUFS ||
        b.length != (ctx->capture_layout ? ctx->capture_layout->planes : 1u) || b.timestamp.tv_sec < 0 || b.timestamp.tv_usec < 0 || b.timestamp.tv_usec >= 1000000 ||
        (uint64_t)b.timestamp.tv_sec > (UINT64_MAX / 1000 - (uint64_t)b.timestamp.tv_usec) / 1000000) { errno = EIO; return -1; }
    if (memory == V4L2_MEMORY_DMABUF && !ctx->capture_dma[b.index].active) { errno = EIO; return -1; }
    unsigned used = 0;
    for (unsigned i = 0; i < b.length; i++) {
        uint32_t limit = ctx->capture_layout ? ctx->capture_layout->size[i] : memory == V4L2_MEMORY_DMABUF ? ctx->capture_dma[b.index].length : ctx->cap_sizeimage;
        if (planes[i].data_offset || planes[i].bytesused > limit) { errno = EIO; return -1; }
        used |= planes[i].bytesused;
    }
    if (!(b.flags & V4L2_BUF_FLAG_ERROR) && (used || !(b.flags & V4L2_BUF_FLAG_LAST))) for (unsigned i = 0; i < b.length; i++) {
        uint32_t need = ctx->capture_layout ? ctx->capture_layout->stride[i] * ctx->capture_layout->rows[i] :
            memory == V4L2_MEMORY_DMABUF ? ctx->cap_sizeimage : vav4l2_capture_plane_size(ctx->cap_pixelformat, ctx->cap_stride, ctx->cap_height);
        if (!need || planes[i].bytesused < need) { errno = EIO; return -1; }
    }
    if (result) { *result = b; result->m.planes = NULL; result->bytesused = ctx->capture_layout && used ? ctx->cap_sizeimage : used; }
    return b.index;
}
int vav4l2_capture_export_plane(struct vav4l2_context *ctx, int index, unsigned plane)
{
    if (!ctx || index < 0 || index >= ctx->capture_bufs_allocd || index >= VAV4L2_MAX_CAPTURE_BUFS || plane >= (ctx->capture_layout ? ctx->capture_layout->planes : 1u)) { errno = EINVAL; return -1; }
    if (plane >= ctx->capture_exports[index].count) {
        unsigned start = ctx->capture_exports[index].count;
        for (unsigned p = start; p <= plane; p++) {
            struct v4l2_exportbuffer e = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, .index = index, .plane = p, .flags = O_CLOEXEC | O_RDWR};
            if (vav4l2_xioctl(ctx->v4l2_fd, VIDIOC_EXPBUF, &e) < 0) return -1;
            uint32_t need = ctx->capture_layout ? ctx->capture_layout->size[p] : ctx->cap_sizeimage, size;
            if (!need) need = vav4l2_capture_plane_size(ctx->cap_pixelformat, ctx->cap_stride, ctx->cap_height);
            if (vav4l2_export_fd_validate(e.fd, need, &size) < 0) { close(e.fd); errno = EPROTO; return -1; }
            ctx->capture_exports[index].fd[p] = e.fd; ctx->capture_exports[index].count++;
        }
    }
    return fcntl(ctx->capture_exports[index].fd[plane], F_DUPFD_CLOEXEC, 0);
}
void vav4l2_capture_exports_release(struct vav4l2_context *ctx)
{
    for (unsigned i = 0; i < VAV4L2_MAX_CAPTURE_BUFS; i++) {
        for (unsigned p = 0; p < ctx->capture_exports[i].count; p++) close(ctx->capture_exports[i].fd[p]);
        ctx->capture_exports[i].count = 0;
    }
}
int vav4l2_capture_layout_pull(struct vav4l2_context *ctx, struct vav4l2_surface *s, int index)
{
    struct vav4l2_capture_layout *layout = ctx->capture_layout;
    if (!layout || index < 0 || index >= ctx->capture_bufs_allocd) return -1;
    if (!ctx->capture_buf_ptr[index]) {
        struct v4l2_plane planes[3] = {{0}};
        struct v4l2_buffer b = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, .memory = V4L2_MEMORY_MMAP,
            .index = index, .length = ctx->capture_layout->planes, .m.planes = planes};
        if (vav4l2_xioctl(ctx->v4l2_fd, VIDIOC_QUERYBUF, &b) < 0 || b.length != layout->planes) return -1;
        for (unsigned i = 0; i < layout->planes; i++) {
            if (planes[i].length < layout->size[i]) return -1;
            void *map = layout->buffers[index][i].map;
            if (!map) {
                map = mmap(NULL, planes[i].length, PROT_READ, MAP_SHARED, ctx->v4l2_fd, planes[i].m.mem_offset);
                if (map == MAP_FAILED) return -1;
                layout->buffers[index][i].map = map; layout->buffers[index][i].length = planes[i].length;
            }
        }
        ctx->capture_buf_ptr[index] = calloc(1, ctx->cap_sizeimage);
        if (!ctx->capture_buf_ptr[index]) return -1;
        ctx->capture_buf_size = ctx->cap_sizeimage; ctx->capture_buf_anon = 1;
    }
    if (s->width > ctx->cap_width || s->height > ctx->cap_height) return -1;
    unsigned width = (s->width + 1) & ~1u;
    uint8_t *dst = ctx->capture_buf_ptr[index];
    for (unsigned y = 0; y < ctx->cap_height; y++) {
        if (!layout->tile_width) {
            memcpy(dst + (size_t)y * ctx->cap_stride, (const uint8_t *)layout->buffers[index][0].map + (size_t)y * layout->stride[0], width); continue;
        }
        for (unsigned x = 0; x < width; x++) {
            size_t at = (size_t)(y / layout->tile_height) * layout->stride[0] * layout->tile_height +
            (size_t)(x / layout->tile_width) * layout->tile_width * layout->tile_height +
            (y % layout->tile_height) * layout->tile_width + x % layout->tile_width;
            dst[(size_t)y * ctx->cap_stride + x] = ((const uint8_t *)layout->buffers[index][0].map)[at];
        }
    }
    dst += (size_t)ctx->cap_stride * ctx->cap_height;
    unsigned rows = layout->canonical == V4L2_PIX_FMT_NV16 ? ctx->cap_height : (ctx->cap_height + 1) / 2;
    int swapped = layout->fourcc == V4L2_PIX_FMT_NV21M || layout->fourcc == V4L2_PIX_FMT_NV61M ||
        layout->fourcc == V4L2_PIX_FMT_YVU420M || layout->fourcc == V4L2_PIX_FMT_YVU422M;
    for (unsigned y = 0; y < rows; y++) {
        if (!layout->tile_width && layout->planes == 2 && !swapped) {
            memcpy(dst + (size_t)y * ctx->cap_stride, (const uint8_t *)layout->buffers[index][1].map + (size_t)y * layout->stride[1], width); continue;
        }
        for (unsigned x = 0; x < width; x++) {
            unsigned plane = layout->planes == 3 ? 1 + ((x & 1) ^ swapped) : 1;
            unsigned column = layout->planes == 3 ? x / 2 : x ^ swapped;
            size_t at = (size_t)y * layout->stride[plane] + column;
            if (layout->tile_width) at = (size_t)(y / layout->tile_height) * layout->stride[plane] * layout->tile_height +
                (size_t)(column / layout->tile_width) * layout->tile_width * layout->tile_height +
                (y % layout->tile_height) * layout->tile_width + column % layout->tile_width;
            dst[(size_t)y * ctx->cap_stride + x] = ((const uint8_t *)layout->buffers[index][plane].map)[at];
        }
    }
    return vav4l2_surface_store_capture(ctx, s, index, ctx->capture_buf_ptr[index], ctx->cap_sizeimage, 1);
}
int vav4l2_capture_layout_exportable(const struct vav4l2_context *ctx)
{
    return ctx->capture_layout && ctx->capture_layout->fourcc == V4L2_PIX_FMT_NV12M;
}
VAStatus vav4l2_capture_layout_export(struct vav4l2_context *ctx, struct vav4l2_surface *s,
    unsigned flags, VADRMPRIMESurfaceDescriptor *out)
{
    if (!ctx->capture_layout || ctx->capture_layout->fourcc != V4L2_PIX_FMT_NV12M || s->buf_index < 0 || s->buf_index >= ctx->capture_bufs_allocd || s->format != VA_FOURCC_NV12)
        return VA_STATUS_ERROR_INVALID_SURFACE;
    VADRMPRIMESurfaceDescriptor d = {.fourcc = VA_FOURCC_NV12, .width = s->width, .height = s->height, .num_objects = 2};
    for (unsigned p = 0; p < 2; p++) d.objects[p].fd = -1;
    for (unsigned p = 0; p < 2; p++) {
        d.objects[p].fd = vav4l2_capture_export_plane(ctx, s->buf_index, p);
        if (d.objects[p].fd < 0 || vav4l2_export_fd_validate(d.objects[p].fd, ctx->capture_layout->size[p], &d.objects[p].size) < 0) goto fail;
        d.objects[p].drm_format_modifier = DRM_FORMAT_MOD_LINEAR;
    }
    if (flags & VA_EXPORT_SURFACE_COMPOSED_LAYERS) {
        d.num_layers = 1; d.layers[0].drm_format = DRM_FORMAT_NV12; d.layers[0].num_planes = 2;
        for (unsigned p = 0; p < 2; p++) { d.layers[0].object_index[p] = p; d.layers[0].pitch[p] = ctx->capture_layout->stride[p]; }
    } else {
        d.num_layers = 2;
        for (unsigned p = 0; p < 2; p++) {
            d.layers[p].drm_format = p ? DRM_FORMAT_GR88 : DRM_FORMAT_R8; d.layers[p].num_planes = 1;
            d.layers[p].object_index[0] = p; d.layers[p].pitch[0] = ctx->capture_layout->stride[p];
        }
    }
    *out = d; return VA_STATUS_SUCCESS;
fail:
    for (unsigned p = 0; p < 2; p++) if (d.objects[p].fd >= 0) close(d.objects[p].fd);
    return VA_STATUS_ERROR_OPERATION_FAILED;
}
