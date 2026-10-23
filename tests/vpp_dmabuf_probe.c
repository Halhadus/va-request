/* Standalone V4L2 transport experiment. No VA submission or CPU fallback.
 * CPU access only creates the source/poison and checks the hardware result. */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/dma-buf.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <gbm.h>
#include <drm_fourcc.h>
#include "va_request.h"

enum { PROBE_FRAMES = 8 };
static const struct { const char *name; uint32_t raw; } probe_formats[] = {
    {"NV12", V4L2_PIX_FMT_NV12}, {"NV21", V4L2_PIX_FMT_NV21},
    {"I420", V4L2_PIX_FMT_YUV420}, {"BGRX", V4L2_PIX_FMT_XBGR32},
};
struct probe_storage {
    enum v4l2_memory memory;
    int fd;
    unsigned size;
    void *map;
    struct gbm_bo *bo;
};
struct probe_result {
    const char *stage;
    int error, status; /* 0 PASS, 1 FAIL, 77 SKIP */
    unsigned completed, frame, streams, index[2], flags[2], used[2];
    unsigned size[2], pitch[2], object[2];
    uint64_t mismatch;
    int expected, actual;
};
static int probe_ioctl(int fd, unsigned long request, void *arg)
{
    int ret;
    do ret = ioctl(fd, request, arg); while (ret < 0 && errno == EINTR);
    return ret;
}
static enum v4l2_buf_type probe_type(unsigned i)
{
    return i ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE : V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
}
static unsigned probe_image(uint32_t raw, unsigned pitch, unsigned height)
{
    return pitch * height * (raw == V4L2_PIX_FMT_XBGR32 ? 2 : 3) / 2;
}
/* S_FMT starts a fresh context with full-frame rectangles. Do not silently
 * accept a clamped geometry: 64x64 is intentionally unsupported by RGA3. */
static int probe_formats_set(int fd, unsigned width, uint32_t raw, struct v4l2_format fmt[2])
{
    for (unsigned i = 0; i < 2; i++) {
        fmt[i].type = probe_type(i);
        struct v4l2_pix_format_mplane *p = &fmt[i].fmt.pix_mp;
        p->width = p->height = width; p->pixelformat = raw;
        p->field = V4L2_FIELD_NONE; p->num_planes = 1;
        p->plane_fmt[0].bytesperline = width * (raw == V4L2_PIX_FMT_XBGR32 ? 4 : 1);
        p->colorspace = V4L2_COLORSPACE_REC709;
        p->ycbcr_enc = V4L2_YCBCR_ENC_709;
        p->quantization = raw == V4L2_PIX_FMT_XBGR32 ? V4L2_QUANTIZATION_FULL_RANGE : V4L2_QUANTIZATION_LIM_RANGE;
        p->xfer_func = V4L2_XFER_FUNC_709;
        if (probe_ioctl(fd, VIDIOC_S_FMT, &fmt[i]) < 0) return -1;
        if (p->width != width || p->height != width || p->pixelformat != raw ||
            p->field != V4L2_FIELD_NONE || p->num_planes != 1 ||
            p->plane_fmt[0].bytesperline != width * (raw == V4L2_PIX_FMT_XBGR32 ? 4 : 1) ||
            p->plane_fmt[0].sizeimage < probe_image(raw, p->plane_fmt[0].bytesperline, width)) return 77;
    }
    return 0;
}
static int probe_request(int fd, unsigned i, unsigned memory)
{
    struct v4l2_requestbuffers r = {.type = probe_type(i), .memory = memory, .count = 1};
    if (probe_ioctl(fd, VIDIOC_REQBUFS, &r) < 0) return -1;
    if (!r.count || (r.flags & V4L2_MEMORY_FLAG_NON_COHERENT)) { errno = EINVAL; return -1; }
    return 0;
}
static int probe_query(int fd, unsigned i, struct v4l2_plane *p)
{
    struct v4l2_buffer b = {.type = probe_type(i), .memory = V4L2_MEMORY_MMAP, .length = 1, .m.planes = p};
    if (probe_ioctl(fd, VIDIOC_QUERYBUF, &b) < 0) return -1;
    if (b.index || b.length != 1 || b.memory != V4L2_MEMORY_MMAP) { errno = EINVAL; return -1; }
    return 0;
}
/* Export from an idle, separate V4L2 context, with source/target allocation
 * directions matching their eventual consumer roles. Closing this context
 * must leave the exported references alive. No job is run by the exporter. */
