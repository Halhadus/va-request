/* Client-owned decoder CAPTURE storage. Native MMAP remains fallback. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <drm_fourcc.h>
#include "va_request.h"

static void capture_trace(const struct vareq_context *ctx, const char *stage, int index, int error)
{
    const char *path = getenv("VAREQ_CAPTURE_TRACE");
    if (!path || !*path) return;
    FILE *file = fopen(path, "a");
    if (!file) { perror("capture DMA trace"); return; }
    if (!ftell(file)) fprintf(file, "STAGE\tCODEC\tFORMAT\tWIDTH\tHEIGHT\tSTRIDE\tSIZEIMAGE\tINDEX\tSURFACE\tMEMORY\tERRNO\n");
    fprintf(file, "%s\t%u\t%08x\t%u\t%u\t%u\t%u\t%d\t%u\t%u\t%d\n", stage, ctx->codec,
        ctx->cap_pixelformat, ctx->cap_width, ctx->cap_height, ctx->cap_stride, ctx->cap_sizeimage, index,
        ctx->current_surface ? ctx->current_surface->surface_id : VA_INVALID_ID, ctx->capture_memory, error);
    if (fclose(file)) perror("close capture DMA trace");
}

static int capture_descriptor(struct vareq_context *ctx, struct vareq_surface *s,
                               const struct v4l2_pix_format_mplane *f, int *fd, unsigned *length)
{
    if (!s || s->format != VA_FOURCC_NV12 || (!s->prime && !s->gbm_imported && !s->gbm_writable)) return 0;
    VADRMPRIMESurfaceDescriptor d = {0};
    VAStatus status = s->prime ? vareq_prime_export_backing(s, VA_EXPORT_SURFACE_COMPOSED_LAYERS, &d, 1) :
        vareq_surface_fill_prime_gbm(s, VA_EXPORT_SURFACE_COMPOSED_LAYERS, &d);
    if (status != VA_STATUS_SUCCESS) return -1;
    int valid = d.num_objects == 1 && d.num_layers == 1 && d.layers[0].num_planes == 2 &&
        d.layers[0].drm_format == DRM_FORMAT_NV12 && d.objects[0].drm_format_modifier == DRM_FORMAT_MOD_LINEAR &&
        !d.layers[0].object_index[0] && !d.layers[0].object_index[1] && !d.layers[0].offset[0] &&
        d.layers[0].offset[1] == (uint64_t)f->plane_fmt[0].bytesperline * f->height &&
        d.layers[0].pitch[0] == f->plane_fmt[0].bytesperline && d.layers[0].pitch[1] == f->plane_fmt[0].bytesperline &&
        d.width <= f->width && d.height <= f->height && d.objects[0].size >= f->plane_fmt[0].sizeimage;
    if (valid) { *fd = d.objects[0].fd; *length = d.objects[0].size; }
    else for (unsigned i = 0; i < d.num_objects; i++) close(d.objects[i].fd);
    (void)ctx;
    return valid;
}

int vareq_capture_dma_setup(struct vareq_context *ctx, unsigned width, unsigned height, uint32_t fourcc, unsigned count)
{
    const char *mode = getenv("VAREQ_CAPTURE_DMABUF");
    if (mode && !strcmp(mode, "0")) return 0;
    int required = mode && !strcmp(mode, "required");
    /* Without a declared pool, later targets may have different layouts.
     * Keep native DPB storage in auto mode; forcing direct storage remains
     * available for clients that guarantee a homogeneous dynamic pool. */
    if ((!mode || (strcmp(mode, "1") && !required)) && !ctx->num_render_targets) return 0;
    if (fourcc != V4L2_PIX_FMT_NV12 || ctx->codec == VAREQ_CODEC_AV1) return required ? -1 : 0;
    if (!ctx->driver_data || (!ctx->num_render_targets && !ctx->current_surface)) {
        capture_trace(ctx, "no-target-fallback", -1, EINVAL); return required ? -1 : 0;
    }
    struct v4l2_format fmt = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE};
    fmt.fmt.pix_mp.width = width; fmt.fmt.pix_mp.height = height; fmt.fmt.pix_mp.pixelformat = fourcc;
    fmt.fmt.pix_mp.num_planes = 1; fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    if (vareq_xioctl(ctx->v4l2_fd, VIDIOC_S_FMT, &fmt) < 0) return required ? -1 : 0;
    struct v4l2_pix_format_mplane *f = &fmt.fmt.pix_mp;
    if (f->width < width || f->height < height || f->pixelformat != fourcc || f->num_planes != 1 ||
        f->field != V4L2_FIELD_NONE || f->plane_fmt[0].bytesperline < f->width ||
        f->plane_fmt[0].sizeimage < (uint64_t)f->plane_fmt[0].bytesperline * (f->height + (uint64_t)f->height / 2)) return required ? -1 : 0;
    int targets = ctx->num_render_targets ? ctx->num_render_targets : 1;
    for (int i = 0; i < targets; i++) {
        int fd = -1; unsigned length = 0;
        struct vareq_surface *target = ctx->num_render_targets ? vareq_surface_by_id(ctx->driver_data, ctx->render_targets[i]) : ctx->current_surface;
        int ret = capture_descriptor(ctx, target, f, &fd, &length);
        if (ret <= 0) { capture_trace(ctx, "pool-layout-fallback", -1, ret < 0 ? errno : EINVAL); return required ? -1 : 0; }
        close(fd);
    }
    struct v4l2_requestbuffers req = {.type = fmt.type, .memory = V4L2_MEMORY_DMABUF, .count = count};
    if (vareq_xioctl(ctx->v4l2_fd, VIDIOC_REQBUFS, &req) < 0) {
        capture_trace(ctx, "REQBUFS-fallback", -1, errno);
        int error = errno;
        if (error == EINVAL || error == ENOTTY || error == EOPNOTSUPP) return required ? -1 : 0;
        req.count = 0;
        if (vareq_xioctl(ctx->v4l2_fd, VIDIOC_REQBUFS, &req) < 0) return -1;
        errno = error;
        return required ? -1 : 0;
    }
    if (!req.count) return required ? -1 : 0;
    ctx->capture_memory = V4L2_MEMORY_DMABUF;
    ctx->cap_width = f->width; ctx->cap_height = f->height; ctx->cap_stride = f->plane_fmt[0].bytesperline;
    ctx->cap_sizeimage = ctx->capture_buf_size = f->plane_fmt[0].sizeimage; ctx->cap_pixelformat = fourcc;
    capture_trace(ctx, "negotiated", -1, 0);
    return req.count < VAREQ_MAX_CAPTURE_BUFS ? req.count : VAREQ_MAX_CAPTURE_BUFS;
}

