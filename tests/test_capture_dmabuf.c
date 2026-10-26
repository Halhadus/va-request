/* Imported client storage is the decoder DPB; no capture MMAP or copy. */
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
#include "va_v4l2.h"
static int failures, fault, queries, cap_index, cap_queues, out_queues, freed_dma, freed_mmap;
static unsigned char decoded = 0x69;
static struct vav4l2_context *current;
static struct v4l2_format negotiated;
static struct timeval submitted_time;
static void expect(int value, const char *tag) { if (!value) { fprintf(stderr, "FAIL %s fault=%d\n", tag, fault); failures++; } }
int __wrap_poll(struct pollfd *p, nfds_t n, int timeout)
{
    expect(n == 1 && (timeout == 2000 || timeout == 3000), "capture-waits-have-timeouts");
    p->revents = p->events; return 1;
}
static void format(struct v4l2_format *f)
{
    f->fmt.pix_mp.num_planes = 1; f->fmt.pix_mp.field = V4L2_FIELD_NONE;
    f->fmt.pix_mp.plane_fmt[0].bytesperline = 64; f->fmt.pix_mp.plane_fmt[0].sizeimage = 4096;
}
static int mock_ioctl(int fd, unsigned long request, void *arg)
{
    (void)fd;
    if (request == DMA_BUF_IOCTL_SYNC) return 0;
    if (request == VIDIOC_ENUM_FMT) {
        struct v4l2_fmtdesc *f = arg; if (f->index) { errno = EINVAL; return -1; }
        f->pixelformat = V4L2_PIX_FMT_NV15; return 0;
    }
    if (request == VIDIOC_S_FMT) { format(arg); negotiated = *(struct v4l2_format *)arg; return 0; }
    if (request == VIDIOC_G_FMT) {
        *(struct v4l2_format *)arg = negotiated; return 0;
    }
    if (request == VIDIOC_REQBUFS) {
        struct v4l2_requestbuffers *r = arg;
        if (fault == 8 && r->memory == V4L2_MEMORY_DMABUF) { errno = EINVAL; return -1; }
        if (!r->count) { if (r->memory == V4L2_MEMORY_DMABUF) freed_dma++; else freed_mmap++; }
        if (r->count && (fault == 1 || fault == 7) && r->memory == V4L2_MEMORY_DMABUF) { errno = fault == 7 ? ENOMEM : EINVAL; return -1; }
        if (r->count) r->count = 4;
        return 0;
    }
    if (request == VIDIOC_QUERYBUF) { queries++; errno = EIO; return -1; }
    if (request == VIDIOC_QBUF) {
        struct v4l2_buffer *b = arg;
        if (b->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
            out_queues++;
            submitted_time = b->timestamp;
            expect(b->memory == V4L2_MEMORY_MMAP && (b->flags & V4L2_BUF_FLAG_REQUEST_FD), "compressed-output-keeps-request-binding");
        } else {
            cap_queues++; cap_index = b->index;
            expect(b->memory == V4L2_MEMORY_DMABUF && !(b->flags & V4L2_BUF_FLAG_REQUEST_FD) && !b->request_fd &&
                b->m.planes[0].length == 8192 && (fcntl(b->m.planes[0].m.fd, F_GETFD) & FD_CLOEXEC), "capture-queues-owned-client-fd-bare-with-allocation-size");
        }
        return 0;
    }
    if (request == MEDIA_REQUEST_IOC_QUEUE) {
        unsigned char pixels[4096]; memset(pixels, decoded, sizeof(pixels));
        expect(pwrite(current->capture_dma[cap_index].fd, pixels, sizeof(pixels), 0) == sizeof(pixels), "mock-decoder-writes-the-imported-object"); return 0;
    }
    if (request == VIDIOC_DQBUF) {
        struct v4l2_buffer *b = arg;
        if (b->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            b->index = cap_index; b->m.planes[0].bytesused = 4096; b->timestamp = submitted_time;
            if (fault == 2) b->flags = V4L2_BUF_FLAG_ERROR;
            if (fault == 3) b->m.planes[0].bytesused = 4095;
            if (fault == 4) b->m.planes[0].data_offset = 1;
            if (fault == 5) b->memory = V4L2_MEMORY_MMAP;
            if (fault == 6) b->index = 4;
        }
        return 0;
    }
    return 0;
}
static void imported(struct vav4l2_surface *s, int owner, unsigned id)
{
    *s = (struct vav4l2_surface){.surface_id = id, .width = 16, .height = 16, .format = VA_FOURCC_NV12,
        .memfd_fd = -1, .buf_index = -1, .import_fd = -1, .gbm_external_fd = -1, .cpu_stride = 64, .cpu_size = 1536};
    VADRMPRIMESurfaceDescriptor d = {.fourcc = VA_FOURCC_NV12, .width = 16, .height = 16, .num_objects = 1, .num_layers = 1};
    d.objects[0].fd = owner; d.objects[0].size = 8192;
    d.layers[0].drm_format = DRM_FORMAT_NV12; d.layers[0].num_planes = 2;
    d.layers[0].pitch[0] = d.layers[0].pitch[1] = 64; d.layers[0].offset[1] = 1024;
    expect(!vav4l2_prime_import(s, &d), "import-client-buffer-with-space-for-decoder-auxiliary-data");
}
int main(void)
{
    struct vav4l2_driver_data dd = {0}; struct vav4l2_surface s[3];
    int owner[2] = {memfd_create("capture-yuv-one", MFD_CLOEXEC), memfd_create("capture-yuv-two", MFD_CLOEXEC)};
    if (owner[0] < 0 || owner[1] < 0 || ftruncate(owner[0], 8192) || ftruncate(owner[1], 8192)) return 1;
    vav4l2_set_ioctl_hook(mock_ioctl);
    imported(&s[0], owner[0], 1); imported(&s[1], owner[1], 2); imported(&s[2], owner[0], 3);
    for (unsigned i = 0; i < 3; i++) dd.surfaces[i + 1] = &s[i];
    VASurfaceID targets[] = {1, 2};
    struct vav4l2_context ctx = {.driver_data = &dd, .codec = VAV4L2_CODEC_H264, .render_targets = targets, .num_render_targets = 2,
        .current_surface = &s[0], .v4l2_fd = dup(owner[0]), .request_fd = dup(owner[1]), .media_fd = -1};
    dd.contexts = current = &ctx;
    unsetenv("VAV4L2_CAPTURE_DMABUF");
    expect(vav4l2_ensure_capture(&ctx, 16, 16, V4L2_PIX_FMT_NV12) == 0 && ctx.capture_memory == V4L2_MEMORY_DMABUF &&
        ctx.capture_bufs_allocd == 4 && !queries, "default-capture-uses-compatible-declared-pool-without-mmap");
    ctx.output_bufs_allocd = 0;
    setenv("VAV4L2_CAPTURE_DMABUF", "1", 1);
    expect(vav4l2_ensure_capture(&ctx, 16, 16, V4L2_PIX_FMT_NV15) == 0, "prepare-dynamic-target-queue-transition");
    ctx.render_targets = NULL; ctx.num_render_targets = 0;
    setenv("VAV4L2_CAPTURE_DMABUF", "required", 1);
    expect(vav4l2_ensure_capture(&ctx, 16, 16, V4L2_PIX_FMT_NV12) == 0 && ctx.capture_memory == V4L2_MEMORY_DMABUF,
        "dynamic-pool-negotiates-from-first-current-target-without-initial-target-array");
    ctx.output_bufs_allocd = 1; ctx.output_buf_size = ctx.output_plane_len[0] = 64;
    int index = vav4l2_decode_submit(&ctx, 0, 16, 1000);
    expect(index >= 0 && !queries && !s[0].cpu_ptr && vav4l2_surface_pull_capture(&ctx, &s[0], index) == 0 &&
        s[0].prime_capture_valid && s[0].capture_ctx == &ctx && s[0].cap_view == NULL, "decode-completes-into-client-object-without-cpu-staging");
    expect(vav4l2_prime_read(&s[0]) == 0 && ((unsigned char *)s[0].cpu_ptr)[0] == decoded &&
        vav4l2_validate_reference(&ctx, 1) == VA_STATUS_SUCCESS, "cpu-readback-keeps-direct-dpb-reference-valid");
    ctx.current_surface = &s[2]; int before = out_queues;
    expect(vav4l2_decode_submit(&ctx, 0, 16, 2000) < 0 && out_queues == before,
        "different-va-id-cannot-overwrite-an-aliased-live-dpb-object");
    ctx.current_surface = &s[1]; fault = 2;
    expect(vav4l2_decode_submit(&ctx, 0, 16, 3000) == -2 && vav4l2_prime_read(&s[1]) < 0,
        "corrupt-direct-decode-does-not-expose-old-cpu-content");
    fault = 0; decoded = 0x6a;
    index = vav4l2_decode_submit(&ctx, 0, 16, 4000);
    expect(index >= 0 && vav4l2_surface_pull_capture(&ctx, &s[1], index) == 0 && vav4l2_prime_read(&s[1]) == 0 &&
        ((unsigned char *)s[1].cpu_ptr)[0] == decoded, "full-direct-decode-can-recover-dirty-client-storage");
    for (fault = 3; fault <= 6; fault++) { int error; expect(vav4l2_capture_dma_dequeue(&ctx, &error) < 0, "direct-completion-rejects-bad-payload-memory-offset-and-index"); }
    fault = 0;
    VADRMPRIMESurfaceDescriptor retained = {0};
    expect(vav4l2_prime_export(&s[1], 0, &retained) == VA_STATUS_SUCCESS, "direct-import-can-be-reexported");
    ctx.output_bufs_allocd = 0;
    setenv("VAV4L2_CAPTURE_DMABUF", "1", 1);
    expect(vav4l2_ensure_capture(&ctx, 16, 16, V4L2_PIX_FMT_NV15) == 0 && !ctx.capture_memory && freed_dma &&
        vav4l2_validate_reference(&ctx, 1) != VA_STATUS_SUCCESS, "format-change-releases-dma-queue-and-invalidates-old-dpb");
    expect(vav4l2_ensure_capture(&ctx, 16, 16, V4L2_PIX_FMT_NV12) == 0 && ctx.capture_memory == V4L2_MEMORY_DMABUF && freed_mmap,
        "capture-can-reenter-dma-mode-after-native-format-transition");
    expect(vav4l2_ensure_capture(&ctx, 16, 16, V4L2_PIX_FMT_NV15) == 0, "prepare-native-capture-for-allocation-rejection");
    fault = 1;
    expect(vav4l2_ensure_capture(&ctx, 16, 16, V4L2_PIX_FMT_NV12) == 0 && !ctx.capture_memory,
        "unsupported-dma-reqbufs-falls-back-before-any-decode-job");
    fault = 0;
    expect(vav4l2_ensure_capture(&ctx, 16, 16, V4L2_PIX_FMT_NV15) == 0, "prepare-memory-pressure-fallback");
    fault = 7; unsetenv("VAV4L2_CAPTURE_DMABUF");
    ctx.render_targets = targets; ctx.num_render_targets = 2;
    expect(vav4l2_ensure_capture(&ctx, 16, 16, V4L2_PIX_FMT_NV12) == 0 && !ctx.capture_memory,
        "dma-import-allocation-pressure-preserves-native-decode-by-default");
    fault = 0;
    expect(vav4l2_ensure_capture(&ctx, 16, 16, V4L2_PIX_FMT_NV15) == 0, "prepare-no-import-support");
    fault = 8;
    expect(vav4l2_ensure_capture(&ctx, 16, 16, V4L2_PIX_FMT_NV12) == 0 && !ctx.capture_memory,
        "driver-rejecting-all-dmabuf-ioctls-still-decodes-through-mmap");
    fault = 0; ctx.render_targets = NULL; ctx.num_render_targets = 0;
    expect(vav4l2_capture_dma_setup(&ctx, 16, 16, V4L2_PIX_FMT_NV12, 4) == 0,
        "auto-mode-keeps-private-dpb-for-unknown-future-target-layouts");
    fault = 0; setenv("VAV4L2_CAPTURE_DMABUF", "required", 1);
    expect(vav4l2_ensure_capture(&ctx, 32, 32, V4L2_PIX_FMT_NV12) < 0 && !ctx.capture_bufs_allocd,
        "required-dma-rejects-pool-layout-that-cannot-hold-new-resolution");
    setenv("VAV4L2_CAPTURE_DMABUF", "1", 1);
    expect(vav4l2_ensure_capture(&ctx, 32, 32, V4L2_PIX_FMT_NV12) == 0 && !ctx.capture_memory,
        "preferred-dma-uses-native-capture-after-incompatible-resolution-change");
    vav4l2_release_context_device(&ctx);
    expect(freed_dma && freed_mmap && fcntl(owner[0], F_GETFD) >= 0 && fcntl(retained.objects[0].fd, F_GETFD) >= 0 &&
        vav4l2_prime_read(&s[1]) == 0, "context-release-uses-dma-memory-and-preserves-surface-and-client-fds");
    close(retained.objects[0].fd);
    for (unsigned i = 0; i < 3; i++) { vav4l2_prime_destroy(&s[i]); free(s[i].cpu_ptr); }
    close(owner[0]); close(owner[1]); unsetenv("VAV4L2_CAPTURE_DMABUF"); vav4l2_set_ioctl_hook(NULL);
    expect(cap_queues >= 3, "success-and-error-jobs-used-direct-capture-queues");
    printf("capture-dmabuf: %s\n", failures ? "FAIL" : "PASS"); return failures != 0;
}
