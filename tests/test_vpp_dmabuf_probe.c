/* Fault-injection model: a queue can report DONE without writing anything.
 * The probe must reject that result in every mixed/direct transport mode. */
#undef NDEBUG /* Meson release builds must still execute these checks. */
#include <assert.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <gbm.h>
#include "va_v4l2.h"
#include "page_size.h"
static int fake_open(const char *, int, ...);
static int fake_close(int);
static int fake_ioctl(int, unsigned long, ...);
static void *fake_mmap(void *, size_t, int, int, int, off_t);
static int fake_munmap(void *, size_t);
static int fake_sync(int, uint64_t);
static int fake_wait(int, int);
static int fake_poll(struct pollfd *, nfds_t, int);
static int fake_alloc(unsigned, unsigned);
static int fake_validate(int, uint64_t, uint32_t *);
#define open fake_open
#define close fake_close
#define ioctl fake_ioctl
#define mmap fake_mmap
#define munmap fake_munmap
#define vav4l2_dmabuf_sync fake_sync
#define vav4l2_dmabuf_wait fake_wait
#define vav4l2_poll_intr fake_poll
#define vav4l2_alloc_shared_storage fake_alloc
#define vav4l2_export_fd_validate fake_validate
#define main hardware_probe_main
#include "vpp_dmabuf_probe.c"
#undef main

enum fault { NONE, NO_WRITE, BAD_SOURCE, BAD_INDEX, BAD_MEMORY, BAD_FLAGS,
    TRUNCATED, OUTPUT_QBUF, CAPTURE_STREAM, TIMEOUT, MMAP_FAIL, SYNC_FAIL,
    OVERSIZED, CLAMPED, STREAMOFF_FAIL, EXPORT_SECOND_FAIL };