int vareq_capture_dma_bind(struct vareq_context *ctx, int index)
{
    if (!ctx || !ctx->driver_data || index < 0 || index >= ctx->capture_bufs_allocd || index >= VAREQ_MAX_CAPTURE_BUFS) return -1;
    if (ctx->capture_internal) return vareq_capture_pool_bind(ctx, index);
    struct v4l2_pix_format_mplane f = {.width = ctx->cap_width, .height = ctx->cap_height};
    f.plane_fmt[0].bytesperline = ctx->cap_stride; f.plane_fmt[0].sizeimage = ctx->cap_sizeimage;
    int fd = -1; unsigned length = 0;
    if (capture_descriptor(ctx, ctx->current_surface, &f, &fd, &length) <= 0) {
        capture_trace(ctx, "target-layout-rejected", index, EINVAL); return -1;
    }
    struct stat current;
    if (fstat(fd, &current) < 0) { close(fd); return -1; }
    for (struct vareq_context *c = ctx->driver_data->contexts; c; c = c->next) {
        if (c->capture_memory != V4L2_MEMORY_DMABUF) continue;
        for (int i = 0; i < c->capture_bufs_allocd; i++) {
            if (!c->capture_dma[i].active || (c == ctx && i == index)) continue;
            struct vareq_surface *owner = vareq_surface_by_id(ctx->driver_data, c->capture_dma[i].owner);
            if (!owner || !vareq_surface_capture_matches(c, owner) || owner->buf_index != i) continue;
            struct stat other;
            if (fstat(c->capture_dma[i].fd, &other) < 0 || (current.st_dev == other.st_dev && current.st_ino == other.st_ino)) {
                close(fd); capture_trace(ctx, "live-DPB-alias-rejected", index, EBUSY); return -1;
            }
        }
    }
    if (vareq_dmabuf_wait(fd, 1) < 0) { close(fd); return -1; }
    if (ctx->capture_dma[index].active) close(ctx->capture_dma[index].fd);
    ctx->capture_dma[index].fd = fd; ctx->capture_dma[index].length = length;
    ctx->capture_dma[index].owner = ctx->current_surface->surface_id; ctx->capture_dma[index].active = 1;
    return 0;
}

