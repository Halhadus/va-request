/* Decoder-owned DMA-BUF pool for undersized native CAPTURE allocations.
 * Pixel snapshots and hardware DPB ownership remain separate lifetimes. */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <linux/dma-buf.h>
#include "va_request.h"

int vareq_capture_pool_allowed(const struct vareq_context *ctx)
{
    const char *mode = getenv("VAREQ_CAPTURE_POOL_DMABUF");
    return ctx && ctx->codec != VAREQ_CODEC_AV1 && (!mode || strcmp(mode, "0"));
}
int vareq_capture_pool_setup(struct vareq_context *ctx, unsigned width, unsigned height, uint32_t fourcc, unsigned count)
{
    struct v4l2_format fmt = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE};
    if (!vareq_capture_pool_allowed(ctx) || !count || count > VAREQ_MAX_CAPTURE_BUFS ||
        vareq_xioctl(ctx->v4l2_fd, VIDIOC_G_FMT, &fmt) < 0) return 0;
    struct v4l2_pix_format_mplane *f = &fmt.fmt.pix_mp;
    unsigned stride = f->plane_fmt[0].bytesperline, size = f->plane_fmt[0].sizeimage;
    if (f->width < width || f->height < height || f->pixelformat != fourcc || f->num_planes != 1 || f->field != V4L2_FIELD_NONE || !stride || !size ||
        (f->pixelformat != V4L2_PIX_FMT_NV12 && f->pixelformat != V4L2_PIX_FMT_NV15 &&
         f->pixelformat != V4L2_PIX_FMT_NV16 && f->pixelformat != V4L2_PIX_FMT_NV20) ||
        !vareq_capture_plane_size(f->pixelformat, stride, f->height) ||
        size < vareq_capture_plane_size(f->pixelformat, stride, f->height)) return 0;
    struct v4l2_requestbuffers req = {.type = fmt.type, .memory = V4L2_MEMORY_DMABUF, .count = count};
    if (vareq_xioctl(ctx->v4l2_fd, VIDIOC_REQBUFS, &req) < 0) return 0;
    if (req.count < count || req.count > VAREQ_MAX_CAPTURE_BUFS) goto fail;
    for (unsigned i = 0; i < req.count; i++) {
        unsigned rows = (unsigned)(((uint64_t)size + stride - 1) / stride), actual;
        int fd = vareq_alloc_shared_storage(stride, rows);
        if (fd < 0) goto fail;
        if (vareq_export_fd_validate(fd, size, &actual) < 0) { close(fd); goto fail; }
        struct stat object;
        if (fstat(fd, &object) < 0) { close(fd); goto fail; }
        for (unsigned j = 0; j < i; j++) {
            struct stat previous;
            if (fstat(ctx->capture_dma[j].fd, &previous) < 0 || (object.st_dev == previous.st_dev && object.st_ino == previous.st_ino)) { close(fd); goto fail; }
        }
        ctx->capture_dma[i].fd = fd; ctx->capture_dma[i].length = actual;
        ctx->capture_dma[i].owner = VA_INVALID_ID; ctx->capture_dma[i].active = 1;
        if (vareq_dmabuf_sync(fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE) < 0) goto fail;
        void *map = mmap(NULL, actual, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        int ok = map != MAP_FAILED;
        if (ok) { memset(map, 0, actual); ok = munmap(map, actual) == 0; }
        if (vareq_dmabuf_sync(fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE) < 0) ok = 0;
        if (!ok) goto fail;
    }
    ctx->capture_memory = V4L2_MEMORY_DMABUF; ctx->capture_internal = 1;
    ctx->cap_width = f->width; ctx->cap_height = f->height; ctx->cap_stride = stride;
    ctx->cap_sizeimage = ctx->capture_buf_size = size; ctx->cap_pixelformat = f->pixelformat;
    vareq_decode_trace(ctx, "capture-pool-ready", 0, req.count);
    return req.count;
fail:
    vareq_capture_dma_release(ctx);
    req.count = 0;
    if (vareq_xioctl(ctx->v4l2_fd, VIDIOC_REQBUFS, &req) < 0) return -1;
    return 0;
}

/* A client can reimport an exported pool object under a different VA ID.
 * Copying into it could overwrite another retained DPB;reject before QBUF. */
int vareq_capture_pool_bind(struct vareq_context *ctx, int index)
{
    struct vareq_surface *s = ctx->current_surface;
    if (!s || index < 0 || index >= ctx->capture_bufs_allocd || !ctx->capture_dma[index].active) return -1;
    if (s->prime || s->gbm_imported || s->gbm_writable) {
        VADRMPRIMESurfaceDescriptor d = {0};
        VAStatus status = s->prime ? vareq_prime_export_backing(s, VA_EXPORT_SURFACE_COMPOSED_LAYERS, &d, 1) :
            vareq_surface_fill_prime_gbm(s, VA_EXPORT_SURFACE_COMPOSED_LAYERS, &d);
        if (status != VA_STATUS_SUCCESS) return -1;
        int ok = 1;
        for (unsigned o = 0; o < d.num_objects; o++) {
            struct stat target;
            if (fstat(d.objects[o].fd, &target) < 0) ok = 0;
            for (struct vareq_context *c = ctx->driver_data->contexts; c && ok; c = c->next) {
                if (!c->capture_internal) continue;
                for (int i = 0; i < c->capture_bufs_allocd; i++) if (c->capture_dma[i].active) {
                    struct stat pool;
                    if (fstat(c->capture_dma[i].fd, &pool) < 0 || (target.st_dev == pool.st_dev && target.st_ino == pool.st_ino)) { ok = 0; break; }
                }
            }
            close(d.objects[o].fd);
        }
        if (!ok) { errno = EBUSY; return -1; }
    }
    if (vareq_dmabuf_wait(ctx->capture_dma[index].fd, 1) < 0) return -1;
    ctx->capture_dma[index].owner = s->surface_id;
    return 0;
}

int vareq_capture_pool_complete(struct vareq_context *ctx, struct vareq_surface *s, int index)
{
    if (index < 0 || index >= ctx->capture_bufs_allocd || !ctx->capture_dma[index].active || ctx->capture_dma[index].owner != s->surface_id) return -1;
    int fd = ctx->capture_dma[index].fd; unsigned size = ctx->cap_sizeimage;
    if (vareq_dmabuf_sync(fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ) < 0) return -1;
    void *map = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    int result = -1;
    if (map != MAP_FAILED) {
        result = vareq_surface_store_capture(ctx, s, index, map, size, 0);
        if (munmap(map, size) < 0) result = -1;
    }
    if (vareq_dmabuf_sync(fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ) < 0) result = -1;
    if (result < 0) vareq_prime_device_written(s, 0);
    vareq_decode_trace(ctx, "capture-pool-complete", result, index);
    return result;
}
