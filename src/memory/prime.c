/* Linear DRM PRIME objects. CPU staging keeps the V4L2 queue allocation and
 * reference lifetime independent of external object layout and ownership. */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <linux/dma-buf.h>
#include <drm_fourcc.h>
#include "va_request.h"

struct prime_plane { unsigned object, offset, pitch; };
struct vareq_prime_import {
    VADRMPRIMESurfaceDescriptor desc;
    struct vareq_prime_layout layout;
    struct prime_plane planes[3];
    void *map[4];
    int dirty;
};

/* Padding may contain another plane's rows. Reject intersecting pixel bytes,
 * rather than intersecting bounding ranges that also include row padding. */
static int plane_rows_overlap(const struct prime_plane *a, unsigned aw, unsigned ah,
                               const struct prime_plane *b, unsigned bw, unsigned bh)
{
    unsigned y = 0, z = 0;
    while (y < ah && z < bh) {
        uint64_t start_a = a->offset + (uint64_t)y * a->pitch, start_b = b->offset + (uint64_t)z * b->pitch;
        if (start_a < start_b + bw && start_b < start_a + aw) return 1;
        if (start_a + aw <= start_b) y++; else z++;
    }
    return 0;
}

int vareq_prime_layout(uint32_t f, unsigned w, unsigned h, struct vareq_prime_layout *l)
{
    if (!w || !h || w > UINT16_MAX || h > UINT16_MAX) return -1;
    memset(l, 0, sizeof(*l)); l->planes = 1; l->row[0] = w; l->rows[0] = h;
    switch (f) {
    case VA_FOURCC_NV12: l->drm_format = DRM_FORMAT_NV12; l->planes = 2; break;
    case VA_FOURCC_NV21: l->drm_format = DRM_FORMAT_NV21; l->planes = 2; break;
    case VA_FOURCC_P010: l->drm_format = DRM_FORMAT_P010; l->planes = 2; l->row[0] *= 2; break;
    case VA_FOURCC_I420: l->drm_format = DRM_FORMAT_YUV420; l->planes = 3; break;
    case VA_FOURCC_YV12: l->drm_format = DRM_FORMAT_YVU420; l->planes = 3; break;
    case VA_FOURCC_422H: l->drm_format = DRM_FORMAT_YUV422; l->planes = 3; break;
    case VA_FOURCC_YV16: l->drm_format = DRM_FORMAT_YVU422; l->planes = 3; break;
    case VA_FOURCC_YUY2: l->drm_format = DRM_FORMAT_YUYV; l->row[0] *= 2; break;
    case VA_FOURCC_UYVY: l->drm_format = DRM_FORMAT_UYVY; l->row[0] *= 2; break;
    case VA_FOURCC_VYUY: l->drm_format = DRM_FORMAT_VYUY; l->row[0] *= 2; break;
    case VA_FOURCC_YVYU: l->drm_format = DRM_FORMAT_YVYU; l->row[0] *= 2; break;
    case VA_FOURCC_Y210: l->drm_format = DRM_FORMAT_Y210; l->row[0] *= 4; break;
    case VA_FOURCC_RGB565: l->drm_format = DRM_FORMAT_RGB565; l->row[0] *= 2; return 0;
    case VA_FOURCC_RGBA: l->drm_format = DRM_FORMAT_ABGR8888; l->row[0] *= 4; return 0;
    case VA_FOURCC_RGBX: l->drm_format = DRM_FORMAT_XBGR8888; l->row[0] *= 4; return 0;
    case VA_FOURCC_ARGB: l->drm_format = DRM_FORMAT_BGRA8888; l->row[0] *= 4; return 0;
    case VA_FOURCC_BGRA: l->drm_format = DRM_FORMAT_ARGB8888; l->row[0] *= 4; return 0;
    case VA_FOURCC_BGRX: l->drm_format = DRM_FORMAT_XRGB8888; l->row[0] *= 4; return 0;
    default: return -1;
    }
    if ((w & 1) || ((h & 1) && (f == VA_FOURCC_NV12 || f == VA_FOURCC_NV21 || f == VA_FOURCC_P010 ||
        f == VA_FOURCC_I420 || f == VA_FOURCC_YV12))) return -1;
    l->plane_format[0] = f == VA_FOURCC_P010 ? DRM_FORMAT_R16 : DRM_FORMAT_R8;
    if (l->planes > 1) {
        l->row[1] = l->planes == 3 ? w / 2 : l->row[0];
        l->rows[1] = f == VA_FOURCC_422H || f == VA_FOURCC_YV16 ? h : h / 2;
        l->plane_format[1] = l->planes == 3 ? DRM_FORMAT_R8 : f == VA_FOURCC_P010 ? DRM_FORMAT_GR1616 : DRM_FORMAT_GR88;
        l->row[2] = l->row[1]; l->rows[2] = l->rows[1]; l->plane_format[2] = DRM_FORMAT_R8;
    }
    return 0;
}