int vareq_capture_dma_queue(struct vareq_context *ctx, int index)
{
    if (!ctx || index < 0 || index >= ctx->capture_bufs_allocd || index >= VAREQ_MAX_CAPTURE_BUFS || !ctx->capture_dma[index].active) return -1;
    struct v4l2_plane plane = {.m.fd = ctx->capture_dma[index].fd, .length = ctx->capture_dma[index].length};
    struct v4l2_buffer buffer = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, .memory = V4L2_MEMORY_DMABUF,
        .index = index, .length = 1, .m.planes = &plane};
    /* Only OUTPUT belongs to the request. CAPTURE is always queued bare. */
    int ret = vareq_xioctl(ctx->v4l2_fd, VIDIOC_QBUF, &buffer);
    capture_trace(ctx, ret < 0 ? "QBUF-failed" : "queued", index, ret < 0 ? errno : 0);
    return ret;
}

int vareq_capture_dma_dequeue(struct vareq_context *ctx, int *error)
{
    struct v4l2_plane plane = {0};
    struct v4l2_buffer buffer = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, .memory = V4L2_MEMORY_DMABUF,
        .length = 1, .m.planes = &plane};
    *error = 0;
    if (vareq_xioctl(ctx->v4l2_fd, VIDIOC_DQBUF, &buffer) < 0) return -1;
    if (buffer.memory != V4L2_MEMORY_DMABUF || buffer.length != 1 || buffer.index >= (unsigned)ctx->capture_bufs_allocd ||
        !ctx->capture_dma[buffer.index].active) return -1;
    *error = !!(buffer.flags & V4L2_BUF_FLAG_ERROR);
    if (!*error && (plane.data_offset || plane.bytesused < ctx->cap_sizeimage || plane.bytesused > ctx->capture_dma[buffer.index].length)) return -1;
    capture_trace(ctx, *error ? "decode-error" : "dequeued", buffer.index, 0);
    return buffer.index;
}

int vareq_capture_dma_complete(struct vareq_context *ctx, struct vareq_surface *s, int index)
{
    if (index < 0 || index >= ctx->capture_bufs_allocd || !ctx->capture_dma[index].active || ctx->capture_dma[index].owner != s->surface_id) return -1;
    if (ctx->capture_internal) return vareq_capture_pool_complete(ctx, s, index);
    vareq_prime_device_written(s, 1);
    s->buf_index = index; s->capture_ctx = ctx; s->cap_view = NULL;
    s->stride = ctx->cap_stride; s->aligned_h = ctx->cap_height; s->cap_fourcc = ctx->cap_pixelformat;
    s->prime_capture_valid = 1;
    capture_trace(ctx, "complete", index, 0);
    return 0;
}

void vareq_capture_dma_release(struct vareq_context *ctx)
{
    for (int i = 0; i < VAREQ_MAX_CAPTURE_BUFS; i++) if (ctx->capture_dma[i].active) {
        close(ctx->capture_dma[i].fd); ctx->capture_dma[i].active = 0;
    }
    ctx->capture_internal = 0;
}
