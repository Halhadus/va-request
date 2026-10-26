/* Linear GBM backing for exported and imported VA surfaces. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <linux/dma-buf.h>
#include <gbm.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <va/va.h>
#include <va/va_drmcommon.h>
#include "va_v4l2.h"

static struct vav4l2_gbm_state fallback_gbm = {.fd = -1, .drm_fd = -1};

/* Preserve public Y/Cr/Cb semantics through descriptor offsets. Native
 * storage uses the working kernel Y/Cb/Cr transport without per-job repacks. */
static uint32_t native_storage_format(uint32_t fourcc)
{
    return fourcc == VA_FOURCC_YV12 ? VA_FOURCC_I420 : fourcc == VA_FOURCC_YV16 ? VA_FOURCC_422H : fourcc;
}

static void cleanup_gbm(struct vav4l2_gbm_state *state)
{
    if (state->device) gbm_device_destroy(state->device);
    if (state->fd >= 0) close(state->fd);
    state->device = NULL; state->fd = -1; state->failed = 0;
}

void vav4l2_gbm_driver_cleanup(struct vav4l2_driver_data *dd)
{
    cleanup_gbm(&dd->gbm);
}

void vav4l2_gbm_cleanup(void)
{
    cleanup_gbm(&fallback_gbm);
    vav4l2_allocator_cleanup();
}

static int open_gbm(struct vav4l2_gbm_state *state, int fd)
{
    if (fd < 0) return 0;
    state->device = gbm_create_device(fd);
    if (!state->device) { close(fd); return 0; }
    state->fd = fd;
    return 1;
}

static struct gbm_device *vav4l2_gbm_device(const struct vav4l2_surface *s)
{
    struct vav4l2_gbm_state *state = s->driver_data ? &s->driver_data->gbm : &fallback_gbm;
    if (state->device) return state->device;
    if (state->failed) return NULL;
    const char *node = getenv("VAV4L2_RENDER_NODE");
    if (node && *node) {
        if (open_gbm(state, open(node, O_RDWR | O_CLOEXEC))) return state->device;
    } else if (state->drm_fd >= 0) {
        if (open_gbm(state, fcntl(state->drm_fd, F_DUPFD_CLOEXEC, 0))) return state->device;
    } else {
        drmDevicePtr devices[64];
        int count = drmGetDevices2(0, devices, 64);
        for (int i = 0; i < count; i++) {
            if (!state->device && (devices[i]->available_nodes & (1 << DRM_NODE_RENDER)))
                open_gbm(state, open(devices[i]->nodes[DRM_NODE_RENDER], O_RDWR | O_CLOEXEC));
        }
        if (count > 0) drmFreeDevices(devices, count);
        if (state->device) return state->device;
    }
    state->failed = 1;
    return NULL;
}

static int import_cpu_access(struct vav4l2_surface *s, uint64_t flags)
{
    int fd = s->gbm_imported ? s->import_fd : s->gbm_writable ? s->gbm_external_fd : -1;
    return fd < 0 ? 0 : vav4l2_dmabuf_sync(fd, flags);
}

struct cpu_bo_map { void *cookie; size_t length; int direct; };

/* Shared linear objects must be mapped through the exported allocation. A
 * GBM map may be a driver transfer/shadow rather than that allocation. */
static void *map_cpu_bo(struct vav4l2_surface *s, unsigned width, unsigned rows,
                        unsigned access, uint32_t *pitch, struct cpu_bo_map *map)
{
    int fd = s->gbm_imported ? s->import_fd : s->gbm_writable ? s->gbm_external_fd : -1;
    if (fd < 0) return gbm_bo_map(s->gbm_bo, 0, 0, width, rows, access, pitch, &map->cookie);
    uint64_t size = (uint64_t)s->gbm_stride * rows;
    if (!rows || width > s->gbm_stride || size > SIZE_MAX) return NULL;
    void *ptr = mmap(NULL, size, PROT_READ | (access & GBM_BO_TRANSFER_WRITE ? PROT_WRITE : 0), MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) return NULL;
    map->cookie = ptr; map->length = size; map->direct = 1; *pitch = s->gbm_stride;
    return ptr;
}