static int probe_queue_export(const char *node, unsigned width, uint32_t raw, int exported[2])
{
    int fd = open(node, O_RDWR | O_NONBLOCK | O_CLOEXEC), ret = -1;
    struct v4l2_format fmt[2] = {{0}};
    if (fd < 0) return -1;
    int format_status = probe_formats_set(fd, width, raw, fmt);
    if (format_status) { if (format_status == 77) errno = ENOTSUP; goto done; }
    for (unsigned i = 0; i < 2; i++) {
        struct v4l2_exportbuffer e = {.type = probe_type(i), .flags = O_RDWR | O_CLOEXEC};
        if (probe_request(fd, i, V4L2_MEMORY_MMAP) < 0 || probe_ioctl(fd, VIDIOC_EXPBUF, &e) < 0) goto done;
        exported[i] = e.fd;
    }
    ret = 0;
done:
    close(fd); return ret;
}
static int probe_stream(int fd, struct probe_result *r)
{
    for (unsigned i = 0; i < 2; i++) if (!(r->streams & (1u << i))) {
        enum v4l2_buf_type type = probe_type(i);
        r->stage = i ? "STREAMON-capture" : "STREAMON-output";
        if (probe_ioctl(fd, VIDIOC_STREAMON, &type) < 0) return -1;
        r->streams |= 1u << i;
    }
    return 0;
}
static unsigned char probe_byte(uint32_t raw, unsigned width, unsigned offset, unsigned frame)
{
    unsigned plane = 0, row = raw == V4L2_PIX_FMT_XBGR32 ? width * 4 : width;
    if (raw == V4L2_PIX_FMT_XBGR32 && offset % 4 == 3) return 255;
    if (offset >= width * width && raw != V4L2_PIX_FMT_XBGR32) {
        offset -= width * width; plane = 1;
        if (raw == V4L2_PIX_FMT_YUV420) {
            row = width / 2;
            if (offset >= width * width / 4) { plane = 2; offset -= width * width / 4; }
        }
    }
    return (unsigned char)(19 + plane * 71 + offset / row * 3 + offset % row * 7 + frame * 23);
}
/* Each DMA CPU access owns a fresh mmap between START/END. A retained GBM
 * attachment, when requested, never provides a shadow mapping or readback. */
static int probe_cpu(struct probe_storage *s, uint32_t raw, unsigned width, unsigned frame,
                     int write, struct probe_result *r)
{
    void *map = s->map;
    uint64_t flags = write ? DMA_BUF_SYNC_WRITE : DMA_BUF_SYNC_READ;
    if (s->memory == V4L2_MEMORY_DMABUF) {
        if (vareq_dmabuf_sync(s->fd, flags | DMA_BUF_SYNC_START) < 0) return -1;
        map = mmap(NULL, s->size, PROT_READ | (write ? PROT_WRITE : 0), MAP_SHARED, s->fd, 0);
        if (map == MAP_FAILED) {
            int error = errno;
            (void)vareq_dmabuf_sync(s->fd, flags | DMA_BUF_SYNC_END);
            errno = error; return -1;
        }
    }
    unsigned count = width * width * (raw == V4L2_PIX_FMT_XBGR32 ? 4 : 3) / (raw == V4L2_PIX_FMT_XBGR32 ? 1 : 2);
    unsigned char *data = map; int ret = 0;
    for (unsigned x = 0; x < count; x++) {
        unsigned char expected = probe_byte(raw, width, x, frame);
        if (write) data[x] = expected;
        else if (data[x] != expected) {
            r->mismatch = x; r->expected = expected; r->actual = data[x]; ret = -1; errno = EILSEQ; break;
        }
    }
    int error = errno;
    if (s->memory == V4L2_MEMORY_DMABUF) {
        if (munmap(map, s->size) < 0) { ret = -1; error = errno; }
        if (vareq_dmabuf_sync(s->fd, flags | DMA_BUF_SYNC_END) < 0) { ret = -1; error = errno; }
    }
    errno = error; return ret;
}
/* combinations: 0 MMAP/MMAP, 1 DMA/MMAP, 2 MMAP/DMA, 3 DMA/DMA.
 * exporter: 0 production coherent allocator, 1 backend's own EXPBUF. */
