/* Real retained PRIME descriptors, fake device DMA, independent byte oracle. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <linux/dma-buf.h>
#include <drm_fourcc.h>
#include "va_request.h"
static int failures, fault, qcount, syncs, releases, dma_output, dma_capture, mmap_output, mmap_capture, poll_count, format_calls, stream_on, stream_off, selections, allocation_calls;
static int out_fd = -1, cap_fd = -1, cap_fds[2] = {-1, -1};
static struct vareq_context *current;
static void expect(int value, const char *tag) { if (!value) { fprintf(stderr, "FAIL %s fault=%d\n", tag, fault); failures++; } }
int __wrap_poll(struct pollfd *p, nfds_t n, int timeout)
{
    expect(n == 1 && timeout == 2000, "bounded-fence-and-queue-wait");
    poll_count++;
    if (fault == 7 && p->fd == current->v4l2_fd) return 0;
    p->revents = p->events; return 1;
}
static int mock_ioctl(int fd, unsigned long req, void *arg)
{
    (void)fd;
    if (req == DMA_BUF_IOCTL_SYNC) { syncs++; return 0; }
    if (req == VIDIOC_TRY_FMT) return 0;
    if (req == VIDIOC_S_FMT) {
        expect(!current->vpp_q.streaming, "format-negotiation-has-idle-streams");
        format_calls++;
        struct v4l2_format *f = arg;
        if (!f->fmt.pix_mp.plane_fmt[0].bytesperline) f->fmt.pix_mp.plane_fmt[0].bytesperline = 8;
        if (fault == 3) f->fmt.pix_mp.plane_fmt[0].bytesperline += 8;
        if (f->fmt.pix_mp.num_planes == 2) {
            f->fmt.pix_mp.plane_fmt[0].sizeimage = f->fmt.pix_mp.plane_fmt[0].bytesperline * 4;
            f->fmt.pix_mp.plane_fmt[1].sizeimage = f->fmt.pix_mp.plane_fmt[1].bytesperline * 2;
        } else f->fmt.pix_mp.plane_fmt[0].sizeimage = f->fmt.pix_mp.plane_fmt[0].bytesperline * 6;
        return 0;
    }
    if (req == VIDIOC_S_SELECTION) {
        expect(!current->vpp_q.streaming, "selection-changes-stop-both-streams-first");
        selections++; return 0;
    }
    if (req == VIDIOC_REQBUFS) {
        struct v4l2_requestbuffers *r = arg;
        if (!r->count) {
            releases++;
            if (r->memory == V4L2_MEMORY_DMABUF) {
                if (r->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) dma_output++;
                else dma_capture++;
            }
            return 0;
        }
        allocation_calls++;
        if (r->memory == V4L2_MEMORY_DMABUF && fault == 1) { errno = EINVAL; return -1; }
        if (r->memory == V4L2_MEMORY_DMABUF && fault == 2) r->count = 0;
        return 0;
    }
    if (req == VIDIOC_QUERYBUF) {
        struct v4l2_buffer *b = arg;
        b->m.planes[0].length = 48;
        b->m.planes[0].m.mem_offset = b->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE ? 4096 : 0;
        return 0;
    }
    if (req == VIDIOC_QBUF) {
        struct v4l2_buffer *b = arg;
        int capture = b->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        if (b->memory == V4L2_MEMORY_DMABUF && (current->vpp_behaviors & VAREQ_COMPAT_STREAM_BEFORE_QUEUE))
            expect(current->vpp_q.streaming == 3, "rga2-cold-and-restarted-streams-are-armed-before-qbuf");
        qcount++;
        if (!strcmp(current->device_path, "mock-dma") && ((fault == 19 && capture) || (fault == 20 && !capture))) { errno = EINVAL; return -1; }
        if (b->memory == V4L2_MEMORY_MMAP) {
            if (capture) mmap_capture++;
            else { mmap_output++; memcpy(current->vpp_q.cap_map, current->vpp_q.out_map[0], 48); }
            return 0;
        }
        expect(b->memory == V4L2_MEMORY_DMABUF && b->length >= 1 && b->length <= 2 && !b->index,
            "queue-has-negotiated-plane-count-and-memory");
        if ((capture && fault == 4) || (!capture && fault == 5)) { errno = EIO; return -1; }
        for (unsigned p = 0; p < b->length; p++) {
            expect(b->m.planes[p].length == (fault == 21 || fault == 23 ? 8192u : 4096u) && (fcntl(b->m.planes[p].m.fd, F_GETFD) & FD_CLOEXEC), "queue-borrows-owned-cloexec-fd-and-full-object-length");
            if (capture) { cap_fds[p] = b->m.planes[p].m.fd; cap_fd = cap_fds[0]; expect(!b->m.planes[p].bytesused, "capture-has-no-input-payload"); }
            else {
                out_fd = b->m.planes[p].m.fd;
                unsigned char data[48];
                unsigned bytes = b->length == 1 ? 48 : p ? 16 : 32;
                expect(b->m.planes[p].bytesused == bytes && pread(out_fd, data, bytes, 0) == bytes &&
                    pwrite(cap_fds[p], data, bytes, 0) == bytes, "mock-dma-copies-each-fd-plane-without-cpu-staging");
            }
        }
        return 0;
    }
    if (req == VIDIOC_STREAMON) stream_on++;
    if (req == VIDIOC_STREAMOFF) stream_off++;
    if (req == VIDIOC_STREAMON && (fault == 6 || fault == 31 || (fault == 32 && *(enum v4l2_buf_type *)arg == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE))) { errno = EIO; return -1; }
    if (req == VIDIOC_STREAMOFF && fault == 14) { errno = EIO; return -1; }
    if (req == VIDIOC_DQBUF) {
        struct v4l2_buffer *b = arg;
        if (b->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
            if (fault == 13) { b->flags = V4L2_BUF_FLAG_ERROR; }
            return 0;
        }
        for (unsigned p = 0; p < b->length; p++) b->m.planes[p].bytesused = b->length == 1 ? 48 : p ? 16 : 32;
        if (fault == 8) b->flags = V4L2_BUF_FLAG_ERROR;
        if (fault == 9) b->index = 1;
        if (fault == 10) b->m.planes[0].data_offset = 1;
        if (fault == 11) b->m.planes[0].bytesused = 47;
        if (fault == 12) b->m.planes[0].bytesused = 4097;
        return 0;
    }
    return 0;
}
static void imported(struct vareq_surface *s, int fd, int uv_fd)
{
    *s = (struct vareq_surface){.width = 4, .height = 4, .format = VA_FOURCC_NV12,
        .memfd_fd = -1, .buf_index = -1, .import_fd = -1, .gbm_external_fd = -1, .cpu_stride = 8, .cpu_size = 48};
    VADRMPRIMESurfaceDescriptor d = {.fourcc = VA_FOURCC_NV12, .width = 4, .height = 4, .num_objects = 1, .num_layers = 1};
    d.objects[0].fd = fd; d.objects[0].size = fault == 21 || fault == 23 ? 8192 : 4096;
    d.layers[0].drm_format = DRM_FORMAT_NV12; d.layers[0].num_planes = 2;
    d.layers[0].pitch[0] = d.layers[0].pitch[1] = 8; d.layers[0].offset[1] = 32;
    if (uv_fd >= 0) {
        d.num_objects = 2; d.objects[1] = d.objects[0]; d.objects[1].fd = uv_fd;
        d.layers[0].object_index[1] = 1; d.layers[0].offset[1] = 0;
    }
    expect(vareq_prime_import(s, &d) == VA_STATUS_SUCCESS, "import-canonical-linear-fd");
}
static unsigned region_mode;
static VAStatus run(struct vareq_context *ctx)
{
    VARectangle region = {0, 0, 2, 2};
    VAProcPipelineParameterBuffer pipe = {.surface = 1, .surface_region = region_mode ? &region : NULL,
        .output_region = region_mode ? &region : NULL};
    struct vareq_buffer parameter = {.type = VAProcPipelineParameterBufferType, .size = sizeof(pipe), .data = &pipe};
    struct vareq_buffer *buffers[] = {&parameter}; current = ctx;
    return vareq_vpp_run(ctx, buffers, 1);
}
int main(void)
{
    struct vareq_driver_data dd = {.vpp_devices_scanned = 1, .num_vpp_devices = 1};
    dd.vpp_devices[0].num_input = dd.vpp_devices[0].num_output = 2;
    dd.vpp_devices[0].input[1] = dd.vpp_devices[0].output[1] = V4L2_PIX_FMT_NV12M;
    dd.vpp_devices[0].input[0] = dd.vpp_devices[0].output[0] = V4L2_PIX_FMT_NV12;
    strcpy(dd.vpp_devices[0].identity.driver, "rockchip-rga"); strcpy(dd.vpp_devices[0].path, "mock-dma"); strcpy(dd.vpp_devices[0].identity.card, "rga3");
    int owners[] = {memfd_create("dma-input", MFD_CLOEXEC), memfd_create("dma-output", MFD_CLOEXEC), memfd_create("dma-input-uv", MFD_CLOEXEC), memfd_create("dma-output-uv", MFD_CLOEXEC)};
    if (owners[0] < 0 || owners[1] < 0 || ftruncate(owners[0], 4096) || ftruncate(owners[1], 4096)) return 1;
    if (owners[2] < 0 || owners[3] < 0 || ftruncate(owners[2], 4096) || ftruncate(owners[3], 4096)) return 1;
    vareq_set_ioctl_hook(mock_ioctl);
    setenv("VAREQ_VPP_DMABUF", "required", 1);
    char alternative[] = "vpp-dma-alternative.XXXXXX"; int alternative_fd = mkstemp(alternative);
    if (alternative_fd < 0 || ftruncate(alternative_fd, 8192)) return 1;
    dd.vpp_devices[1] = dd.vpp_devices[0]; strcpy(dd.vpp_devices[1].path, alternative);
    for (unsigned scenario = 0; scenario < 33; scenario++) {
        dd.num_vpp_devices = (scenario >= 19 && scenario <= 22) || (scenario >= 28 && scenario <= 30) ? 2 : 1;
        strcpy(dd.vpp_devices[0].identity.driver, "rockchip-rga"); strcpy(dd.vpp_devices[0].path, "mock-dma");
        strcpy(dd.vpp_devices[0].identity.card, scenario >= 21 ? "rga2" : "rga3");
        expect(!ftruncate(owners[0], scenario == 21 || scenario == 23 ? 8192 : 4096) &&
               !ftruncate(owners[1], scenario == 21 || scenario == 23 ? 8192 : 4096), "object-span-fixture");
        fault = 0; qcount = syncs = poll_count = format_calls = 0;
        stream_on = stream_off = selections = allocation_calls = region_mode = 0;
        unsetenv("VAREQ_VPP_DMABUF_SESSION");
        if (scenario == 26) setenv("VAREQ_VPP_DMABUF_SESSION", "reconfigure", 1);
        if (scenario == 27) {
            setenv("VAREQ_VPP_DMABUF_SESSION", "isolated", 1);
            strcpy(dd.vpp_devices[0].path, alternative);
        }
        if (scenario == 21 || scenario == 23) fault = scenario;
        struct vareq_surface src, dst;
        imported(&src, owners[0], scenario == 18 || scenario == 22 ? owners[2] : -1); imported(&dst, scenario == 15 ? owners[0] : owners[1], scenario == 18 || scenario == 22 ? owners[3] : -1);
        dd.surfaces[1] = &src;
        struct vareq_context ctx = {.codec = VAREQ_CODEC_VPP, .driver_data = &dd,
            .device_path = strdup(dd.vpp_devices[0].path), .current_surface = &dst, .v4l2_fd = memfd_create("mock-node", MFD_CLOEXEC)};
        if (ctx.v4l2_fd < 0 || ftruncate(ctx.v4l2_fd, 8192)) return 1;
        current = &ctx; syncs = 0; fault = scenario < 15 || scenario >= 19 ? scenario : 0;
        if (scenario == 16) { unsetenv("VAREQ_VPP_DMABUF"); fault = 1; }
        if (scenario == 17) setenv("VAREQ_VPP_DMABUF", "0", 1);
        unsigned char data[48], got[48]; memset(data, 0x3c, sizeof(data));
        expect(pwrite(owners[0], data, sizeof(data), 0) == sizeof(data), "write-external-producer");
        if (scenario == 18 || scenario == 22) expect(pwrite(owners[2], data, 16, 0) == 16, "write-external-chroma-producer");
        int ok = scenario == 0 || (scenario >= 16 && scenario != 23 && scenario < 31);
        expect((run(&ctx) == VA_STATUS_SUCCESS) == ok, "completion-or-injected-error-is-truthful");
        if (scenario == 0 || scenario == 24) {
            expect(!src.cpu_ptr && !dst.cpu_ptr && !syncs && poll_count >= 3, "device-dma-never-enters-cpu-cache-access-or-pixel-copy");
            expect(ctx.vpp_q.out_memory == V4L2_MEMORY_DMABUF && ctx.vpp_q.cap_memory == V4L2_MEMORY_DMABUF &&
                dst.last_writer == VAREQ_WRITER_BO, "successful-queue-transport-is-explicit");
            expect(fcntl(out_fd, F_GETFD) < 0 && fcntl(cap_fd, F_GETFD) < 0, "completed-job-closes-temporary-owned-fds");
            for (unsigned frame = 0; frame < 128; frame++) {
                memset(data, frame, sizeof(data));
                expect(pwrite(owners[0], data, sizeof(data), 0) == sizeof(data) && run(&ctx) == VA_STATUS_SUCCESS &&
                    pread(owners[1], got, sizeof(got), 0) == sizeof(got) && !memcmp(data, got, sizeof(data)), "repeated-external-producer-and-device-consumer-retain-current-bytes");
            }
            if (scenario == 24) expect(stream_on == 2 && !stream_off && ctx.vpp_q.streaming == 3 && allocation_calls == 2,
                "rga2-repeated-jobs-retain-active-stream-and-dequeued-objects");
            expect(!src.cpu_ptr && !dst.cpu_ptr && !syncs && format_calls == 2, "reused-dma-jobs-keep-queue-attachments-and-remain-copy-free");
            expect(vareq_prime_read(&dst) == 0 && ((unsigned char *)dst.cpu_ptr)[0] == 127, "first-cpu-reader-refreshes-device-result");
            dd.surfaces[1] = &dst; ctx.current_surface = &src; memset(data, 0x73, sizeof(data));
            expect(pwrite(owners[1], data, sizeof(data), 0) == sizeof(data) && run(&ctx) == VA_STATUS_SUCCESS &&
                pread(owners[0], got, sizeof(got), 0) == sizeof(got) && !memcmp(data, got, sizeof(data)) && format_calls == 2,
                "warm-queue-rebinds-different-source-and-target-objects-without-stale-fds");
            dd.surfaces[1] = &src; ctx.current_surface = &dst;
            setenv("VAREQ_VPP_DMABUF", "0", 1);
            expect(run(&ctx) == VA_STATUS_SUCCESS && mmap_output && mmap_capture, "warm-dma-to-mmap-transition-reconfigures-memory-type");
        } else if (scenario <= 3 || scenario == 15) expect(!qcount, "fallback-and-alias-rejection-happen-before-queueing");
        else if (scenario >= 5 && scenario <= 14) expect(vareq_prime_read(&dst) < 0, "incomplete-device-write-cannot-expose-old-cpu-pixels");
        if (scenario == 25) {
            region_mode = 1;
            expect(run(&ctx) == VA_STATUS_SUCCESS && selections == 2 && ctx.vpp_q.streaming == 3,
                "rga2-live-session-stops-for-partial-selection-and-restarts");
            region_mode = 0;
            expect(run(&ctx) == VA_STATUS_SUCCESS && selections == 4 && ctx.vpp_q.streaming == 3,
                "rga2-partial-to-full-reset-happens-before-requeue");
            fault = 14;
            region_mode = 1;
            expect(run(&ctx) != VA_STATUS_SUCCESS && ctx.v4l2_fd < 0 && vareq_prime_read(&dst) == 0,
                "pre-submit-selection-stop-failure-closes-active-device-without-invalidating-completed-pixels");
            fault = 0; region_mode = 0;
        }
        if (scenario == 26 || scenario == 27) {
            expect(run(&ctx) == VA_STATUS_SUCCESS && format_calls == 4 && allocation_calls == 4 && !src.cpu_ptr && !dst.cpu_ptr,
                "diagnostic-sessions-repeat-real-dma-and-reconfigure-without-staging");
        }
        if (scenario >= 28 && scenario <= 30) {
            if (scenario == 28) {
                vareq_prime_destroy(&src); vareq_prime_destroy(&dst);
                expect(!ftruncate(owners[0], 8192) && !ftruncate(owners[1], 8192), "warm-object-growth-fixture");
                fault = 21; imported(&src, owners[0], -1); imported(&dst, owners[1], -1);
            } else fault = scenario == 29 ? 20 : 8;
            VAStatus st = run(&ctx);
            expect((st == VA_STATUS_SUCCESS) == (scenario != 30), "persistent-session-does-not-replay-a-started-failed-job");
            if (scenario == 30) expect(!strcmp(ctx.device_path, "mock-dma") && qcount == 4 && vareq_prime_read(&dst) < 0,
                "started-dma-completion-failure-invalidates-target-without-backend-retry");
            else expect(!strcmp(ctx.device_path, alternative) && qcount == (scenario == 28 ? 4 : 6) && !src.cpu_ptr && !dst.cpu_ptr,
                "idle-active-stream-can-retry-span-or-qbuf-rejection-after-cancelling-unstarted-job");
            fault = 0;
        }
        if (scenario >= 31) expect(!qcount && dst.has_pic && vareq_prime_read(&dst) == 0,
            "rga2-cold-streamon-failure-submits-no-buffer-and-preserves-previous-content");
        if (scenario >= 19 && scenario <= 22) expect(!strcmp(ctx.device_path, alternative) && !src.cpu_ptr && !dst.cpu_ptr &&
            ctx.vpp_q.out_memory == V4L2_MEMORY_DMABUF, "allocator-rejection-retries-another-node-after-cancelling-unstarted-queues");
        if (scenario >= 21 && scenario <= 23) expect(qcount == (scenario == 23 ? 0 : 2),
            "rga2-excess-object-pages-never-reach-qbuf-and-safe-backend-retry-is-copy-free");
        if (scenario == 14) expect(ctx.v4l2_fd < 0, "failed-active-streamoff-closes-device-before-owned-fds");
        if (scenario == 16) expect(ctx.v4l2_fd >= 0 && mmap_output && mmap_capture, "unsupported-reqbufs-dmabuf-safely-falls-back-on-same-open-node");
        if (scenario == 18 || scenario == 22) expect(ctx.vpp_q.out_planes == 2 && ctx.vpp_q.cfmt.fmt.pix_mp.num_planes == 2 &&
            pread(owners[3], got, 16, 0) == 16 && !memcmp(data, got, 16) && !src.cpu_ptr && !dst.cpu_ptr,
            "two-discontiguous-dma-objects-use-native-two-plane-queues");
        fault = 0;
        vareq_m2m_teardown(&ctx, &ctx.vpp_q);
        if (ctx.v4l2_fd >= 0) close(ctx.v4l2_fd);
        free(ctx.device_path);
        vareq_prime_destroy(&src); vareq_prime_destroy(&dst); free(src.cpu_ptr); free(dst.cpu_ptr);
        setenv("VAREQ_VPP_DMABUF", "required", 1);
    }
    expect(dma_output > 0 && dma_capture > 0 && releases > 0, "queue-teardown-uses-negotiated-memory-type");
    expect(fcntl(owners[0], F_GETFD) >= 0 && fcntl(owners[1], F_GETFD) >= 0, "driver-never-closes-client-object-owners");
    unsetenv("VAREQ_VPP_DMABUF_SESSION");
    close(alternative_fd); unlink(alternative);
    for (unsigned i = 0; i < 4; i++) close(owners[i]);
    vareq_set_ioctl_hook(NULL); unsetenv("VAREQ_VPP_DMABUF");
    printf("vpp-dmabuf: %s\n", failures ? "FAIL" : "PASS"); return failures != 0;
}
