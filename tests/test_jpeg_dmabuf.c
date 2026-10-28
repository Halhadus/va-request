/* Transport/error oracle: input bytes are read only by the mocked encoder. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <linux/dma-buf.h>
#include <drm_fourcc.h>
#include <va/va.h>
#include <va/va_enc_jpeg.h>
#include "va_v4l2.h"
#include "page_size.h"
static unsigned failures, jobs, syncs, expected_planes, held[3], active, scenario;
static struct v4l2_format format;
static void expect(int ok, const char *s) { if (!ok) { fprintf(stderr, "FAIL JPEG DMA %s scenario=%u\n", s, scenario); failures++; } }
int __wrap_poll(struct pollfd *p, nfds_t n, int timeout)
{
    (void)n; (void)timeout; p->revents = p->events;
    if (active && scenario == 14) p->revents = POLLERR;
    return active && scenario == 6 ? 0 : 1;
}
static int mock_ioctl(int fd, unsigned long request, void *arg)
{
    (void)fd;
    if (request == DMA_BUF_IOCTL_SYNC) { syncs++; expect(!active || scenario == 13, "direct-input-never-CPU-synchronized-or-refreshed"); return 0; }
    if (request == VIDIOC_S_FMT) {
        struct v4l2_format *f = arg;
        if (f->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
            if (scenario == 1 || (scenario == 13 && f->fmt.pix_mp.plane_fmt[0].bytesperline)) { errno = EINVAL; return -1; }
            format = *f;
            for (unsigned p = 0; p < f->fmt.pix_mp.num_planes; p++) {
                format.fmt.pix_mp.plane_fmt[p].sizeimage = p ? expected_planes == 3 ? 16 : 32 : expected_planes == 1 ? 96 : 64;
                if (scenario == 2) format.fmt.pix_mp.plane_fmt[p].bytesperline++;
                if (scenario == 12) format.fmt.pix_mp.plane_fmt[p].sizeimage = 8192;
                if (scenario == 13) format.fmt.pix_mp.plane_fmt[p].bytesperline = 8;
            }
            *f = format;
        }
    }
    if (request == VIDIOC_G_FMT) *(struct v4l2_format *)arg = format;
    if (request == VIDIOC_REQBUFS) {
        struct v4l2_requestbuffers *r = arg;
        if (r->count && r->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
            expect(r->memory == V4L2_MEMORY_DMABUF || scenario == 13, "OUTPUT-uses-DMABUF");
            if (scenario == 3) { errno = EINVAL; return -1; }
            if (scenario == 9) r->count = 0;
        }
        if (r->count && r->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE && scenario == 10) r->count = 0;
    }
    if (request == VIDIOC_QUERYBUF) {
        struct v4l2_buffer *b = arg;
        expect(b->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE || scenario == 13, "raw-input-is-never-MMAP-allocated");
        b->length = b->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE ? 1 : 2;
        for (unsigned p = 0; p < b->length; p++) { b->m.planes[p].length = p ? 32 : 64; b->m.planes[p].m.mem_offset = p * vav4l2_test_page_size(); }
    }
    if (request == VIDIOC_QBUF) {
        struct v4l2_buffer *b = arg;
        if (b->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
            if (scenario == 13) { expect(b->memory == V4L2_MEMORY_MMAP && b->length == 2, "unsupported-direct-setup-falls-back-before-submission"); return 0; }
            jobs++; expect(b->memory == V4L2_MEMORY_DMABUF && b->length == expected_planes, "QBUF-actual-DMA-plane-count");
            for (unsigned p = 0; p < b->length; p++) {
                unsigned char byte = 0; held[p] = b->m.planes[p].m.fd;
                expect(fcntl(held[p], F_GETFD) & FD_CLOEXEC, "held-input-fd-is-owned-CLOEXEC");
                expect(!b->m.planes[p].data_offset && b->m.planes[p].bytesused <= b->m.planes[p].length &&
                    pread(held[p], &byte, 1, 0) == 1 && byte == 31 + p * 67, "physical-Y-U-V-plane-order-and-byte-span");
            }
            if (scenario == 4) { errno = EIO; return -1; }
        }
    }
    if (request == VIDIOC_DQBUF) {
        struct v4l2_buffer *b = arg;
        if (b->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            b->m.planes[0].bytesused = 24; b->m.planes[0].data_offset = 4;
            if (scenario == 5) b->flags |= V4L2_BUF_FLAG_ERROR;
        }
        if (b->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE && scenario == 7) b->flags |= V4L2_BUF_FLAG_ERROR;
    }
    if (request == VIDIOC_STREAMOFF && scenario == 8) { errno = EIO; return -1; }
    return 0;
}
static void run(unsigned fourcc, unsigned planes, unsigned fault)
{
    scenario = fault; expected_planes = planes; jobs = syncs = active = 0;
    VADRMPRIMESurfaceDescriptor d = {.fourcc = fourcc, .width = 8, .height = 8, .num_objects = planes, .num_layers = 1};
    struct vav4l2_prime_layout l; expect(!vav4l2_prime_layout(fourcc, 8, 8, &l), "layout");
    d.layers[0].num_planes = l.planes; d.layers[0].drm_format = l.drm_format;
    for (unsigned p = 0; p < planes; p++) {
        d.objects[p].fd = memfd_create("JPEG input", MFD_CLOEXEC); d.objects[p].size = 4096;
        expect(d.objects[p].fd >= 0 && !ftruncate(d.objects[p].fd, 4096), "input-storage");
        unsigned char bytes[4096]; memset(bytes, 31 + p * 67, sizeof(bytes));
        expect(pwrite(d.objects[p].fd, bytes, sizeof(bytes), 0) == sizeof(bytes), "producer-data");
    }
    for (unsigned p = 0; p < l.planes; p++) {
        unsigned physical = p && fourcc == VA_FOURCC_YV12 ? 3 - p : p;
        d.layers[0].pitch[p] = planes == 3 && p ? 4 : 8;
        d.layers[0].object_index[p] = planes == 1 ? 0 : physical;
        d.layers[0].offset[p] = planes == 1 && p ? 64 : 0;
    }
    if (scenario == 11) d.layers[0].offset[0] = 16;
    struct vav4l2_surface s = {.width = 8, .height = 8, .format = fourcc, .cpu_stride = 64,
        .cpu_size = vav4l2_va_image_size(fourcc, 64, 8), .memfd_fd = -1, .buf_index = -1};
    vav4l2_set_ioctl_hook(mock_ioctl);
    expect(vav4l2_prime_import(&s, &d) == VA_STATUS_SUCCESS, "import");
    for (unsigned p = 0; p < planes; p++) close(d.objects[p].fd);
    struct vav4l2_buffer coded = {.buffer_id = 1, .type = VAEncCodedBufferType, .size = 64, .num_elements = 1};
    coded.data = calloc(1, sizeof(VACodedBufferSegment) + 64);
    struct vav4l2_context ctx = {.codec = VAV4L2_CODEC_JPEG_ENC, .current_surface = &s, .buffers = &coded};
    ctx.v4l2_fd = memfd_create("JPEG coded queue", MFD_CLOEXEC); expect(ctx.v4l2_fd >= 0 && !ftruncate(ctx.v4l2_fd, 2 * vav4l2_test_page_size()), "coded-storage");
    VAEncPictureParameterBufferJPEG pic = {.coded_buf = 1, .quality = 50};
    struct vav4l2_buffer params = {.type = VAEncPictureParameterBufferType, .data = &pic, .size = sizeof(pic)};
    struct vav4l2_buffer *buffers[] = {&params};
    setenv("VAV4L2_JPEG_DMABUF", scenario == 13 ? "auto" : "required", 1); active = 1; unsigned before_sync = syncs;
    for (unsigned frame = 0; frame < (fault ? 1u : 3u); frame++) {
        VAStatus status = vav4l2_jpeg_encode(&ctx, buffers, 1);
        VACodedBufferSegment *seg = coded.data;
        expect(fault && fault != 13 ? status != VA_STATUS_SUCCESS && !seg->size : status == VA_STATUS_SUCCESS && seg->size == 20, "success-or-transactional-failure");
        if (fault != 13) expect(!s.cpu_ptr && syncs == before_sync, "no-input-staging-or-CPU-cache-access");
        else expect(s.cpu_ptr && syncs > before_sync, "fallback-refreshes-external-input");
        if (jobs) for (unsigned p = 0; p < planes; p++) {
            int flags = fcntl(held[p], F_GETFD);
            expect(flags < 0 && errno == EBADF, "job-export-FDs-released-after-stop");
        }
    }
    active = 0; scenario = 0; vav4l2_m2m_teardown(&ctx, &ctx.jpeg_q);
    if (ctx.v4l2_fd >= 0) close(ctx.v4l2_fd);
    vav4l2_prime_destroy(&s); free(s.cpu_ptr); free(coded.data); vav4l2_set_ioctl_hook(NULL);
}
int main(void)
{
    run(VA_FOURCC_NV12, 1, 0); run(VA_FOURCC_NV12, 2, 0);
    run(VA_FOURCC_I420, 3, 0); run(VA_FOURCC_YV12, 3, 0);
    for (unsigned fault = 1; fault <= 14; fault++) run(VA_FOURCC_NV12, 2, fault);
    printf("jpeg_dmabuf: %s\n", failures ? "FAIL" : "PASS"); return failures != 0;
}