static enum fault injected;
static unsigned char pixels[2][65536];
static unsigned image_size, active, queued, memory_type[2], cpu_sync[2], mapped[2];
static unsigned allocated, consumer_open, exporter_open, live[2], closes, dma_reads, produced;
static int fake_open(const char *name, int flags, ...)
{
    (void)name; (void)flags;
    if (!consumer_open) { consumer_open = 1; return 100; }
    exporter_open = 1; return 101;
}
static int fake_close(int fd)
{
    if (fd == 100) { consumer_open = 0; active = queued = 0; }
    else if (fd == 101) exporter_open = 0;
    else {
        assert(fd == 201 || fd == 202);
        assert(!consumer_open && !active && !cpu_sync[fd - 201] && !mapped[fd - 201]);
        assert(live[fd - 201]); live[fd - 201] = 0; closes++;
    }
    return 0;
}
static int fake_ioctl(int fd, unsigned long request, ...)
{
    va_list args; va_start(args, request); void *arg = va_arg(args, void *); va_end(args);
    if (request == VIDIOC_S_FMT) {
        struct v4l2_format *f = arg;
        image_size = probe_image(f->fmt.pix_mp.pixelformat, f->fmt.pix_mp.plane_fmt[0].bytesperline, f->fmt.pix_mp.height);
        f->fmt.pix_mp.plane_fmt[0].sizeimage = image_size;
        if (injected == CLAMPED) f->fmt.pix_mp.width *= 2;
    } else if (request == VIDIOC_REQBUFS) {
        struct v4l2_requestbuffers *r = arg;
        if (fd == 100) memory_type[r->type == probe_type(1)] = r->memory;
    } else if (request == VIDIOC_QUERYBUF) {
        struct v4l2_buffer *b = arg;
        b->m.planes[0].length = image_size;
        b->m.planes[0].m.mem_offset = b->type == probe_type(1) ? 65536 : 0;
    } else if (request == VIDIOC_EXPBUF) {
        struct v4l2_exportbuffer *e = arg;
        unsigned i = e->type == probe_type(1);
        assert(exporter_open);
        if (i && injected == EXPORT_SECOND_FAIL) { errno = EIO; return -1; }
        live[i] = 1; e->fd = 201 + i;
    } else if (request == VIDIOC_STREAMON || request == VIDIOC_STREAMOFF) {
        unsigned i = *(enum v4l2_buf_type *)arg == probe_type(1);
        if (request == VIDIOC_STREAMON) {
            if (i && injected == CAPTURE_STREAM) { errno = EIO; return -1; }
            active |= 1u << i;
        } else {
            if (injected == STREAMOFF_FAIL) { errno = EIO; return -1; }
            active &= ~(1u << i); queued &= ~(1u << i);
        }
    } else if (request == VIDIOC_QBUF || request == VIDIOC_DQBUF) {
        struct v4l2_buffer *b = arg; unsigned i = b->type == probe_type(1);
        assert(b->memory == memory_type[i] && !cpu_sync[0] && !cpu_sync[1]);
        if (request == VIDIOC_QBUF) {
            if (!i && injected == OUTPUT_QBUF) { errno = EIO; return -1; }
            assert(!(queued & (1u << i)));
            queued |= 1u << i;
        } else {
            assert(active == 3 && (queued & (1u << i)));
            if (i) {
                assert(queued == 3); produced++;
                if (injected != NO_WRITE) memcpy(pixels[1], pixels[0], image_size);
                if (injected == BAD_SOURCE) pixels[0][0] ^= 1;
            }
            queued &= ~(1u << i);
            b->m.planes[0].bytesused = image_size;
            if (i && injected == BAD_INDEX) b->index = 1;
            if (i && injected == BAD_MEMORY) b->memory = V4L2_MEMORY_USERPTR;
            if (i && injected == BAD_FLAGS) b->flags = V4L2_BUF_FLAG_ERROR;
            if (i && injected == TRUNCATED) b->m.planes[0].bytesused--;
        }
    } else assert(!"unexpected ioctl");
    return 0;
}
static void *fake_mmap(void *address, size_t size, int prot, int flags, int fd, off_t offset)
{
    (void)address; (void)size; (void)prot; (void)flags;
    if (injected == MMAP_FAIL) { errno = ENOMEM; return MAP_FAILED; }
    unsigned i = fd == 100 ? offset != 0 : (unsigned)(fd - 201);
    assert(i < 2);
    if (fd != 100) { assert(cpu_sync[i]); mapped[i]++; }
    return pixels[i];
}
static int fake_munmap(void *address, size_t size)
{
    (void)size; unsigned i = address == pixels[1];
    assert(address == pixels[i]);
    if (mapped[i]) { assert(cpu_sync[i]); mapped[i]--; }
    else assert(!consumer_open); /* persistent MMAP released after close */
    return 0;
}
static int fake_sync(int fd, uint64_t flags)
{
    unsigned i = fd - 201; assert(i < 2 && live[i]);
    if (flags & DMA_BUF_SYNC_END) { assert(cpu_sync[i] && !mapped[i]); cpu_sync[i] = 0; }
    else {
        if (injected == SYNC_FAIL) { errno = EIO; return -1; }
        assert(!cpu_sync[i]); cpu_sync[i] = 1;
        dma_reads += !(flags & DMA_BUF_SYNC_WRITE);
    }
    return 0;
}
static int fake_wait(int fd, int write) { (void)fd; (void)write; return 0; }
static int fake_poll(struct pollfd *p, nfds_t count, int timeout)
{
    (void)count; (void)timeout;
    if (injected == TIMEOUT) return 0;
    p->revents = p->events; return 1;
}
static int fake_alloc(unsigned bytes, unsigned rows)
{
    assert(bytes * rows == image_size && allocated < 2);
    unsigned i = memory_type[0] == V4L2_MEMORY_DMABUF && !live[0] ? 0 : 1;
    live[i] = 1; allocated++; return 201 + i;
}
static int fake_validate(int fd, uint64_t need, uint32_t *size)
{
    assert(live[fd - 201] && need == image_size);
    *size = injected == OVERSIZED ? vav4l2_test_page_align(image_size) + vav4l2_test_page_size() : image_size; return 0;
}
static struct probe_result exercise(unsigned format, unsigned mode, int order, enum fault fault, int exporter)
{
    memset(pixels, 0, sizeof(pixels)); memset(cpu_sync, 0, sizeof(cpu_sync)); memset(mapped, 0, sizeof(mapped));
    memset(live, 0, sizeof(live));
    memset(memory_type, 0, sizeof(memory_type));
    injected = fault; active = queued = allocated = consumer_open = exporter_open = closes = dma_reads = produced = 0;
    struct probe_result r = probe_case("mock-rga2", 1, 64, probe_formats[format].raw, mode, exporter, order, 0, NULL);
    assert(!active && !queued && !consumer_open && !exporter_open && !live[0] && !live[1]);
    assert(!cpu_sync[0] && !cpu_sync[1] && !mapped[0] && !mapped[1]);
    return r;
}
int main(void)
{
    unsigned scenarios = 0;
    for (unsigned f = 0; f < 4; f++) for (unsigned mode = 0; mode < 4; mode++) for (int order = 0; order < 2; order++) {
        struct probe_result r = exercise(f, mode, order, NONE, 0);
        if (r.status) fprintf(stderr, "unexpected probe failure: format=%u mode=%u order=%d stage=%s errno=%d\n", f, mode, order, r.stage, r.error);
        assert(!r.status && r.completed == 8 && produced == 8);
        assert(dma_reads == 8u * ((unsigned)!!(mode & 1) + (unsigned)!!(mode & 2)));
        r = exercise(f, mode, order, NO_WRITE, 0);
        assert(r.status == 1 && !r.completed && !strcmp(r.stage, "target-readback"));
        assert(r.expected == 102 && r.actual == 58 && r.error == EILSEQ);
        scenarios += 2;
    }
    for (enum fault fault = BAD_SOURCE; fault <= EXPORT_SECOND_FAIL; fault++) {
        struct probe_result r = exercise(0, 3, 1, fault, fault == EXPORT_SECOND_FAIL);
        assert(r.status == (fault == OVERSIZED || fault == CLAMPED ? 77 : 1));
        if (fault == BAD_SOURCE) assert(!strcmp(r.stage, "source-readback"));
        if (fault == EXPORT_SECOND_FAIL) assert(closes == 1 && !produced);
        if (fault == OVERSIZED) assert(!strcmp(r.stage, "unsafe-object-span") && !produced);
        scenarios++;
    }
    struct probe_result r = exercise(0, 3, 0, NONE, 1);
    assert(!r.status && closes == 2 && r.completed == 8);
    printf("PASS standalone transport: %u success/fault cases, no-op detection, ownership and CPU sync\n", scenarios + 1);
    return 0;
}