static void unmap_cpu_bo(struct vav4l2_surface *s, struct cpu_bo_map *map)
{
    if (map->direct) munmap(map->cookie, map->length);
    else gbm_bo_unmap(s->gbm_bo, map->cookie);
}

int vav4l2_import_refresh(struct vav4l2_surface *s)
{
    if (s->prime) return vav4l2_prime_read(s);
    if (s->gbm_bo && s->format != VA_FOURCC_NV12 && (s->gbm_writable || s->last_writer == VAV4L2_WRITER_BO)) return vav4l2_gbm_read_cpu(s);
    if (s->gbm_imported || s->gbm_writable) {
        if (s->gbm_dirty || !s->gbm_bo) return -1;
        s->last_writer = VAV4L2_WRITER_BO;
    }
    return 0;
}

VAStatus vav4l2_gbm_surface_import(struct vav4l2_surface *s, const VADRMPRIMESurfaceDescriptor *desc)
{
    if (desc->fourcc != VA_FOURCC_NV12) return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    if (desc->num_objects != 1 || desc->num_layers != 1 || desc->layers[0].num_planes != 2 ||
        desc->layers[0].drm_format != DRM_FORMAT_NV12 || desc->objects[0].drm_format_modifier != DRM_FORMAT_MOD_LINEAR)
        return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
    uint32_t pitch = desc->layers[0].pitch[0];
    uint64_t required = (uint64_t)pitch * (s->height + (uint64_t)s->height / 2);
    if (!s->width || !s->height || (s->width & 1) || (s->height & 1) || desc->width != s->width || desc->height != s->height ||
        desc->objects[0].fd < 0 || pitch < s->width || pitch > INT_MAX || desc->layers[0].pitch[1] != pitch ||
        desc->layers[0].object_index[0] || desc->layers[0].object_index[1] || desc->layers[0].offset[0] ||
        desc->layers[0].offset[1] != (uint64_t)pitch * s->height || required > desc->objects[0].size)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    int fd = fcntl(desc->objects[0].fd, F_DUPFD_CLOEXEC, 0);
    if (fd < 0) return errno == EBADF ? VA_STATUS_ERROR_INVALID_PARAMETER : VA_STATUS_ERROR_ALLOCATION_FAILED;
    uint32_t actual;
    if (vav4l2_export_fd_validate(fd, required, &actual) < 0 || desc->objects[0].size > actual) {
        close(fd); return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    struct gbm_device *dev = vav4l2_gbm_device(s);
    struct gbm_import_fd_modifier_data input = {.width = s->width, .height = s->height + s->height / 2,
        .format = GBM_FORMAT_R8, .num_fds = 1, .fds = {fd}, .strides = {(int)pitch}, .modifier = DRM_FORMAT_MOD_LINEAR};
    struct gbm_bo *bo = dev ? gbm_bo_import(dev, GBM_BO_IMPORT_FD_MODIFIER, &input, GBM_BO_USE_LINEAR) : NULL;
    if (!bo) { close(fd); return VA_STATUS_ERROR_OPERATION_FAILED; }
    if (gbm_bo_get_stride_for_plane(bo, 0) != pitch || gbm_bo_get_modifier(bo) != DRM_FORMAT_MOD_LINEAR) {
        gbm_bo_destroy(bo); close(fd); return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
    }
    s->gbm_bo = bo; s->gbm_stride = pitch; s->gbm_dirty = 0; s->gbm_imported = 1; s->import_fd = fd;
    s->stride = pitch; s->aligned_h = s->height; s->cap_fourcc = V4L2_PIX_FMT_NV12;
    s->has_pic = 1; s->last_writer = VAV4L2_WRITER_BO;
    return VA_STATUS_SUCCESS;
}

static void copy_plane(uint8_t *dst, uint32_t dst_stride, const uint8_t *src,
                       uint32_t src_stride, uint32_t width, uint32_t rows)
{
    if (dst_stride == src_stride && dst_stride == width) {
        memcpy(dst, src, (size_t)dst_stride * rows);
        return;
    }
    for (uint32_t y = 0; y < rows; y++)
        memcpy(dst + (size_t)dst_stride * y, src + (size_t)src_stride * y,
               width);
}

int vav4l2_gbm_surface_upload(struct vav4l2_surface *s, const void *src,
                              uint32_t src_stride, uint32_t src_alh)
{
    uint32_t w, h;
    uint8_t *dst;
    struct cpu_bo_map map_data = {0};
    uint32_t mstride = 0;

    if (!s || !s->gbm_bo || !src || !src_stride) return -1;
    if (s->format != VA_FOURCC_NV12) {
        s->gbm_dirty = 1;
        if (src_alh < s->height || import_cpu_access(s, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE) < 0) return -1;
        uint32_t rows = vav4l2_va_image_size(s->format, s->gbm_stride, s->height) / s->gbm_stride;
        dst = map_cpu_bo(s, vav4l2_default_image_stride(s->format, s->width), rows, GBM_BO_TRANSFER_WRITE, &mstride, &map_data);
        int ret = -1;
        if (dst) {
            struct vav4l2_image_view from = {.data = (void *)src, .fourcc = s->format, .stride = src_stride,
                .width = s->width, .height = s->height, .alloc_height = src_alh,
                .size = vav4l2_va_image_size(s->format, src_stride, src_alh)};
            struct vav4l2_image_view to = {.data = dst, .fourcc = native_storage_format(s->format), .stride = mstride,
                .width = s->width, .height = s->height, .size = vav4l2_va_image_size(s->format, mstride, s->height)};
            if (mstride == s->gbm_stride) ret = vav4l2_copy_image_rect(&to, 0, 0, &from, 0, 0, s->width, s->height) == VA_STATUS_SUCCESS ? 0 : -1;
            unmap_cpu_bo(s, &map_data);
        }
        if (import_cpu_access(s, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE) < 0) ret = -1;
        if (!ret) s->gbm_dirty = 0;
        return ret;
    }
    w = s->width; h = s->height;
    s->gbm_dirty = 1;
    if (!w || !h || src_stride < w + (w & 1) || src_alh < h) return -1;
    if (import_cpu_access(s, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE) < 0) return -1;
    dst = map_cpu_bo(s, w + (w & 1), h + (h + 1) / 2,
                     GBM_BO_TRANSFER_WRITE, &mstride, &map_data);
    if (!dst) {
        fprintf(stderr, "v4l2: gbm: bo map failed\n");
        (void)import_cpu_access(s, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
        return -1;
    }
    if (mstride < w + (w & 1)) {
        unmap_cpu_bo(s, &map_data); (void)import_cpu_access(s, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE); return -1;
    }
    copy_plane(dst, mstride, src, src_stride, w, h);
    copy_plane(dst + (size_t)mstride * h, mstride,
               (const uint8_t *)src + (size_t)src_stride * src_alh,
               src_stride, w + (w & 1), (h + 1) / 2);
    unmap_cpu_bo(s, &map_data);
    if (import_cpu_access(s, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE) < 0) return -1;
    s->gbm_dirty = 0;
    return 0;
}

/* Lazy memfd: refill the memfd snapshot (capture layout: Y@0,
 * UV@stride*aligned_h) from the bo (image layout: Y@0, UV@gbm_stride*h).
 * Only the visible rows/bytes are copied — padding is never read. */
int vav4l2_surface_ensure_memfd(struct vav4l2_surface *s)
{
    uint32_t sz, w, h, uvw;
    struct cpu_bo_map bo_map = {0}; void *m;
    uint8_t *bo_data = NULL, *dst;
    uint32_t bo_stride = 0;

    if (!s || !vav4l2_memfd_stale(s) || !s->gbm_bo)
        return 0;
    if (!s->stride || !s->aligned_h)
        return -1;
    w = s->width;
    h = s->height;
    uvw = w + (w & 1);
    sz = vav4l2_capture_plane_size(V4L2_PIX_FMT_NV12, s->stride, s->aligned_h);
    if (vav4l2_surface_grow_memfd(s, sz) < 0)
        return -1;
    if (import_cpu_access(s, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ) < 0) return -1;
    bo_data = map_cpu_bo(s, w, h + (h + 1) / 2,
                         GBM_BO_TRANSFER_READ, &bo_stride, &bo_map);
    if (!bo_data) {
        (void)import_cpu_access(s, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
        return -1;
    }
    if (bo_stride < uvw) {
        unmap_cpu_bo(s, &bo_map); (void)import_cpu_access(s, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ); return -1;
    }
    m = vav4l2_surface_map_memfd(s, sz);
    if (!m) {
        unmap_cpu_bo(s, &bo_map);
        (void)import_cpu_access(s, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
        return -1;
    }
    dst = m;
    for (uint32_t y = 0; y < h; y++)
        memcpy(dst + (size_t)s->stride * y,
               bo_data + (size_t)bo_stride * y, w);
    dst = (uint8_t *)m + (size_t)s->stride * s->aligned_h;
    for (uint32_t y = 0; y < (h + 1) / 2; y++)
        memcpy(dst + (size_t)s->stride * y,
               bo_data + (size_t)bo_stride * (h + y), uvw);
    unmap_cpu_bo(s, &bo_map);
    if (import_cpu_access(s, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ) < 0) return -1;
    s->last_writer = VAV4L2_WRITER_MEMFD;
    return 0;
}

static void storage_trace(const struct vav4l2_surface *s, const char *stage, int provider, int status, int error)
{
    int saved = errno; const char *path = getenv("VAV4L2_STORAGE_TRACE"); if (!path || !*path) return;
    FILE *file = fopen(path, "a"); if (!file) { perror("storage trace"); errno = saved; return; }
    if (!ftell(file)) fprintf(file, "PID\tSURFACE\tSTAGE\tPROVIDER\tSTATUS\tERRNO\tFORMAT\tWIDTH\tHEIGHT\n");
    fprintf(file, "%ld\t%u\t%s\t%d\t%d\t%d\t%08x\t%u\t%u\n", (long)getpid(), s->surface_id, stage, provider, status, error, s->format, s->width, s->height);
    if (fclose(file)) perror("close storage trace");
    errno = saved;
}

int vav4l2_gbm_surface_ensure(struct vav4l2_surface *s)
{
    struct gbm_device *dev;
    uint32_t rows;

    if (!s || s->width == 0 || s->height == 0)
        return -1;
    if (s->gbm_bo)
        return s->gbm_dirty ? vav4l2_gbm_surface_sync(s) : 0;
    struct vav4l2_prime_layout layout;
    if (s->format != VA_FOURCC_NV12 && vav4l2_prime_layout(s->format, s->width, s->height, &layout) < 0) return -1;
    dev = vav4l2_gbm_device(s);
    if (!dev)
        return -1;
    uint32_t bytes = s->format == VA_FOURCC_NV12 ? s->width + (s->width & 1) : vav4l2_default_image_stride(s->format, s->width);
    if (s->format == VA_FOURCC_422H || s->format == VA_FOURCC_YV16) bytes = (bytes + 127) & ~127u;
    uint32_t size = vav4l2_va_image_size(s->format, bytes, s->height);
    if (!bytes || !size || size % bytes) return -1;
    rows = size / bytes;
    unsigned shared_pitch = (bytes + 63) & ~63u;
    const char *storage = getenv("VAV4L2_SHARED_STORAGE");
    int coherent = storage && !strcmp(storage, "coherent");
    int providers = coherent ? 1 : 2;
    for (int provider = 0; provider < providers && !s->gbm_bo; provider++) {
        errno = 0;
        int shared = coherent || provider ? vav4l2_alloc_coherent_storage(shared_pitch, rows) : vav4l2_alloc_shared_storage(shared_pitch, rows);
        storage_trace(s, "allocate", provider, shared < 0 ? -1 : 0, shared < 0 ? errno : 0);
        if (shared < 0) continue;
        struct gbm_import_fd_modifier_data input = {.width = bytes, .height = rows, .format = GBM_FORMAT_R8,
            .num_fds = 1, .fds = {shared}, .strides = {(int)shared_pitch}, .modifier = DRM_FORMAT_MOD_LINEAR};
        errno = 0;
        s->gbm_bo = gbm_bo_import(dev, GBM_BO_IMPORT_FD_MODIFIER, &input, GBM_BO_USE_LINEAR);
        storage_trace(s, "import", provider, s->gbm_bo ? 0 : -1, s->gbm_bo ? 0 : errno);
        if (s->gbm_bo && (gbm_bo_get_stride_for_plane(s->gbm_bo, 0) != shared_pitch || gbm_bo_get_modifier(s->gbm_bo) != DRM_FORMAT_MOD_LINEAR)) {
            storage_trace(s, "layout", provider, -1, EINVAL);
            gbm_bo_destroy(s->gbm_bo); s->gbm_bo = NULL;
        }
        if (s->gbm_bo) { s->gbm_writable = 1; s->gbm_external_fd = shared; }
        else close(shared);
    }
    if (!s->gbm_bo) {
        struct vav4l2_gbm_state *state = s->driver_data ? &s->driver_data->gbm : &fallback_gbm;
        int shared = vav4l2_alloc_drm_storage(state->fd, shared_pitch, rows, coherent);
        if (shared >= 0) {
            struct gbm_import_fd_modifier_data input = {.width = bytes, .height = rows, .format = GBM_FORMAT_R8,
                .num_fds = 1, .fds = {shared}, .strides = {(int)shared_pitch}, .modifier = DRM_FORMAT_MOD_LINEAR};
            s->gbm_bo = gbm_bo_import(dev, GBM_BO_IMPORT_FD_MODIFIER, &input, GBM_BO_USE_LINEAR);
            if (s->gbm_bo && (gbm_bo_get_stride_for_plane(s->gbm_bo, 0) != shared_pitch || gbm_bo_get_modifier(s->gbm_bo) != DRM_FORMAT_MOD_LINEAR)) {
                gbm_bo_destroy(s->gbm_bo); s->gbm_bo = NULL;
            }
            if (s->gbm_bo) { s->gbm_writable = 1; s->gbm_external_fd = shared; }
            else { int saved = errno; close(shared); errno = saved; return -1; }
        } else if (errno != ENOTSUP && errno != ENOTTY && errno != EOPNOTSUPP) return -1;
    }
    if (!s->gbm_bo) {
        errno = 0;
        s->gbm_bo = gbm_bo_create(dev, bytes, rows, GBM_FORMAT_R8, GBM_BO_USE_LINEAR);
        storage_trace(s, "native-create", 2, s->gbm_bo ? 0 : -1, s->gbm_bo ? 0 : errno);
    }
    if (!s->gbm_bo) {
        fprintf(stderr, "v4l2: gbm: bo create %ux%u failed\n",
                s->width, rows);
        return -1;
    }
    s->gbm_stride = gbm_bo_get_stride_for_plane(s->gbm_bo, 0);
    if (s->gbm_stride < bytes || (s->format != VA_FOURCC_NV12 &&
        vav4l2_va_image_size(s->format, s->gbm_stride, s->height) / s->gbm_stride != rows)) {
        vav4l2_gbm_surface_destroy(s); return -1;
    }
    if (s->last_writer != VAV4L2_WRITER_NONE && vav4l2_gbm_surface_sync(s) < 0) {
        storage_trace(s, "initial-upload", -1, -1, errno);
        vav4l2_gbm_surface_destroy(s);
        return -1;
    }
    return 0;
}

int vav4l2_gbm_surface_ensure_export(struct vav4l2_surface *s, uint32_t flags)
{ (void)flags; return vav4l2_gbm_surface_ensure(s); }

/* Upload the surface's current pixel data into its bo. The pixels live in
 * either cpu_ptr (VPP output, vaPutImage uploads; image layout: Y@0,
 * UV@cpu_stride*h) or the memfd (decode pull_capture; capture layout).
 * The last_writer marker says which backing was written last. */
int vav4l2_gbm_surface_sync(struct vav4l2_surface *s)
{
    if (!s || !s->gbm_bo)
        return -1;
    if (s->last_writer == VAV4L2_WRITER_BO)
        return s->gbm_dirty ? -1 : 0; /* bo already holds the latest frame (lazy memfd) */
    if ((s->last_writer == VAV4L2_WRITER_CPU || s->format != VA_FOURCC_NV12) && s->cpu_ptr && s->cpu_stride)
        return vav4l2_gbm_surface_upload(s, s->cpu_ptr, s->cpu_stride,
                                         s->height);
    if (s->last_writer == VAV4L2_WRITER_MEMFD && s->memfd_fd >= 0 &&
        s->stride && s->aligned_h) {
        uint32_t sz = vav4l2_capture_plane_size(V4L2_PIX_FMT_NV12,
                                                s->stride, s->aligned_h);
        void *m = vav4l2_surface_map_memfd(s, sz);
        int r;

        if (!m)
            return -1;
        r = vav4l2_gbm_surface_upload(s, m, s->stride, s->aligned_h);
        if (r == 0)
            s->last_writer = VAV4L2_WRITER_BO; /* bo now holds the frame */
        return r;
    }
    return -1;
}

void vav4l2_gbm_surface_destroy(struct vav4l2_surface *s)
{
    if (s && s->gbm_bo) {
        gbm_bo_destroy(s->gbm_bo);
        s->gbm_bo = NULL;
        s->gbm_stride = 0; s->gbm_dirty = 0;
    }
    if (s && s->gbm_imported) { close(s->import_fd); s->import_fd = -1; s->gbm_imported = 0; }
    if (s && s->gbm_writable) { close(s->gbm_external_fd); s->gbm_external_fd = -1; s->gbm_writable = 0; }
    vav4l2_prime_destroy(s);
}

/* Once shared, descriptor identity belongs to the retained DMA-BUF, not a
 * fresh GBM export which may flush or replace driver-private storage. */
static int shared_bo_fd(const struct vav4l2_surface *s)
{
    int fd = s->gbm_imported ? s->import_fd : s->gbm_writable ? s->gbm_external_fd : -1;
    return fd >= 0 ? fcntl(fd, F_DUPFD_CLOEXEC, 0) : gbm_bo_get_fd_for_plane(s->gbm_bo, 0);
}

VAStatus vav4l2_surface_fill_prime_gbm(const struct vav4l2_surface *surf,
                                       uint32_t flags, void *descriptor)
{
    VADRMPRIMESurfaceDescriptor *desc = descriptor;
    uint32_t h, pitch;
    int fd;

    if (!surf || !surf->gbm_bo || !descriptor)
        return VA_STATUS_ERROR_INVALID_SURFACE;
    h = surf->height;
    pitch = surf->gbm_stride;
    if (surf->format != VA_FOURCC_NV12) {
        struct vav4l2_prime_layout l;
        if (vav4l2_prime_layout(surf->format, surf->width, h, &l) < 0 || gbm_bo_get_modifier(surf->gbm_bo) != DRM_FORMAT_MOD_LINEAR)
            return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
        uint32_t size = vav4l2_va_image_size(surf->format, pitch, h), actual;
        fd = shared_bo_fd(surf);
        if (fd < 0) return errno == EMFILE || errno == ENFILE || errno == ENOMEM ? VA_STATUS_ERROR_ALLOCATION_FAILED : VA_STATUS_ERROR_OPERATION_FAILED;
        if (vav4l2_export_fd_validate(fd, size, &actual) < 0) { close(fd); return VA_STATUS_ERROR_OPERATION_FAILED; }
        VADRMPRIMESurfaceDescriptor d = {.fourcc = surf->format, .width = surf->width, .height = h, .num_objects = 1};
        d.objects[0].fd = fd; d.objects[0].size = actual; d.objects[0].drm_format_modifier = DRM_FORMAT_MOD_LINEAR;
        int separate = (flags & VA_EXPORT_SURFACE_SEPARATE_LAYERS) && l.planes > 1;
        d.num_layers = separate ? l.planes : 1;
        unsigned offset = 0;
        for (unsigned i = 0; i < l.planes; i++) {
            unsigned layer = separate ? i : 0, plane = separate ? 0 : i;
            unsigned stride = i && l.planes == 3 ? (pitch + 1) / 2 : pitch;
            d.layers[layer].drm_format = separate ? l.plane_format[i] : l.drm_format;
            d.layers[layer].num_planes = separate ? 1 : l.planes;
            d.layers[layer].pitch[plane] = stride; d.layers[layer].offset[plane] = offset;
            offset += stride * l.rows[i];
        }
        if (surf->format == VA_FOURCC_YV12 || surf->format == VA_FOURCC_YV16) {
            uint32_t *u = separate ? &d.layers[2].offset[0] : &d.layers[0].offset[2];
            uint32_t *v = separate ? &d.layers[1].offset[0] : &d.layers[0].offset[1];
            uint32_t first = *v; *v = *u; *u = first;
        }
        *desc = d; return VA_STATUS_SUCCESS;
    }
    uint64_t size = (uint64_t)pitch * ((uint64_t)h + ((uint64_t)h + 1) / 2);
    if (!h || pitch < surf->width + (surf->width & 1) || size > UINT32_MAX ||
        gbm_bo_get_modifier(surf->gbm_bo) != DRM_FORMAT_MOD_LINEAR)
        return VA_STATUS_ERROR_OPERATION_FAILED;
    fd = shared_bo_fd(surf);
    if (fd < 0) return errno == EMFILE || errno == ENFILE || errno == ENOMEM ? VA_STATUS_ERROR_ALLOCATION_FAILED : VA_STATUS_ERROR_OPERATION_FAILED;
    uint32_t object_size;
    if (vav4l2_export_fd_validate(fd, size, &object_size) < 0) {
        close(fd); return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    vav4l2_fill_prime_layers(desc, fd, object_size, DRM_FORMAT_MOD_LINEAR,
                             VA_FOURCC_NV12, surf->width, h, pitch, h,
                             DRM_FORMAT_NV12, DRM_FORMAT_R8, DRM_FORMAT_GR88,
                             flags);
    return VA_STATUS_SUCCESS;
}

int vav4l2_gbm_enable_write(struct vav4l2_surface *s)
{
    if (s->gbm_imported || s->gbm_writable) return 0;
    int fd = gbm_bo_get_fd_for_plane(s->gbm_bo, 0); uint32_t size;
    uint32_t need = vav4l2_va_image_size(s->format, s->gbm_stride, s->height);
    if (fd < 0) return -1;
    if (vav4l2_export_fd_validate(fd, need, &size) < 0) { close(fd); return -1; }
    if (vav4l2_dmabuf_sync(fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ) < 0) { close(fd); return -1; }
    if (vav4l2_dmabuf_sync(fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ) < 0) { close(fd); return -1; }
    s->gbm_external_fd = fd; s->gbm_writable = 1; s->has_pic = 1; return 0;
}

int vav4l2_gbm_read_cpu(struct vav4l2_surface *s)
{
    if (!s->gbm_bo || s->gbm_dirty || vav4l2_surface_ensure_cpu(s) < 0 ||
        import_cpu_access(s, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ) < 0) return -1;
    struct cpu_bo_map map_data = {0}; uint32_t pitch = 0;
    uint32_t rows = vav4l2_va_image_size(s->format, s->gbm_stride, s->height) / s->gbm_stride;
    uint32_t bytes = s->format == VA_FOURCC_NV12 ? s->width + (s->width & 1) : vav4l2_default_image_stride(s->format, s->width);
    void *map = map_cpu_bo(s, bytes, rows, GBM_BO_TRANSFER_READ, &pitch, &map_data);
    int ret = -1;
    if (map) {
        struct vav4l2_image_view from = {.data = map, .fourcc = native_storage_format(s->format), .stride = pitch,
            .width = s->width, .height = s->height, .size = vav4l2_va_image_size(s->format, pitch, s->height)};
        struct vav4l2_image_view to = {.data = s->cpu_ptr, .fourcc = s->format, .stride = s->cpu_stride,
            .width = s->width, .height = s->height, .size = s->cpu_size};
        if (pitch == s->gbm_stride) ret = vav4l2_copy_image_rect(&to, 0, 0, &from, 0, 0, s->width, s->height) == VA_STATUS_SUCCESS ? 0 : -1;
        unmap_cpu_bo(s, &map_data);
    }
    if (import_cpu_access(s, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ) < 0) ret = -1;
    if (!ret) s->last_writer = VAV4L2_WRITER_CPU;
    return ret;
}
