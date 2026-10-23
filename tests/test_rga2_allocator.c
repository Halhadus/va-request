/* Allocation/provider selection, exact page bounds and FD ownership. Real
 * cache coherency is covered by the hardware transport/decoder/renderer runs. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include "va_request.h"
enum { BAD_CAP = 1, WRONG_CARD, WRONG_DRIVER, BAD_FORMAT, SHORT_IMAGE, MANY_PLANES,
    REQ_FAIL, ZERO_COUNT, NON_COHERENT, QUERY_FAIL, SHORT_QUERY, BAD_MEMORY,
    BAD_INDEX, EXP_FAIL, SHORT_OBJECT, EXCESS_OBJECT, NO_CLOEXEC, FORMAT_FAIL };
static int owner, failures, fault, rga2 = 1, rga3 = 1, alias_card, force_coherent;
static int tags[1024], object_size[1024], formats[1024], exporters[1024], exported[128], num_exported;
static int contexts[128], num_contexts;
static unsigned request_size, page_size, requests, source_exports[3], opens, jobs;
static void expect(int ok, const char *message)
{
    if (!ok) { fprintf(stderr, "FAIL rga2 allocator: %s fault=%d\n", message, fault); failures++; }
}
int __wrap_open(const char *path, int flags, ...)
{
    opens++;
    expect(flags == (O_RDWR | O_NONBLOCK | O_CLOEXEC), "nonblocking CLOEXEC device open");
    int tag = !strcmp(path, "/dev/video0") && rga2 ? 1 : !strcmp(path, "/dev/video1") && rga3 ? 2 : 0;
    if (!tag) { errno = ENOENT; return -1; }
    int fd = fcntl(owner, F_DUPFD_CLOEXEC, 0);
    if (fd >= 0 && fd < 1024) {
        tags[fd] = tag; expect(num_contexts < 128, "bounded context records"); contexts[num_contexts++] = fd;
    }
    else { if (fd >= 0) close(fd); errno = EMFILE; return -1; }
    return fd;
}
int __wrap_open64(const char *path, int flags, ...) { return __wrap_open(path, flags); }
static int mock_ioctl(int fd, unsigned long request, void *arg)
{
    expect(fd >= 0 && fd < 1024 && fcntl(fd, F_GETFD) >= 0, "live exporter context");
    int tag = tags[fd]; expect(tag == 1 || tag == 2, "recognized mock node");
    if (request == VIDIOC_QUERYCAP) {
        struct v4l2_capability *cap = arg;
        strcpy((char *)cap->driver, tag == 1 && fault == WRONG_DRIVER ? "other-driver" : "rockchip-rga");
        strcpy((char *)cap->card, tag == 1 ? fault == WRONG_CARD ? "other-rga" : alias_card ? "rockchip-rga" : "rga2" : "rga3");
        cap->capabilities = V4L2_CAP_DEVICE_CAPS;
        cap->device_caps = V4L2_CAP_STREAMING | V4L2_CAP_VIDEO_M2M_MPLANE;
        if (tag == 1 && fault == BAD_CAP) cap->device_caps = V4L2_CAP_STREAMING;
        return 0;
    }
    if (request == VIDIOC_S_FMT) {
        struct v4l2_format *f = arg; struct v4l2_pix_format_mplane *p = &f->fmt.pix_mp;
        expect(f->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE && p->num_planes == 1 && p->field == V4L2_FIELD_NONE,
               "CAPTURE allocation preserves exporter CPU invalidation direction");
        if (tag == 1 && fault == FORMAT_FAIL) { errno = EINVAL; return -1; }
        expect(p->width >= (tag == 1 ? 34u : 128u) && p->height >= (tag == 1 ? 34u : 128u) &&
               p->width <= 8192 && p->height <= 8192, "bounded allocation geometry");
        /* Match the target menu: unsupported packed YUYV is clamped to RGB. */
        if (tag == 1 && p->pixelformat == V4L2_PIX_FMT_YUYV) p->pixelformat = V4L2_PIX_FMT_ARGB32;
        unsigned bytes = (p->pixelformat == V4L2_PIX_FMT_XBGR32 || p->pixelformat == V4L2_PIX_FMT_ARGB32) ? 4 : p->pixelformat == V4L2_PIX_FMT_YUYV ? 2 : 1;
        formats[fd] = p->pixelformat;
        p->plane_fmt[0].bytesperline = p->width * bytes;
        p->plane_fmt[0].sizeimage = p->width * p->height * (bytes == 1 ? 3 : bytes * 2) / 2;
        if (tag == 1 && fault == BAD_FORMAT) p->pixelformat = V4L2_PIX_FMT_GREY;
        if (tag == 1 && fault == SHORT_IMAGE) p->plane_fmt[0].sizeimage = 1;
        if (tag == 1 && fault == MANY_PLANES) p->num_planes = 2;
        object_size[fd] = (p->plane_fmt[0].sizeimage + page_size - 1) / page_size * page_size;
        return 0;
    }
    if (request == VIDIOC_REQBUFS) {
        struct v4l2_requestbuffers *req = arg; requests++;
        expect(req->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE && req->memory == V4L2_MEMORY_MMAP && req->count == 1 && !req->flags,
               "one owned MMAP allocation, default cache policy");
        if (tag == 1 && fault == REQ_FAIL) { errno = ENOMEM; return -1; }
        if (tag == 1 && fault == ZERO_COUNT) req->count = 0;
        if (tag == 1 && fault == NON_COHERENT) req->flags = V4L2_MEMORY_FLAG_NON_COHERENT;
        return 0;
    }
    if (request == VIDIOC_QUERYBUF) {
        struct v4l2_buffer *b = arg;
        if (tag == 1 && fault == QUERY_FAIL) { errno = EIO; return -1; }
        b->m.planes[0].length = tag == 1 && fault == SHORT_QUERY ? 1 : (unsigned)object_size[fd];
        if (tag == 1 && fault == BAD_MEMORY) b->memory = V4L2_MEMORY_DMABUF;
        if (tag == 1 && fault == BAD_INDEX) b->index = 1;
        return 0;
    }
    if (request == VIDIOC_EXPBUF) {
        struct v4l2_exportbuffer *e = arg;
        expect(e->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE && !e->index && !e->plane && e->flags == (O_RDWR | O_CLOEXEC),
               "owned CAPTURE plane0 export is RW CLOEXEC");
        if (tag == 1 && fault == EXP_FAIL) { errno = EIO; return -1; }
        e->fd = memfd_create("rga-storage-export", MFD_CLOEXEC);
        unsigned size = tag == 1 && fault == SHORT_OBJECT ? 1 : (unsigned)object_size[fd];
        if (tag == 1 && fault == EXCESS_OBJECT) size += page_size;
        expect(e->fd >= 0 && ftruncate(e->fd, size) == 0, "export fixture storage");
        expect(e->fd < 1024, "bounded exported FD record"); if (e->fd >= 0 && e->fd < 1024) exporters[e->fd] = tag;
        if (tag == 1 && fault == NO_CLOEXEC) expect(fcntl(e->fd, F_SETFD, 0) == 0, "missing CLOEXEC injection");
        expect(num_exported < 128, "bounded export records");
        exported[num_exported++] = e->fd; source_exports[tag]++;
        return 0;
    }
    jobs++; errno = ENOTTY; return -1;
}
static void exercise(unsigned bytes, unsigned rows, int provider)
{
    request_size = bytes * rows; num_exported = num_contexts = 0; memset(source_exports, 0, sizeof(source_exports));
    vareq_allocator_cleanup();
    int fd = force_coherent ? vareq_alloc_coherent_storage(bytes, rows) : vareq_alloc_shared_storage(bytes, rows);
    expect((fd >= 0) == !!provider, "successful expected provider or truthful allocation failure");
    if (fd >= 0) {
        expect(fd < 1024 && exporters[fd] == provider, "returned FD belongs to the expected provider");
        uint32_t size = 0;
        expect(vareq_export_fd_validate(fd, request_size, &size) == 0, "actual FD span validated");
        if (provider == 1) expect(size <= (request_size + page_size - 1) / page_size * page_size && source_exports[1],
                                  "RGA2 storage adds no internal MMU pages");
        else expect(source_exports[2], "coherent provider selected on skip/rejection");
        unsigned char value = 0x9b, got = 0;
        expect((fcntl(fd, F_GETFD) & FD_CLOEXEC) && pwrite(fd, &value, 1, 0) == 1 && pread(fd, &got, 1, 0) == 1 && got == value,
               "actual exported storage survives temporary context close");
        close(fd);
    }
    for (int i = 0; i < num_exported; i++) expect(fcntl(exported[i], F_GETFD) < 0 && errno == EBADF, "accepted/rejected export references released");
    for (int i = 0; i < num_contexts; i++) expect(fcntl(contexts[i], F_GETFD) < 0 && errno == EBADF, "discovery and allocation contexts closed");
    expect(fcntl(owner, F_GETFD) >= 0 && !jobs, "client owner alive; no queue/stream/control submission");
}
int main(void)
{
    unsetenv("VAREQ_ALLOC_TRACE"); unsetenv("VAREQ_SHARED_STORAGE");
    page_size = sysconf(_SC_PAGESIZE);
    if (!page_size || page_size > 65536) return 2;
    owner = memfd_create("rga-allocator-owner", MFD_CLOEXEC); if (owner < 0) return 2;
    vareq_set_ioctl_hook(mock_ioctl);
    unsigned scenarios = 0;
    const unsigned shapes[][2] = {{64, 96}, {256, 64}, {128, 128}, {128, 256}, {64, 512}, {128, 257}, {64, 20000}, {128, 192}, {512, 128}, {1920, 1620}, {64, 6}};
    for (unsigned i = 0; i < sizeof(shapes) / sizeof(shapes[0]); i++) { exercise(shapes[i][0], shapes[i][1], 1); scenarios++; }
    alias_card = 1; exercise(64, 96, 1); alias_card = 0; scenarios++;
    for (fault = BAD_CAP; fault <= FORMAT_FAIL; fault++) {
        exercise(64, 96, 2); rga3 = 0; exercise(64, 96, 0); rga3 = 1; scenarios += 2;
    }
    fault = 0; rga2 = 0; exercise(64, 96, 2); rga2 = 1; scenarios++;
    setenv("VAREQ_SHARED_STORAGE", "coherent", 1); exercise(64, 96, 2); scenarios++;
    unsetenv("VAREQ_SHARED_STORAGE"); exercise(64, 96, 1); scenarios++;
    expect(requests && source_exports[1], "RGA2 MMAP EXPBUF allocation actually exercised");
    force_coherent = 1; exercise(64, 96, 2); scenarios++;
    expect(!getenv("VAREQ_SHARED_STORAGE"), "per-call-coherent-policy-does-not-mutate-process-environment");
    force_coherent = 0;
    unsigned before = opens;
    expect(vareq_alloc_shared_storage(0, 1) < 0 && vareq_alloc_shared_storage(UINT32_MAX, 2) < 0 && opens == before,
           "zero/overflow allocation never opens nodes");
    close(owner); vareq_set_ioctl_hook(NULL); vareq_allocator_cleanup();
    printf("RGA2 allocator: %s (%u provider/geometry/fault scenarios)\n", failures ? "FAIL" : "PASS", scenarios);
    return failures != 0;
}

int __wrap_vareq_list_video_nodes(char (*paths)[64], unsigned capacity)
{
    unsigned count = capacity < 8 ? capacity : 8;
    for (unsigned i = 0; i < count; i++) snprintf(paths[i], 64, "/dev/video%u", i);
    return count;
}
