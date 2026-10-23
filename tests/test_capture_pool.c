/* Real FD/map lifetime with a native pool too small for eight retained frames. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <linux/dma-buf.h>
#include <linux/media.h>
#include <drm_fourcc.h>
#include "va_request.h"
extern VAStatus __vaDriverInit_1_20(VADriverContextP ctx);
static int failures, fault, allocations, mmap_freed, dma_freed, dma_requests, queued, current_index, sync_reads, sync_writes;
static int owned[64], n_owned, query_calls, export_calls;
static unsigned frame, native_count = 2;
static void *pool_mapping;
static int pool_mapped;
static struct vareq_context *current;
static struct v4l2_format format;
static void expect(int ok, const char *tag) { if (!ok) { fprintf(stderr, "FAIL capture pool %s fault=%d\n", tag, fault); failures++; } }
int __wrap_poll(struct pollfd *p, nfds_t n, int timeout) { (void)timeout; expect(n == 1, "single-device-wait"); p->revents = p->events; return 1; }
int __wrap_vareq_alloc_shared_storage(unsigned bytes, unsigned rows)
{
    allocations++;
    if (fault == 1 && allocations == 3) { errno = ENOMEM; return -1; }
    int fd = fault == 7 && n_owned ? fcntl(owned[0], F_DUPFD_CLOEXEC, 0) : memfd_create("capture pool raw bytes", MFD_CLOEXEC);
    expect(fd >= 0 && ftruncate(fd, fault == 2 ? 1 : (off_t)bytes * rows + 4096) == 0, "pool-storage-fixture");
    if (fd >= 0 && fault != 2) { unsigned char poison[12288]; memset(poison, 0xa5, sizeof(poison)); expect(pwrite(fd, poison, sizeof(poison), 0) == sizeof(poison), "poison-entire-allocation-including-padding"); }
    if (fd >= 0 && n_owned < 64) owned[n_owned++] = fd;
    return fd;
}
void *__real_mmap(void *a, size_t n, int p, int f, int fd, off_t o);
void *__real_mmap64(void *a, size_t n, int p, int f, int fd, off64_t o);
static int pool_map_allowed(int fd, int prot)
{
    for (int i = 0; i < n_owned; i++) if (fd == owned[i]) {
        expect(prot == PROT_READ ? sync_reads == 1 && !sync_writes : sync_writes == 1 && !sync_reads, "pool-mapping-bracketed-by-correct-CPU-sync");
        if (fault == 8) { errno = ENOMEM; return 0; }
        break;
    }
    return 1;
}
static void track_pool_map(int fd, void *map)
{
    if (map != MAP_FAILED) for (int i = 0; i < n_owned; i++) if (fd == owned[i]) { expect(!pool_mapped, "one-temporary-pool-map"); pool_mapping = map; pool_mapped = 1; break; }
}
void *__wrap_mmap(void *a, size_t n, int p, int f, int fd, off_t o) { void *map = pool_map_allowed(fd, p) ? __real_mmap(a, n, p, f, fd, o) : MAP_FAILED; track_pool_map(fd, map); return map; }
void *__wrap_mmap64(void *a, size_t n, int p, int f, int fd, off64_t o) { void *map = pool_map_allowed(fd, p) ? __real_mmap64(a, n, p, f, fd, o) : MAP_FAILED; track_pool_map(fd, map); return map; }
int __real_munmap(void *map, size_t size);
int __wrap_munmap(void *map, size_t size) { int result = __real_munmap(map, size); if (!result && pool_mapped && map == pool_mapping) { expect(sync_reads == 1 || sync_writes == 1, "pool-unmap-stays-inside-CPU-sync"); pool_mapped = 0; } return result; }
static int mock_ioctl(int fd, unsigned long req, void *arg)
{
    (void)fd;
    if (req == DMA_BUF_IOCTL_SYNC) {
        struct dma_buf_sync *s = arg;
        if (s->flags & DMA_BUF_SYNC_END) expect(!pool_mapped, "pool-unmapped-before-CPU-END");
        if (s->flags & DMA_BUF_SYNC_END) { if (s->flags & DMA_BUF_SYNC_WRITE) sync_writes--; else sync_reads--; }
        else { if (s->flags & DMA_BUF_SYNC_WRITE) sync_writes++; else sync_reads++; }
        if (fault == 5 && (s->flags & DMA_BUF_SYNC_END) && (s->flags & DMA_BUF_SYNC_READ)) { errno = EIO; return -1; }
        return 0;
    }
    if (req == VIDIOC_S_FMT) {
        format = *(struct v4l2_format *)arg;
        format.fmt.pix_mp.plane_fmt[0].bytesperline = 64;
        format.fmt.pix_mp.plane_fmt[0].sizeimage = 8192; /* Includes auxiliary bytes beyond visible pixels. */
        *(struct v4l2_format *)arg = format; return 0;
    }
    if (req == VIDIOC_G_FMT) { *(struct v4l2_format *)arg = format; return 0; }
    if (req == VIDIOC_REQBUFS) {
        struct v4l2_requestbuffers *r = arg;
        if (!r->count) { if (r->memory == V4L2_MEMORY_DMABUF) { dma_freed++; if (fault == 4) { errno = EBUSY; return -1; } } else mmap_freed++; return 0; }
        if (r->memory == V4L2_MEMORY_DMABUF) { dma_requests++; if (fault == 6) { errno = EINVAL; return -1; } if (fault == 3 || fault == 4) r->count = 1; if (fault == 9) r->count = 0; }
        else r->count = native_count;
        return 0;
    }
    if (req == VIDIOC_QUERYBUF) { query_calls++; errno = EIO; return -1; }
    if (req == VIDIOC_EXPBUF) { export_calls++; errno = EIO; return -1; }
    if (req == VIDIOC_QBUF) {
        struct v4l2_buffer *b = arg;
        if (b->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            expect(b->memory == V4L2_MEMORY_DMABUF && !(b->flags & V4L2_BUF_FLAG_REQUEST_FD) && b->length == 1 && b->m.planes[0].length >= 8192,
                "internal-pool-capture-queues-bare-full-auxiliary-allocation");
            current_index = b->index; queued++;
        }
        return 0;
    }
    if (req == MEDIA_REQUEST_IOC_QUEUE) {
        unsigned char data[8192]; memset(data, frame, sizeof(data));
        expect(!sync_reads && !sync_writes && pwrite(current->capture_dma[current_index].fd, data, sizeof(data), 0) == sizeof(data), "device-write-outside-CPU-sync"); return 0;
    }
    if (req == VIDIOC_DQBUF) {
        struct v4l2_buffer *b = arg;
        if (b->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) { b->index = current_index; b->m.planes[0].bytesused = 8192; }
        else b->index = 0;
        return 0;
    }
    return 0;
}
static void setup(struct vareq_context *c, struct vareq_driver_data *dd, int backing)
{
    *c = (struct vareq_context){.driver_data = dd, .codec = VAREQ_CODEC_H264, .v4l2_fd = dup(backing), .media_fd = -1,
        .request_fd = dup(backing), .output_bufs_allocd = 0, .output_buf_size = 64, .output_plane_len = {64}, .streamed = 0};
    dd->contexts = current = c; allocations = n_owned = mmap_freed = dma_freed = dma_requests = query_calls = export_calls = queued = 0;
    vareq_set_ioctl_hook(mock_ioctl);
}
static void cleanup(struct vareq_context *c, struct vareq_driver_data *dd)
{
    vareq_release_context_device(c); dd->contexts = NULL;
    expect(!sync_reads && !sync_writes, "CPU-sync-pairs-balanced");
    for (int i = 0; i < n_owned; i++) expect(fcntl(owned[i], F_GETFD) < 0 && errno == EBADF, "all-pool-FDs-closed");
}
int main(void)
{
    struct VADriverVTable v = {0}; struct VADriverContext va = {.vtable = &v};
    setenv("VAREQ_PROBE_NOCACHE", "1", 1); setenv("VAREQ_CAPTURE_DMABUF", "0", 1); unsetenv("VAREQ_CAPTURE_POOL_DMABUF");
    if (__vaDriverInit_1_20(&va)) return 1;
    struct vareq_driver_data *dd = va.pDriverData; struct vareq_context c;
    int backing = memfd_create("decode device fixture", MFD_CLOEXEC); if (backing < 0) return 1;
    for (unsigned mode = 0; mode < 2; mode++) {
        setenv("VAREQ_EXPBUF_EXPORT", mode ? "1" : "0", 1);
        setup(&c, dd, backing); fault = 0; native_count = mode ? 5 : 2;
        expect(!vareq_ensure_capture(&c, 16, 16, V4L2_PIX_FMT_NV12) && c.capture_internal && c.capture_memory == V4L2_MEMORY_DMABUF && c.capture_bufs_allocd == VAREQ_NUM_CAPTURE_BUFS && mmap_freed == 1,
            "undersized-native-pool-replaced-before-any-queue");
        c.output_bufs_allocd = 1;
        VASurfaceID ids[8]; VAImage image; VAImageFormat fmt = {.fourcc = VA_FOURCC_NV12};
        expect(v.vaCreateSurfaces(&va, 16, 16, VA_RT_FORMAT_YUV420, 8, ids) == VA_STATUS_SUCCESS && v.vaCreateImage(&va, &fmt, 16, 16, &image) == VA_STATUS_SUCCESS, "public-Image-and-surface-setup");
        int retained = -1;
        for (frame = 1; frame <= 8; frame++) {
            struct vareq_surface *s = dd->surfaces[ids[frame - 1]]; c.current_surface = s;
            if (frame == 1) {
                expect(!vareq_claim_capture_for_export(&c, s), "reserve-slot-before-first-picture"); retained = vareq_capture_expbuf(&c, s->buf_index);
                unsigned char zero = 1; expect(retained >= 0 && (fcntl(retained, F_GETFD) & FD_CLOEXEC) && pread(retained, &zero, 1, 0) == 1 && !zero, "predecode-export-is-zeroed-independent-CLOEXEC-FD");
                zero = 1; expect(pread(retained, &zero, 1, 12287) == 1 && !zero, "predecode-export-padding-is-zeroed-too");
            }
            int index = vareq_decode_submit(&c, 0, 16, frame * 1000);
            expect(index >= 0 && !vareq_surface_pull_capture(&c, s, index) && !s->cap_view && s->capture_ctx == &c &&
                vareq_validate_reference(&c, ids[frame - 1]) == VA_STATUS_SUCCESS, "all-eight-frames-retain-real-DPB-with-snapshot-readback");
            s->status = VASurfaceReady;
            expect(v.vaGetImage(&va, ids[frame - 1], 0, 0, 16, 16, image.image_id) == VA_STATUS_SUCCESS, "public-GetImage-reads-internal-pool-snapshot");
            unsigned char *pixels; expect(v.vaMapBuffer(&va, image.buf, (void **)&pixels) == VA_STATUS_SUCCESS && pixels[0] == frame && pixels[image.offsets[1]] == frame, "both-image-planes-match-decoded-frame");
            v.vaUnmapBuffer(&va, image.buf);
        }
        expect(!query_calls && !export_calls && queued == 8 && c.n_free_cap == VAREQ_NUM_CAPTURE_BUFS - 8, "no-native-MMAP-or-EXPBUF-and-no-live-DPB-recycling");
        /* Reimporting an internal export cannot become a writeback target. */
        struct vareq_surface alias = {.surface_id = 100, .width = 16, .height = 16, .format = VA_FOURCC_NV12, .buf_index = -1, .memfd_fd = -1};
        VADRMPRIMESurfaceDescriptor d = {.fourcc = VA_FOURCC_NV12, .width = 16, .height = 16, .num_objects = 1, .num_layers = 1};
        d.objects[0].fd = retained; d.objects[0].size = 8192; d.layers[0].drm_format = DRM_FORMAT_NV12; d.layers[0].num_planes = 2;
        d.layers[0].pitch[0] = d.layers[0].pitch[1] = 64; d.layers[0].offset[1] = 1024;
        expect(!vareq_prime_import(&alias, &d), "alias-fixture"); c.current_surface = &alias;
        int previous = queued; expect(vareq_decode_submit(&c, 0, 16, 9000) < 0 && errno == EBUSY && queued == previous, "pool-alias-rejected-before-submission"); vareq_prime_destroy(&alias);
        c.current_surface = NULL; cleanup(&c, dd);
        unsigned char value = 0; expect(retained >= 0 && pread(retained, &value, 1, 0) == 1 && value == 1, "export-outlives-pool-and-context"); close(retained);
        expect(v.vaGetImage(&va, ids[7], 0, 0, 16, 16, image.image_id) == VA_STATUS_SUCCESS, "last-snapshot-survives-context-teardown");
        v.vaDestroyImage(&va, image.image_id); v.vaDestroySurfaces(&va, ids, 8);
    }
    for (fault = 1; fault <= 9; fault++) {
        if (fault == 5) continue;
        setup(&c, dd, backing); native_count = 2;
        int result = vareq_ensure_capture(&c, 16, 16, V4L2_PIX_FMT_NV12);
        expect(fault == 4 ? result < 0 : !result && !c.capture_internal && !c.capture_memory, "partial-allocation-short-object-short-queue-and-stop-failure-are-transactional");
        cleanup(&c, dd);
    }
    fault = 0; setup(&c, dd, backing); native_count = 2;
    expect(!vareq_ensure_capture(&c, 16, 16, V4L2_PIX_FMT_NV12) && c.capture_internal, "prepare-END-failure");
    VASurfaceID failed_id; expect(!v.vaCreateSurfaces(&va, 16, 16, VA_RT_FORMAT_YUV420, 1, &failed_id), "END-failure-target");
    c.current_surface = dd->surfaces[failed_id]; c.output_bufs_allocd = 1; frame = 9;
    int failed_index = vareq_decode_submit(&c, 0, 16, 9000); fault = 5;
    expect(failed_index >= 0 && vareq_surface_pull_capture(&c, c.current_surface, failed_index) < 0 &&
        !c.current_surface->has_pic && vareq_surface_capture_matches(&c, c.current_surface) && c.current_surface->buf_index == failed_index,
        "failed-CPU-END-invalidates-pixels-without-returning-an-owned-slot");
    fault = 0; c.current_surface = NULL; cleanup(&c, dd); v.vaDestroySurfaces(&va, &failed_id, 1);
    fault = 0; setup(&c, dd, backing); setenv("VAREQ_CAPTURE_POOL_DMABUF", "0", 1);
    expect(!vareq_ensure_capture(&c, 16, 16, V4L2_PIX_FMT_NV12) && !allocations && !dma_requests && c.capture_bufs_allocd == 2, "explicit-native-only-policy-kept"); cleanup(&c, dd);
    unsetenv("VAREQ_CAPTURE_POOL_DMABUF"); setup(&c, dd, backing); c.codec = VAREQ_CODEC_AV1;
    expect(!vareq_ensure_capture(&c, 16, 16, V4L2_PIX_FMT_NV12) && !allocations && !dma_requests, "AV1-path-is-deferred"); cleanup(&c, dd);
    for (unsigned i = 0; i < 3; i++) {
        const uint32_t raw[] = {V4L2_PIX_FMT_NV15, V4L2_PIX_FMT_NV16, V4L2_PIX_FMT_NV20};
        setup(&c, dd, backing); expect(!vareq_ensure_capture(&c, 16, 16, raw[i]) && c.capture_internal && c.cap_pixelformat == raw[i], "opaque-pool-preserves-negotiated-packed-and-422-formats"); cleanup(&c, dd);
    }
    setup(&c, dd, backing); native_count = 0;
    expect(!vareq_ensure_capture(&c, 16, 16, V4L2_PIX_FMT_NV12) && c.capture_internal && c.capture_bufs_allocd == VAREQ_NUM_CAPTURE_BUFS, "zero-native-grant-can-still-use-DMA-import"); cleanup(&c, dd);
    native_count = VAREQ_NUM_CAPTURE_BUFS; setup(&c, dd, backing); setenv("VAREQ_CAPTURE_POOL_DMABUF", "required", 1);
    expect(!vareq_ensure_capture(&c, 16, 16, V4L2_PIX_FMT_NV12) && c.capture_internal, "required-pool-also-exercises-a-full-native-grant"); cleanup(&c, dd);
    fault = 1; setup(&c, dd, backing);
    expect(vareq_ensure_capture(&c, 16, 16, V4L2_PIX_FMT_NV12) < 0 && !c.capture_internal, "required-pool-cannot-succeed-via-native-fallback"); cleanup(&c, dd);
    unsetenv("VAREQ_CAPTURE_POOL_DMABUF");
    vareq_set_ioctl_hook(NULL); close(backing); v.vaTerminate(&va);
    printf("capture-pool:%s\n", failures ? "FAIL" : "PASS"); return failures != 0;
}
