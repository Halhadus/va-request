/* Queue-allocation negotiation and exported-FD lifetime; no device job. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include "va_request.h"
static int exact_enabled, exact_only, exact_fault, exact_fd[1024], exact_exports;
static unsigned exact_size;
static int owner, fault, failures, queried, allocated, exported, jobs;
static void expect(int value, const char *tag) { if (!value) { fprintf(stderr, "FAIL %s fault=%d\n", tag, fault); failures++; } }
int __wrap_open(const char *path, int flags, ...)
{
    expect(flags == (O_RDWR | O_NONBLOCK | O_CLOEXEC), "allocator-open-is-cloexec-and-nonblocking");
    int exact = !strcmp(path, "/dev/video8") && exact_enabled;
    if ((!exact && (strcmp(path, "/dev/video7") || exact_only)) || fault == 1) { errno = ENOENT; return -1; }
    int fd = fcntl(owner, F_DUPFD_CLOEXEC, 0);
    if (fd >= 0 && fd < 1024) exact_fd[fd] = exact;
    return fd;
}
int __wrap_open64(const char *path, int flags, ...) { return __wrap_open(path, flags); }
static int mock_ioctl(int fd, unsigned long request, void *arg)
{
    expect(fcntl(fd, F_GETFD) >= 0, "allocator-uses-live-device-fd");
    if (exact_fd[fd]) {
        if (request == VIDIOC_QUERYCAP) {
            struct v4l2_capability *c = arg;
            strcpy((char *)c->driver, "hantro-vpu"); strcpy((char *)c->card, "rockchip,rk3588-vepu121-enc");
            c->capabilities = V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING;
            if (exact_fault == 1) strcpy((char *)c->card, "other-encoder");
            return 0;
        }
        if (request == VIDIOC_S_FMT) {
            struct v4l2_format *f = arg;
            exact_size = f->fmt.pix_mp.plane_fmt[0].sizeimage;
            expect(f->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE && f->fmt.pix_mp.pixelformat == V4L2_PIX_FMT_JPEG && exact_size,
                "coded-allocator-requests-explicit-byte-span");
            if (exact_fault == 2) { errno = EIO; return -1; }
            if (exact_fault == 3) f->fmt.pix_mp.plane_fmt[0].sizeimage += 32768;
            if (exact_fault == 4) f->fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
            if (exact_fault == 5) f->fmt.pix_mp.num_planes = 2;
            return 0;
        }
        if (request == VIDIOC_QUERYBUF) {
            struct v4l2_buffer *b = arg; b->m.planes[0].length = (exact_size + 4095) & ~4095u; return 0;
        }
        if (request == VIDIOC_EXPBUF) {
            struct v4l2_exportbuffer *e = arg; exact_exports++;
            e->fd = memfd_create("exact-coherent-export", MFD_CLOEXEC);
            unsigned size = (exact_size + 4095) & ~4095u;
            expect(e->fd >= 0 && ftruncate(e->fd, size + (exact_fault == 6 ? 4096 : 0)) == 0, "exact-export-fixture");
            return 0;
        }
    }
    if (request == VIDIOC_QUERYCAP) {
        queried++; struct v4l2_capability *c = arg;
        strcpy((char *)c->driver, "rockchip-rga"); strcpy((char *)c->card, fault == 2 ? "other-rga" : "rga3");
        c->capabilities = V4L2_CAP_DEVICE_CAPS; c->device_caps = V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING;
        if (fault == 3) c->device_caps = 0;
        return 0;
    }
    if (request == VIDIOC_S_FMT) {
        struct v4l2_format *f = arg;
        expect(f->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE && f->fmt.pix_mp.width >= 128 && f->fmt.pix_mp.height >= 128 &&
            f->fmt.pix_mp.pixelformat == V4L2_PIX_FMT_NV12 && f->fmt.pix_mp.num_planes == 1, "allocator-negotiates-supported-flat-storage");
        if (fault == 4) { errno = EIO; return -1; }
        f->fmt.pix_mp.plane_fmt[0].sizeimage = fault == 5 ? 1 : 65536;
        if (fault == 6) f->fmt.pix_mp.num_planes = 2;
        return 0;
    }
    if (request == VIDIOC_REQBUFS) {
        struct v4l2_requestbuffers *r = arg; allocated++;
        expect(r->memory == V4L2_MEMORY_MMAP && r->count == 1 && !r->flags, "allocator-requests-default-coherent-mmap-storage");
        if (fault == 7) { errno = ENOMEM; return -1; }
        if (fault == 8) r->count = 0;
        if (fault == 13) r->flags = V4L2_MEMORY_FLAG_NON_COHERENT;
        return 0;
    }
    if (request == VIDIOC_QUERYBUF) {
        if (fault == 9) { errno = EIO; return -1; }
        struct v4l2_buffer *b = arg; b->m.planes[0].length = fault == 10 ? 1 : fault == 16 ? 32768 : 65536;
        if (fault == 14) b->memory = V4L2_MEMORY_DMABUF;
        if (fault == 15) b->index = 1;
        return 0;
    }
    if (request == VIDIOC_EXPBUF) {
        exported++; struct v4l2_exportbuffer *e = arg;
        expect(e->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE && !e->index && !e->plane && e->flags == (O_RDWR | O_CLOEXEC), "allocator-exports-only-owned-buffer-zero");
        if (fault == 11) { errno = EIO; return -1; }
        e->fd = fcntl(owner, F_DUPFD_CLOEXEC, 0);
        if (fault == 12) expect(ftruncate(e->fd, 1) == 0, "short-export-fixture");
        return 0;
    }
    jobs++; return -1;
}
int main(void)
{
    unsetenv("VAREQ_ALLOC_TRACE"); /* Mock discovery is never hardware evidence. */
    owner = memfd_create("coherent-export-fixture", MFD_CLOEXEC); if (owner < 0) return 2;
    vareq_set_ioctl_hook(mock_ioctl);
    for (fault = 0; fault <= 16; fault++) {
        if (ftruncate(owner, 65536)) return 2;
        vareq_allocator_cleanup(); int fd = vareq_alloc_shared_storage(64, 96);
        expect((fd >= 0) == !fault, "allocator-success-or-injected-failure");
        if (fd >= 0) {
            unsigned char byte = 0x69, got = 0;
            expect((fcntl(fd, F_GETFD) & FD_CLOEXEC) && pwrite(fd, &byte, 1, 0) == 1 && pread(owner, &got, 1, 0) == 1 && got == byte,
                "export-survives-temporary-queue-close-and-retains-actual-storage"); close(fd);
        }
        expect(fcntl(owner, F_GETFD) >= 0, "allocator-never-closes-client-owner");
    }
    fault = 0; exact_enabled = 1;
    for (exact_only = 0; exact_only <= 1; exact_only++) for (exact_fault = 0; exact_fault <= 6; exact_fault++) {
        vareq_allocator_cleanup();
        int fd = vareq_alloc_shared_storage(64, 96);
        expect((fd >= 0) == (!exact_only || !exact_fault), "exact-size-or-recognized-rga3-fallback");
        if (fd >= 0) {
            unsigned actual = 0;
            expect(vareq_export_fd_validate(fd, 6144, &actual) == 0 && actual == (exact_fault ? 65536u : 8192u),
                "small-image-storage-has-no-extra-imported-pages"); close(fd);
        }
    }
    exact_only = 0; exact_fault = 0; vareq_allocator_cleanup();
    int before = exact_exports, large = vareq_alloc_shared_storage(128, 192);
    expect(large >= 0 && exact_exports == before, "exact-rga3-geometry-keeps-proven-allocation-backend");
    if (large >= 0) close(large);
    expect(vareq_alloc_shared_storage(0, 1) < 0 && vareq_alloc_shared_storage(UINT32_MAX, 2) < 0, "allocator-rejects-zero-or-overflow-span");
    expect(queried && allocated && exported && !jobs, "allocation-never-queues-or-streams-a-device-job");
    close(owner); vareq_set_ioctl_hook(NULL); vareq_allocator_cleanup();
    printf("allocator: %s\n", failures ? "FAIL" : "PASS"); return failures != 0;
}

int __wrap_vareq_list_video_nodes(char (*paths)[64], unsigned capacity)
{
    unsigned count = capacity < 9 ? capacity : 9;
    for (unsigned i = 0; i < count; i++) snprintf(paths[i], 64, "/dev/video%u", i);
    return count;
}