static struct probe_result probe_case(const char *node, int rga2, unsigned width, uint32_t raw,
                                      unsigned combination, int exporter, int stream_first,
                                      int attach, struct gbm_device *gbm)
{
    struct probe_result r = {.stage = "open", .status = 1, .expected = -1, .actual = -1};
    struct probe_storage s[2] = {{.fd = -1}, {.fd = -1}};
    struct v4l2_format fmt[2] = {{0}};
    int exported[2] = {-1, -1};
    int fd = open(node, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) goto done;
    r.stage = "format";
    int format_status = probe_formats_set(fd, width, raw, fmt);
    if (format_status) { if (format_status == 77 || errno == EINVAL) r.status = 77; goto done; }
    if (attach && !gbm) { r.stage = "GBM-unavailable"; r.status = 77; goto done; }
    r.stage = "export-queue";
    if (exporter && probe_queue_export(node, width, raw, exported) < 0) goto done;
    for (unsigned i = 0; i < 2; i++) {
        r.size[i] = fmt[i].fmt.pix_mp.plane_fmt[0].sizeimage;
        r.pitch[i] = fmt[i].fmt.pix_mp.plane_fmt[0].bytesperline;
        s[i].memory = combination & (1u << i) ? V4L2_MEMORY_DMABUF : V4L2_MEMORY_MMAP;
        r.stage = i ? "REQBUFS-capture" : "REQBUFS-output";
        if (probe_request(fd, i, s[i].memory) < 0) goto done;
        if (s[i].memory == V4L2_MEMORY_MMAP) {
            struct v4l2_plane p = {0}; r.stage = "QUERYBUF";
            if (probe_query(fd, i, &p) < 0) goto done;
            if (p.length < r.size[i]) goto invalid;
            s[i].size = p.length; r.stage = "mmap-queue";
            s[i].map = mmap(NULL, p.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, p.m.mem_offset);
            if (s[i].map == MAP_FAILED) { s[i].map = NULL; goto done; }
        } else {
            r.stage = "allocate-DMA";
            s[i].fd = exporter ? exported[i] : vareq_alloc_shared_storage(r.pitch[i], r.size[i] / r.pitch[i]);
            if (exporter) exported[i] = -1;
            if (s[i].fd < 0) goto done;
            r.stage = "DMA-span";
            if (vareq_export_fd_validate(s[i].fd, r.size[i], &s[i].size) < 0) goto done;
            long page = sysconf(_SC_PAGESIZE);
            if (page <= 0) goto invalid;
            /* Never overrun RGA2's image-sized internal MMU page table. */
            if (rga2 && ((uint64_t)s[i].size + page - 1) / page > ((uint64_t)r.size[i] + page - 1) / page) {
                r.stage = "unsafe-object-span"; r.object[i] = s[i].size; r.status = 77; goto done;
            }
            if (attach) {
                struct gbm_import_fd_modifier_data d = {.width = r.pitch[i], .height = r.size[i] / r.pitch[i],
                    .format = GBM_FORMAT_R8, .num_fds = 1, .fds = {s[i].fd}, .strides = {r.pitch[i]}, .modifier = DRM_FORMAT_MOD_LINEAR};
                r.stage = "GBM-import";
                s[i].bo = gbm_bo_import(gbm, GBM_BO_IMPORT_FD_MODIFIER, &d, GBM_BO_USE_LINEAR);
                if (!s[i].bo) goto done;
                if (gbm_bo_get_stride_for_plane(s[i].bo, 0) != r.pitch[i] || gbm_bo_get_modifier(s[i].bo) != DRM_FORMAT_MOD_LINEAR) goto invalid;
            }
        }
        r.object[i] = s[i].size;
    }
    for (r.frame = 0; r.frame < PROBE_FRAMES; r.frame++) {
        unsigned pattern = 37 + r.frame;
        r.stage = "source-fill"; if (probe_cpu(&s[0], raw, width, pattern, 1, &r) < 0) goto done;
        r.stage = "target-poison"; if (probe_cpu(&s[1], raw, width, 113, 1, &r) < 0) goto done;
        r.stage = "reservation-wait";
        for (unsigned i = 0; i < 2; i++) if (s[i].memory == V4L2_MEMORY_DMABUF && vareq_dmabuf_wait(s[i].fd, i) < 0) goto done;
        if (stream_first && probe_stream(fd, &r) < 0) goto done;
        for (int i = 1; i >= 0; i--) {
            struct v4l2_plane p = {.length = s[i].size, .bytesused = i ? 0 : r.size[i]};
            if (s[i].memory == V4L2_MEMORY_DMABUF) p.m.fd = s[i].fd;
            struct v4l2_buffer b = {.type = probe_type(i), .memory = s[i].memory, .length = 1, .m.planes = &p};
            r.stage = i ? "QBUF-capture" : "QBUF-output";
            if (probe_ioctl(fd, VIDIOC_QBUF, &b) < 0) goto done;
        }
        if (probe_stream(fd, &r) < 0) goto done;
        struct pollfd pfd = {.fd = fd, .events = POLLIN}; r.stage = "poll";
        int polled = vareq_poll_intr(&pfd, 1, 2000);
        if (polled <= 0 || !(pfd.revents & POLLIN) || pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) { errno = ETIMEDOUT; goto done; }
        for (int i = 1; i >= 0; i--) {
            struct v4l2_plane p = {0};
            struct v4l2_buffer b = {.type = probe_type(i), .memory = s[i].memory, .length = 1, .m.planes = &p};
            r.stage = i ? "DQBUF-capture" : "DQBUF-output";
            if (!i) {
                /* OUTPUT readiness is distinct from CAPTURE readiness. */
                pfd.events = POLLOUT; pfd.revents = 0;
                if (vareq_poll_intr(&pfd, 1, 2000) <= 0 || !(pfd.revents & POLLOUT) ||
                    pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) { errno = ETIMEDOUT; goto done; }
            }
            if (probe_ioctl(fd, VIDIOC_DQBUF, &b) < 0) goto done;
            r.index[i] = b.index; r.flags[i] = b.flags; r.used[i] = p.bytesused;
            if (b.index || b.memory != s[i].memory || b.length != 1 || b.flags & V4L2_BUF_FLAG_ERROR ||
                p.data_offset || p.bytesused < r.size[i] || p.bytesused > s[i].size) goto invalid;
        }
        r.stage = "source-readback"; if (probe_cpu(&s[0], raw, width, pattern, 0, &r) < 0) goto done;
        r.stage = "target-readback"; if (probe_cpu(&s[1], raw, width, pattern, 0, &r) < 0) goto done;
        r.completed++;
    }
    r.status = 0; r.stage = "complete"; goto done;
invalid:
    errno = EPROTO;
done:
    r.error = r.status == 1 ? errno : 0;
    if (fd >= 0) {
        for (unsigned i = 0; i < 2; i++) if (r.streams & (1u << i)) {
            enum v4l2_buf_type type = probe_type(i);
            if (probe_ioctl(fd, VIDIOC_STREAMOFF, &type) < 0) {
                if (r.status != 1) { r.status = 1; r.error = errno; r.stage = "STREAMOFF"; }
                /* Closing the consumer below cancels both queues before any
                 * mapping, attachment or exported reference is released. */
            }
        }
        close(fd);
    }
    for (unsigned i = 0; i < 2; i++) {
        if (s[i].bo) gbm_bo_destroy(s[i].bo);
        if (s[i].map) munmap(s[i].map, s[i].size);
        if (s[i].fd >= 0) close(s[i].fd);
        if (exported[i] >= 0) close(exported[i]);
    }
    return r;
}
int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: %s RENDER-NODE RESULTS.tsv\n", argv[0]); return 2; }
    FILE *out = fopen(argv[2], "w"); if (!out) { perror(argv[2]); return 2; }
    fprintf(out, "STATUS\tNODE\tCARD\tFORMAT\tSIZE\tMEMORY_OUTPUT\tMEMORY_CAPTURE\tEXPORTER\tGBM_ATTACH\tORDER\tSTAGE\tERRNO\tFRAMES\tFRAME\tOUTPUT_SIZE\tCAPTURE_SIZE\tOUTPUT_PITCH\tCAPTURE_PITCH\tOUTPUT_OBJECT\tCAPTURE_OBJECT\tOUTPUT_INDEX\tCAPTURE_INDEX\tOUTPUT_FLAGS\tCAPTURE_FLAGS\tOUTPUT_USED\tCAPTURE_USED\tMISMATCH\tEXPECTED\tACTUAL\n");
    int render = open(argv[1], O_RDWR | O_CLOEXEC);
    unsigned pass = 0, fail = 0, skip = 0, nodes = 0;
    for (unsigned n = 0; n < 64; n++) {
        char node[64]; snprintf(node, sizeof(node), "/dev/video%u", n);
        int fd = open(node, O_RDWR | O_NONBLOCK | O_CLOEXEC); if (fd < 0) continue;
        struct v4l2_capability cap = {0}; int ret = probe_ioctl(fd, VIDIOC_QUERYCAP, &cap); close(fd);
        unsigned caps = cap.capabilities & V4L2_CAP_DEVICE_CAPS ? cap.device_caps : cap.capabilities;
        int rga2 = !memcmp(cap.card, "rga2", sizeof("rga2")) || !memcmp(cap.card, "rockchip-rga", sizeof("rockchip-rga"));
        if (ret || memcmp(cap.driver, "rockchip-rga", sizeof("rockchip-rga")) ||
            (!rga2 && memcmp(cap.card, "rga3", sizeof("rga3"))) ||
            !(caps & V4L2_CAP_VIDEO_M2M_MPLANE) || !(caps & V4L2_CAP_STREAMING)) continue;
        nodes++;
        /* Six transport choices, both orders and sizes; fresh fd per case.
         * Baseline, mixed queues, shared pairs, own-export pairs, GBM pairs. */
        for (unsigned f = 0; f < sizeof(probe_formats) / sizeof(probe_formats[0]); f++)
        for (unsigned width = 64; width <= 128; width *= 2)
        for (unsigned order = 0; order < 2; order++)
        for (unsigned choice = 0; choice < 6; choice++) {
            unsigned combination = choice < 4 ? choice : 3;
            /* Baseline/mixed/own-export cases create no GBM device or BO.
             * Limit the optional attachment to this one fresh experiment. */
            struct gbm_device *gbm = choice == 5 && render >= 0 ? gbm_create_device(render) : NULL;
            struct probe_result r = probe_case(node, rga2, width, probe_formats[f].raw, combination,
                                               choice == 4, order, choice == 5, gbm);
            if (gbm) gbm_device_destroy(gbm);
            const char *status = r.status == 0 ? "PASS" : r.status == 77 ? "SKIP" : "FAIL";
            pass += r.status == 0; skip += r.status == 77; fail += r.status == 1;
            fprintf(out, "%s\t%s\t%.*s\t%s\t%u\t%u\t%u\t%s\t%u\t%s\t%s\t%d\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%" PRIu64 "\t%d\t%d\n",
                status, node, (int)sizeof(cap.card), cap.card, probe_formats[f].name, width,
                combination & 1 ? V4L2_MEMORY_DMABUF : V4L2_MEMORY_MMAP,
                combination & 2 ? V4L2_MEMORY_DMABUF : V4L2_MEMORY_MMAP,
                choice == 4 ? "backend-EXPBUF" : "shared-allocator", choice == 5,
                order ? "stream-first" : "queue-first", r.stage, r.error, r.completed, r.frame,
                r.size[0], r.size[1], r.pitch[0], r.pitch[1], r.object[0], r.object[1],
                r.index[0], r.index[1], r.flags[0], r.flags[1], r.used[0], r.used[1], r.mismatch, r.expected, r.actual);
            fflush(out);
        }
    }
    if (render >= 0) close(render);
    vareq_allocator_cleanup();
    if (fclose(out)) { perror("close probe results"); return 2; }
    printf("TRANSPORT_PROBE PASS=%u FAIL=%u SKIP=%u NODES=%u COMPLETE=1 EXIT=%u\n", pass, fail, skip, nodes, fail ? 1 : nodes ? 0 : 77);
    return fail ? 1 : nodes ? 0 : 77;
}
