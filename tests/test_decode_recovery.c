/* Fault injection for synchronous queue ownership; no VPU is required. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <poll.h>
#include <linux/media.h>
#include "va_v4l2.h"

static int failures, mode, stopped, queued_cap, allocations, releases;
static int field_active, field_fault, field_stage, field_outputs, field_captures, field_dequeues, field_requests;
static struct timeval submitted_time;
int __wrap_poll(struct pollfd *fds, nfds_t count, int timeout)
{
    (void)timeout;
    if (field_active && field_stage == 1 && field_fault == 1) return 0;
    for (nfds_t i = 0; i < count; i++) fds[i].revents =
        field_active && field_stage == 1 && field_fault == 2 ? POLLERR : fds[i].events;
    return 1;
}
static void expect(int ok, const char *tag)
{
    if (!ok) { fprintf(stderr, "FAIL %s mode=%d\n", tag, mode); failures++; }
}
static int fault_ioctl(int fd, unsigned long request, void *arg)
{
    (void)fd;
    if (request == VIDIOC_QUERYCTRL) { ((struct v4l2_queryctrl *)arg)->maximum = 1; return 0; }
    if (request == VIDIOC_S_FMT) {
        struct v4l2_format *f = arg;
        if (mode == 9) f->fmt.pix_mp.width--;
        if (mode == 10) f->fmt.pix_mp.pixelformat = V4L2_PIX_FMT_VP8_FRAME;
        if (mode == 11) f->fmt.pix_mp.num_planes = 2;
        return 0;
    }
    if (request == VIDIOC_REQBUFS) {
        struct v4l2_requestbuffers *r = arg;
        if (mode < 13) { allocations++; return 0; }
        expect(r->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE && r->memory == V4L2_MEMORY_MMAP, "retry-keeps-OUTPUT-memory-contract");
        if (!r->count) { releases++; if (mode == 17) { errno = EBUSY; return -1; } return 0; }
        allocations++;
        if (mode == 16) { errno = EIO; return -1; }
        if (mode == 18 && r->count == 4) { r->count = 0; return 0; }
        if (mode == 15 || mode == 17 || r->count > (mode == 13 || mode == 18 ? 2u : 1u)) { errno = ENOMEM; return -1; }
        return 0;
    }
    if (request == VIDIOC_STREAMOFF) { stopped++; if (mode == 7 || mode == 8) { errno = EIO; return -1; } return 0; }
    if (request == VIDIOC_QBUF) {
        struct v4l2_buffer *b = arg;
        if (b->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            queued_cap = b->index;
            if (mode == 1 || mode == 7 || mode == 8) { errno = EIO; return -1; }
        }
        else submitted_time = b->timestamp;
        return 0;
    }
    if (request == VIDIOC_DQBUF) {
        struct v4l2_buffer *b = arg;
        if (b->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
            if (mode == 2) { errno = EAGAIN; return -1; }
            b->index = 0;
            if (mode == 4) b->flags |= V4L2_BUF_FLAG_ERROR;
        } else {
            b->index = mode == 6 ? 39 : queued_cap;
            b->timestamp = submitted_time; b->m.planes[0].bytesused = 384;
            if (mode == 5) b->flags |= V4L2_BUF_FLAG_ERROR;
            if (mode == 20) b->m.planes[0].bytesused = 0;
            if (mode == 21) b->m.planes[0].bytesused = 383;
            if (mode == 22) b->m.planes[0].bytesused = 385;
            if (mode == 23) b->m.planes[0].data_offset = 1;
            if (mode == 24) b->length = 2;
            if (mode == 25) b->memory = V4L2_MEMORY_DMABUF;
            if (mode == 26) b->timestamp.tv_usec++;
            if (mode == 27) b->timestamp.tv_sec = -1;
            if (mode == 28) { b->m.planes[0].bytesused = 0; b->flags = V4L2_BUF_FLAG_LAST; }
        }
        return 0;
    }
    if (request == MEDIA_REQUEST_IOC_REINIT && mode == 3) { errno = EBUSY; return -1; }
    return 0;
}
static void recovery_case(int test_mode)
{
    struct vav4l2_driver_data dd = {0};
    struct vav4l2_context c = {.driver_data = &dd, .output_bufs_allocd = 1, .output_buf_size = 64,
        .output_plane_len = {64}, .capture_bufs_allocd = 2, .streamed = 1, .media_fd = -1,
        .cap_width = 16, .cap_height = 16, .cap_stride = 16, .cap_sizeimage = 384, .cap_pixelformat = V4L2_PIX_FMT_NV12};
    struct vav4l2_surface s = {.surface_id = 1, .buf_index = 1, .capture_ctx = &c};
    c.v4l2_fd = open("/dev/null", O_RDWR | O_CLOEXEC);
    c.request_fd = dup(c.v4l2_fd);
    int video_fd = c.v4l2_fd, request_fd = c.request_fd;
    dd.surfaces[1] = &s;
    /* Surface 1 is deliberately absent from render_targets. */
    c.current_surface = &s;
    c.n_free_cap = 1; c.free_cap_bufs[0] = 0;
    mode = test_mode; stopped = 0;
    vav4l2_set_ioctl_hook(fault_ioctl);
    int result = mode == 8 ? vav4l2_ensure_capture(&c, 32, 32, V4L2_PIX_FMT_NV12) :
        vav4l2_decode_submit(&c, 0, 8, 1000);
    if (mode == 7 || mode == 8) {
        expect(result == -1 && stopped == 2 && c.v4l2_fd == -1 && c.request_fd == -1 && !c.streamed &&
               !c.n_free_out && !c.n_free_cap && fcntl(video_fd, F_GETFD) < 0 && fcntl(request_fd, F_GETFD) < 0,
               "failed-streamoff-closes-device-without-recycling-uncertain-buffers");
        expect(!s.has_pic && s.status == VASurfaceSkipped && s.buf_index == -1 && !s.capture_ctx && !s.cap_view,
               "failed-stop-invalidates-capture-views");
        expect(vav4l2_decode_submit(&c, 0, 8, 2000) == -1 && !c.n_free_cap && !c.n_free_out,
               "dead-context-cannot-resubmit-old-indices");
    } else if (mode == 1 || mode == 2 || mode == 6 || mode >= 20) {
        expect(result == -1 && stopped == 2 && !c.streamed && c.request_fd == -1, "failure-resets-both-queues");
        expect(c.n_free_out == 1 && c.n_free_cap == 1 && c.free_cap_bufs[0] == 0,
               "reset-preserves-unregistered-claimed-capture");
    } else if (mode == 3) {
        expect(result == 1 && c.request_fd == -1 && stopped == 0, "reinit-ebusy-drops-request");
    } else if (!mode) expect(result == 1 && !stopped, "valid-payload-and-timestamp-complete");
    else {
        expect(result == -2 && c.n_free_cap == 1 && c.free_cap_bufs[0] == 0,
               "error-does-not-free-claimed-capture");
    }
    vav4l2_set_ioctl_hook(NULL);
    if (c.request_fd >= 0) close(c.request_fd);
    if (c.v4l2_fd >= 0) close(c.v4l2_fd);
}
static int field_ioctl(int fd, unsigned long request, void *arg)
{
    (void)fd;
    if (request == VIDIOC_QBUF) {
        struct v4l2_buffer *b = arg;
        if (b->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
            field_outputs++;
            expect(!!(b->flags & V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF) == (field_stage == 1) &&
                (b->flags & V4L2_BUF_FLAG_REQUEST_FD) && b->timestamp.tv_usec == 1,
                "fields-use-HOLD-only-on-first-OUTPUT-with-shared-timestamp");
            if (field_fault == 8 || (field_fault == 5 && field_stage == 2)) { errno = EIO; return -1; }
        } else {
            field_captures++; queued_cap = b->index;
            expect(field_captures == 1 && !(b->flags & V4L2_BUF_FLAG_REQUEST_FD), "held-capture-is-queued-bare-exactly-once");
            if (field_fault == 9) { errno = EIO; return -1; }
        }
    }
    if (request == VIDIOC_DQBUF) {
        struct v4l2_buffer *b = arg;
        if (b->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
            b->index = 0;
            if (field_fault == 3) b->flags = V4L2_BUF_FLAG_ERROR;
        } else {
            field_dequeues++;
            expect(field_stage == 2, "first-field-never-dequeues-held-CAPTURE");
            b->index = field_fault == 11 ? 39 : queued_cap;
            b->timestamp.tv_usec = 1; b->m.planes[0].bytesused = 384;
            if (field_fault == 6) b->flags = V4L2_BUF_FLAG_ERROR;
        }
    }
    if (request == MEDIA_REQUEST_IOC_QUEUE) {
        field_requests++;
        if (field_fault == 10 || (field_fault == 7 && field_stage == 2)) { errno = EIO; return -1; }
    }
    if (request == MEDIA_REQUEST_IOC_REINIT && field_fault == 4) { errno = EBUSY; return -1; }
    if (request == VIDIOC_STREAMOFF) stopped++;
    return 0;
}
static void fields_case(int fault, int preclaimed)
{
    struct vav4l2_driver_data dd = {0};
    struct vav4l2_context c = {.driver_data = &dd, .output_bufs_allocd = 1, .output_buf_size = 64,
        .output_plane_len = {64}, .capture_bufs_allocd = 2, .streamed = 1, .media_fd = -1,
        .cap_width = 16, .cap_height = 16, .cap_stride = 16, .cap_sizeimage = 384, .cap_pixelformat = V4L2_PIX_FMT_NV12};
    struct vav4l2_surface s = {.surface_id = 1, .buf_index = preclaimed ? 1 : -1, .capture_ctx = preclaimed ? &c : NULL,
        .status = VASurfaceRendering};
    dd.surfaces[1] = &s; c.current_surface = &s;
    c.v4l2_fd = open("/dev/null", O_RDWR | O_CLOEXEC); c.request_fd = dup(c.v4l2_fd);
    c.n_free_cap = preclaimed ? 1 : 2; c.free_cap_bufs[0] = 0; c.free_cap_bufs[1] = 1;
    field_active = field_stage = 1; field_fault = fault;
    stopped = field_outputs = field_captures = field_dequeues = field_requests = 0;
    vav4l2_set_ioctl_hook(field_ioctl);
    int first = vav4l2_decode_submit_field(&c, 0, 8, 1000, -1, 1);
    if (!fault || fault == 5 || fault == 6 || fault == 7 || fault == 11) {
        expect(first == 1 && !field_dequeues && field_captures == 1 && c.n_free_out == 1 && c.n_free_cap == 1 && !stopped,
            "first-field-completes-request-and-OUTPUT-but-retains-CAPTURE");
        field_stage = 2; c.n_free_out--;
        int second = vav4l2_decode_submit_field(&c, 0, 8, 1000, first, 0);
        if (!fault) expect(second == first && field_outputs == 2 && field_captures == 1 && field_dequeues == 1 &&
            field_requests == 2 && c.n_free_out == 1 && c.n_free_cap == 1 && !stopped,
            "second-field-releases-the-same-full-capture-without-a-second-QBUF");
        else {
            expect(second == (fault == 6 ? -2 : -1), "second-field-failure-propagates");
            if (fault == 6) vav4l2_decode_reset(&c); /* Translator cleanup after corrupt final capture. */
            expect(stopped == 2 && c.request_fd == -1, "second-field-failure-resets-held-ownership");
        }
    } else {
        expect(first == (fault == 3 ? -2 : -1), "first-field-failure-propagates");
        expect(fault == 8 ? !stopped && !field_captures && c.n_free_out == 1 : stopped == 2 && c.request_fd == -1,
            "failed-first-field-never-recycles-a-kernel-held-capture");
    }
    field_active = 0; vav4l2_set_ioctl_hook(NULL);
    if (c.request_fd >= 0) close(c.request_fd);
    if (c.v4l2_fd >= 0) close(c.v4l2_fd);
}
int main(void)
{
    setenv("VAV4L2_EXPBUF_EXPORT", "1", 1);
    for (int i = 0; i <= 8; i++) recovery_case(i);
    for (int i = 20; i <= 28; i++) recovery_case(i);
    vav4l2_set_ioctl_hook(fault_ioctl);
    for (mode = 9; mode <= 12; mode++) {
        allocations = 0;
        int result = vav4l2_setup_output_queue(7, V4L2_PIX_FMT_H264_SLICE, 320, 240);
        expect(mode == 12 ? result > 0 && allocations == 1 : result < 0 && !allocations,
            "coded-negotiation-must-preserve-format-size-and-single-plane-before-allocation");
    }
    allocations = 0;
    expect(vav4l2_setup_output_queue(7, V4L2_PIX_FMT_H264_SLICE, 65535, 65535) < 0 && !allocations,
        "coded-sizeimage-overflow-rejected-before-allocation");
    for (mode = 13; mode <= 18; mode++) {
        allocations = releases = 0; errno = 0;
        int result = vav4l2_setup_output_queue(7, V4L2_PIX_FMT_H264_SLICE, 320, 240);
        if (mode == 13 || mode == 18) expect(result == 2 && allocations == 2 && releases == 1, "ENOMEM-or-zero-grant-can-use-two-OUTPUT-slots");
        if (mode == 14) expect(result == 1 && allocations == 3 && releases == 2, "synchronous-decoder-can-use-one-OUTPUT-slot");
        if (mode == 15) expect(result < 0 && errno == ENOMEM && allocations == 3 && releases == 3, "all-counts-fail-truthfully-with-clean-queue");
        if (mode == 16) expect(result < 0 && errno == EIO && allocations == 1 && !releases, "non-allocation-error-is-not-retried");
        if (mode == 17) expect(result < 0 && errno == EBUSY && allocations == 1 && releases == 1, "failed-release-stops-before-reallocation");
    }
    vav4l2_set_ioctl_hook(NULL);
    for (int fault = 0; fault <= 11; fault++) for (int claimed = 0; claimed < 2; claimed++) fields_case(fault, claimed);
    return failures != 0;
}