/* Device DMA waits for reservation fences; CPU access additionally needs SYNC.
 * Neither operation serializes a client that submits new work concurrently. */
int vareq_dmabuf_wait(int fd, int write)
{
    short events = write ? POLLOUT : POLLIN;
    struct pollfd pfd = {.fd = fd, .events = events};
    int ret = vareq_poll_intr(&pfd, 1, 2000);
    if (ret < 0) return -1;
    if (!ret) { errno = ETIMEDOUT; return -1; }
    if (!(pfd.revents & events) || (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
        errno = pfd.revents & POLLNVAL ? EBADF : EIO; return -1;
    }
    return 0;
}

int vareq_dmabuf_sync(int fd, uint64_t flags)
{
    if (!(flags & DMA_BUF_SYNC_END) && vareq_dmabuf_wait(fd, flags & DMA_BUF_SYNC_WRITE) < 0) return -1;
    struct dma_buf_sync sync = {.flags = flags};
    struct timespec start, now, pause = {.tv_nsec = 1000000};
    if (clock_gettime(CLOCK_MONOTONIC, &start) < 0) return -1;
    for (;;) {
        int ret = vareq_ioctl_once(fd, DMA_BUF_IOCTL_SYNC, &sync);
        if (ret >= 0 || (errno != EAGAIN && errno != EINTR)) return ret;
        if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return -1;
        int64_t elapsed = (int64_t)(now.tv_sec - start.tv_sec) * 1000000000 + now.tv_nsec - start.tv_nsec;
        if (elapsed >= 2000000000) { errno = ETIMEDOUT; return -1; }
        (void)nanosleep(&pause, NULL);
    }
}

void vareq_prime_device_written(struct vareq_surface *s, int success)
{
    if (s->prime) s->prime->dirty = !success;
    if (s->gbm_bo) s->gbm_dirty = !success;
    s->prime_capture_valid = 0;
    s->last_writer = VAREQ_WRITER_BO;
    s->has_pic = success;
}

void vareq_prime_destroy(struct vareq_surface *s)
{
    if (!s || !s->prime) return;
    struct vareq_prime_import *p = s->prime;
    for (unsigned i = 0; i < p->desc.num_objects; i++) {
        if (p->map[i]) munmap(p->map[i], p->desc.objects[i].size);
        if (p->desc.objects[i].fd >= 0) close(p->desc.objects[i].fd);
    }
    free(p); s->prime = NULL;
}

VAStatus vareq_prime_import(struct vareq_surface *s, const VADRMPRIMESurfaceDescriptor *d)
{
    struct vareq_prime_layout l;
    if (d->fourcc != s->format || vareq_prime_layout(d->fourcc, s->width, s->height, &l) < 0)
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    if (d->width != s->width || d->height != s->height || !d->num_objects || d->num_objects > l.planes ||
        (d->num_layers != 1 && d->num_layers != l.planes)) return VA_STATUS_ERROR_INVALID_PARAMETER;
    int composed = d->num_layers == 1;
    if (composed && (d->layers[0].drm_format != l.drm_format || d->layers[0].num_planes != l.planes))
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    struct prime_plane planes[3] = {0}; unsigned used = 0;
    for (unsigned i = 0; i < l.planes; i++) {
        unsigned layer = composed ? 0 : i, plane = composed ? i : 0;
        if (!composed && (d->layers[layer].num_planes != 1 || d->layers[layer].drm_format != l.plane_format[i]))
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        planes[i] = (struct prime_plane){d->layers[layer].object_index[plane], d->layers[layer].offset[plane], d->layers[layer].pitch[plane]};
        unsigned o = planes[i].object;
        uint64_t end = planes[i].offset + (uint64_t)planes[i].pitch * (l.rows[i] - 1) + l.row[i];
        if (o >= d->num_objects || planes[i].pitch < l.row[i] || end > d->objects[o].size)
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        used |= 1u << o;
    }
    if (used != (1u << d->num_objects) - 1) return VA_STATUS_ERROR_INVALID_PARAMETER;
    struct stat statbuf[4];
    for (unsigned i = 0; i < d->num_objects; i++) {
        if (d->objects[i].drm_format_modifier != DRM_FORMAT_MOD_LINEAR) return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
        if (d->objects[i].fd < 0 || fstat(d->objects[i].fd, &statbuf[i]) < 0) return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    for (unsigned i = 0; i < l.planes; i++) for (unsigned j = 0; j < i; j++) {
        unsigned a = planes[i].object, b = planes[j].object;
        if (statbuf[a].st_dev != statbuf[b].st_dev || statbuf[a].st_ino != statbuf[b].st_ino) continue;
        uint64_t ae = planes[i].offset + (uint64_t)planes[i].pitch * (l.rows[i] - 1) + l.row[i];
        uint64_t be = planes[j].offset + (uint64_t)planes[j].pitch * (l.rows[j] - 1) + l.row[j];
        if (planes[i].offset < be && planes[j].offset < ae &&
            plane_rows_overlap(&planes[i], l.row[i], l.rows[i], &planes[j], l.row[j], l.rows[j]))
            return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    struct vareq_prime_import *p = calloc(1, sizeof(*p));
    if (!p) return VA_STATUS_ERROR_ALLOCATION_FAILED;
    p->desc = *d; p->layout = l; memcpy(p->planes, planes, sizeof(planes));
    for (unsigned i = 0; i < d->num_objects; i++) p->desc.objects[i].fd = -1;
    s->prime = p;
    VAStatus status = VA_STATUS_ERROR_INVALID_PARAMETER;
    for (unsigned i = 0; i < d->num_objects; i++) {
        int fd = fcntl(d->objects[i].fd, F_DUPFD_CLOEXEC, 0); uint32_t actual;
        if (fd < 0) { status = errno == EBADF ? VA_STATUS_ERROR_INVALID_PARAMETER : VA_STATUS_ERROR_ALLOCATION_FAILED; goto fail; }
        p->desc.objects[i].fd = fd;
        if (vareq_export_fd_validate(fd, d->objects[i].size, &actual) < 0) goto fail;
        /* Reject ordinary files and non-mappable exporters before publishing IDs. */
        if (vareq_dmabuf_sync(fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ) < 0) goto fail;
        if (vareq_dmabuf_sync(fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ) < 0) goto fail;
        p->desc.objects[i].size = actual;
        void *map = mmap(NULL, actual, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (map == MAP_FAILED) { status = VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE; goto fail; }
        munmap(map, actual);
    }
    s->has_pic = 1; s->last_writer = VAREQ_WRITER_CPU;
    return VA_STATUS_SUCCESS;
fail:
    vareq_prime_destroy(s); return status;
}

static int prime_copy(struct vareq_surface *s, int write)
{
    struct vareq_prime_import *p = s->prime;
    if (!p || (!write && p->dirty) || vareq_surface_ensure_cpu(s) < 0) return -1;
    if (write) p->dirty = 1;
    unsigned started = 0; int ret = -1;
    uint64_t access = write ? DMA_BUF_SYNC_WRITE : DMA_BUF_SYNC_READ;
    for (; started < p->desc.num_objects; started++)
        if (vareq_dmabuf_sync(p->desc.objects[started].fd, DMA_BUF_SYNC_START | access) < 0) goto end;
    /* Map only after the device fence and CPU-access transition. Do not
     * retain an earlier CPU mapping across external DMA ownership. */
    for (unsigned i = 0; i < p->desc.num_objects; i++) {
        void *map = mmap(NULL, p->desc.objects[i].size, PROT_READ | (write ? PROT_WRITE : 0), MAP_SHARED, p->desc.objects[i].fd, 0);
        if (map == MAP_FAILED) goto end;
        p->map[i] = map;
    }
    unsigned offset = 0;
    for (unsigned i = 0; i < p->layout.planes; i++) {
        unsigned pitch = i && p->layout.planes == 3 ? (s->cpu_stride + 1) / 2 : s->cpu_stride;
        const struct prime_plane *plane = &p->planes[i];
        for (unsigned y = 0; y < p->layout.rows[i]; y++) {
            void *cpu = (char *)s->cpu_ptr + offset + (size_t)y * pitch;
            void *external = (char *)p->map[plane->object] + plane->offset + (size_t)y * plane->pitch;
            if (write) memcpy(external, cpu, p->layout.row[i]); else memcpy(cpu, external, p->layout.row[i]);
        }
        offset += pitch * p->layout.rows[i];
    }
    ret = 0;
end:
    for (unsigned i = 0; i < p->desc.num_objects; i++) if (p->map[i]) {
        munmap(p->map[i], p->desc.objects[i].size); p->map[i] = NULL;
    }
    while (started) if (vareq_dmabuf_sync(p->desc.objects[--started].fd, DMA_BUF_SYNC_END | access) < 0) ret = -1;
    if (!ret) { if (write) p->dirty = 0; else s->last_writer = VAREQ_WRITER_CPU; }
    return ret;
}

int vareq_prime_read(struct vareq_surface *s) { return prime_copy(s, 0); }
int vareq_prime_write(struct vareq_surface *s) { s->prime_capture_valid = 0; return prime_copy(s, 1); }

VAStatus vareq_prime_export_backing(struct vareq_surface *s, uint32_t flags, void *descriptor, int overwrite)
{
    struct vareq_prime_import *p = s->prime;
    if (!p || (p->dirty && !overwrite)) return VA_STATUS_ERROR_OPERATION_FAILED;
    VADRMPRIMESurfaceDescriptor d = p->desc;
    memset(d.layers, 0, sizeof(d.layers));
    int separate = (flags & VA_EXPORT_SURFACE_SEPARATE_LAYERS) && p->layout.planes > 1;
    d.num_layers = separate ? p->layout.planes : 1;
    for (unsigned i = 0; i < p->layout.planes; i++) {
        unsigned layer = separate ? i : 0, plane = separate ? 0 : i;
        d.layers[layer].drm_format = separate ? p->layout.plane_format[i] : p->layout.drm_format;
        d.layers[layer].num_planes = separate ? 1 : p->layout.planes;
        d.layers[layer].object_index[plane] = p->planes[i].object;
        d.layers[layer].offset[plane] = p->planes[i].offset; d.layers[layer].pitch[plane] = p->planes[i].pitch;
    }
    for (unsigned i = 0; i < d.num_objects; i++) {
        d.objects[i].fd = fcntl(p->desc.objects[i].fd, F_DUPFD_CLOEXEC, 0);
        if (d.objects[i].fd < 0) {
            while (i) close(d.objects[--i].fd);
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
    }
    *(VADRMPRIMESurfaceDescriptor *)descriptor = d;
    return VA_STATUS_SUCCESS;
}

VAStatus vareq_prime_export(struct vareq_surface *s, uint32_t flags, void *descriptor)
{
    return vareq_prime_export_backing(s, flags, descriptor, 0);
}

int vareq_prime_capture(struct vareq_surface *s, const void *src, uint32_t stride, uint32_t alh, uint32_t fcc, uint32_t size)
{
    if (vareq_surface_ensure_cpu(s) < 0) return -1;
    struct vareq_image_view to = {.data = s->cpu_ptr, .fourcc = s->format, .stride = s->cpu_stride,
        .width = s->width, .height = s->height, .size = s->cpu_size};
    struct vareq_image_view from = {.data = (void *)src, .fourcc = fcc, .stride = stride,
        .width = s->width, .height = s->height, .alloc_height = alh, .size = size};
    int ret = -1;
    if (fcc == V4L2_PIX_FMT_NV12 || fcc == V4L2_PIX_FMT_NV16) {
        if (fcc == V4L2_PIX_FMT_NV12) from.fourcc = VA_FOURCC_NV12;
        ret = vareq_copy_image_rect(&to, 0, 0, &from, 0, 0, s->width, s->height) == VA_STATUS_SUCCESS ? 0 : -1;
        if (ret && fcc == V4L2_PIX_FMT_NV16 && s->format == VA_FOURCC_YUY2) {
            vareq_nv16_to_yuy2(s->cpu_ptr, s->cpu_stride, src, stride, alh, s->width, s->height); ret = 0;
        }
    } else if (fcc == V4L2_PIX_FMT_NV15 && s->format == VA_FOURCC_P010) {
        vareq_nv15_to_p010(s->cpu_ptr, s->cpu_stride, s->height, src, stride, alh, s->width, s->height); ret = 0;
    } else if (fcc == V4L2_PIX_FMT_NV20) {
        if (s->format == VA_FOURCC_Y210) ret = vareq_nv20_to_y210(s->cpu_ptr, s->cpu_stride, src, stride, alh, s->width, s->height);
        else if (s->format == VA_FOURCC_YUY2) ret = vareq_nv20_to_yuy2(s->cpu_ptr, s->cpu_stride, src, stride, alh, s->width, s->height);
    }
    if (ret < 0) return -1;
    if (s->prime) ret = vareq_prime_write(s);
    else ret = vareq_gbm_surface_upload(s, s->cpu_ptr, s->cpu_stride, s->height);
    s->prime_capture_valid = ret == 0;
    return ret;
}
