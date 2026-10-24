/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <stdio.h>
#include "stateful_fixture.h"
#include "probe.h"
#include <va/va_backend_vpp.h>
extern VAStatus __vaDriverInit_1_20(VADriverContextP);
static struct { struct timeval time; unsigned index, flags, bytes; } done[16];
static unsigned tail, head, output[4], queued[40], stops, starts, captures, source, submitted;
static uint64_t timestamps[8];
static int no_complete, bad_format, stop_error, short_buffer, extra_event, hold_output, probe_only;
static uint32_t active_coded;
static unsigned output_stops;
static void result(unsigned index, uint64_t timestamp, unsigned flags, unsigned bytes)
{
    assert(tail < 16); done[tail].index = index; done[tail].time.tv_sec = timestamp / 1000000000;
    done[tail].time.tv_usec = timestamp / 1000 % 1000000; done[tail].flags = flags; done[tail++].bytes = bytes;
}
static int mock(int fd, unsigned long request, void *arg)
{
    (void)fd;
    if (request == VIDIOC_SUBSCRIBE_EVENT || request == VIDIOC_UNSUBSCRIBE_EVENT || request == VIDIOC_TRY_DECODER_CMD) return 0;
    if (request == VIDIOC_QUERYCTRL) {
        struct v4l2_queryctrl *q = arg;
        assert(q->id == (active_coded == V4L2_PIX_FMT_HEVC ? V4L2_CID_MPEG_VIDEO_HEVC_PROFILE : V4L2_CID_MPEG_VIDEO_H264_PROFILE));
        q->type = V4L2_CTRL_TYPE_MENU; q->maximum = 17; return 0;
    }
    if (request == VIDIOC_QUERYMENU) return 0;
    if (request == VIDIOC_S_FMT || request == VIDIOC_G_FMT) {
        struct v4l2_format *f = arg;
        if (f->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            f->fmt.pix_mp.width = f->fmt.pix_mp.height = 16; f->fmt.pix_mp.num_planes = 1;
            f->fmt.pix_mp.pixelformat = bad_format ? V4L2_PIX_FMT_YUV420 : V4L2_PIX_FMT_NV12;
            f->fmt.pix_mp.field = V4L2_FIELD_NONE; f->fmt.pix_mp.plane_fmt[0].bytesperline = 16; f->fmt.pix_mp.plane_fmt[0].sizeimage = 384;
        } else {
            if (request == VIDIOC_S_FMT) active_coded = f->fmt.pix_mp.pixelformat;
            else f->fmt.pix_mp.pixelformat = active_coded;
            f->fmt.pix_mp.num_planes = 1;
        }
        return 0;
    }
    if (request == VIDIOC_ENUM_FMT) {
        struct v4l2_fmtdesc *f = arg; if (f->index && !(probe_only && f->index == 1)) { errno = EINVAL; return -1; }
        f->pixelformat = f->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE ? V4L2_PIX_FMT_NV12 : f->index ? V4L2_PIX_FMT_HEVC : V4L2_PIX_FMT_H264; return 0;
    }
    if (request == VIDIOC_G_SELECTION) { struct v4l2_selection *s = arg; s->r.width = s->r.height = 16; return 0; }
    if (request == VIDIOC_G_CTRL) { ((struct v4l2_control *)arg)->value = 4; return 0; }
    if (request == VIDIOC_REQBUFS) {
        struct v4l2_requestbuffers *b = arg;
        if (b->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE && b->count) { assert(b->count >= 8); captures++; }
        return 0;
    }
    if (request == VIDIOC_QUERYBUF) {
        struct v4l2_buffer *b = arg; b->m.planes[0].length = b->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE ? 512 : 65536;
        b->m.planes[0].m.mem_offset = b->index * 65536; return 0;
    }
    if (request == VIDIOC_STREAMON || request == VIDIOC_STREAMOFF) {
        if (request == VIDIOC_STREAMOFF) {
            if (*(unsigned *)arg == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) memset(queued, 0, sizeof(queued));
            else output_stops++;
        }
        return 0;
    }
    if (request == VIDIOC_QBUF) {
        struct v4l2_buffer *b = arg;
        if (b->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) { assert(!queued[b->index]); queued[b->index] = 1; }
        else {
            assert(!(b->flags & V4L2_BUF_FLAG_REQUEST_FD) && b->m.planes[0].bytesused); output[b->index] = 1;
            timestamps[submitted++] = ((uint64_t)b->timestamp.tv_sec * 1000000 + b->timestamp.tv_usec) * 1000;
            if (!captures) source = 1;
        }
        return 0;
    }
    if (request == VIDIOC_DQEVENT) {
        if (source || extra_event) {
            struct v4l2_event *e = arg; e->type = extra_event ? V4L2_EVENT_EOS : V4L2_EVENT_SOURCE_CHANGE;
            e->u.src_change.changes = V4L2_EVENT_SRC_CH_RESOLUTION; source = extra_event = 0; return 0;
        }
        errno = EAGAIN; return -1;
    }
    if (request == VIDIOC_DQBUF) {
        struct v4l2_buffer *b = arg;
        if (b->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
            if (!hold_output) for (unsigned i = 0; i < 4; i++) if (output[i]) { b->index = i; output[i] = 0; return 0; }
        } else if (head < tail) {
            b->index = done[head].index; b->timestamp = done[head].time; b->flags = done[head].flags;
            b->m.planes[0].bytesused = short_buffer ? 1 : done[head].bytes;
            assert(queued[b->index]); queued[b->index] = 0; head++; return 0;
        }
        errno = EAGAIN; return -1;
    }
    if (request == VIDIOC_DECODER_CMD) {
        struct v4l2_decoder_cmd *c = arg;
        if (c->cmd == V4L2_DEC_CMD_STOP) {
            if (stop_error) { errno = EIO; return -1; }
            stops++;
            if (!no_complete) { for (unsigned i = 0; i < submitted; i++) result(i, timestamps[i], 0, 384); result(submitted, 0, V4L2_BUF_FLAG_LAST, 0); }
        } else { assert(c->cmd == V4L2_DEC_CMD_START); starts++; }
        return 0;
    }
    errno = EINVAL; return -1;
}
int __wrap_ioctl(int fd, unsigned long request, ...) { va_list ap; va_start(ap, request); void *p = va_arg(ap, void *); va_end(ap); return mock(fd, request, p); }
int __wrap_poll(struct pollfd *fds, nfds_t count, int timeout) { (void)count; (void)timeout; fds[0].revents = POLLIN; return 1; }
static void init(struct vav4l2_context *c, struct vav4l2_driver_data *dd, struct vav4l2_surface *surfaces)
{
    tail = head = stops = starts = captures = source = submitted = output_stops = 0;
    no_complete = bad_format = stop_error = short_buffer = extra_event = hold_output = probe_only = 0;
    memset(output, 0, sizeof(output)); memset(queued, 0, sizeof(queued));
    memset(c, 0, sizeof(*c)); memset(dd, 0, sizeof(*dd)); memset(surfaces, 0, 2 * sizeof(*surfaces));
    c->codec = VAV4L2_CODEC_H264; c->profile = VAProfileH264High; c->rt_format = VA_RT_FORMAT_YUV420; c->coded_format = V4L2_PIX_FMT_H264;
    c->width = c->height = 16; c->driver_data = dd; c->media_fd = c->request_fd = -1;
    c->v4l2_fd = memfd_create("stateful-queue", MFD_CLOEXEC); assert(c->v4l2_fd >= 0 && !ftruncate(c->v4l2_fd, 4 * 65536));
    for (unsigned i = 0; i < 2; i++) { surfaces[i].surface_id = i; surfaces[i].width = surfaces[i].height = 16;
        surfaces[i].memfd_fd = surfaces[i].buf_index = -1; surfaces[i].format = VA_FOURCC_NV12; surfaces[i].status = VASurfaceRendering; dd->surfaces[i] = &surfaces[i]; }
    vav4l2_set_ioctl_hook(mock);
    const char *modes[] = {"VAV4L2_CAPTURE_DMABUF", "VAV4L2_CAPTURE_POOL_DMABUF"};
    for (unsigned i = 0; i < 2; i++) {
        setenv(modes[i], "required", 1); assert(vav4l2_stateful_init(c) < 0 && !c->stateful); unsetenv(modes[i]);
    }
    assert(vav4l2_stateful_init(c) == 0 && c->media_fd == -1 && c->request_fd == -1 && !captures);
}
static void submit(struct vav4l2_context *c, struct vav4l2_surface *surface, struct stateful_fixture *f, uint64_t timestamp)
{
    c->current_surface = surface; c->current_surface_id = surface->surface_id; surface->timestamp = timestamp;
    assert(vav4l2_stateful_translate(c, f->batch, 3) == VA_STATUS_SUCCESS); c->current_surface = NULL;
}
static void finish(struct vav4l2_context *c, struct vav4l2_surface *surfaces)
{
    vav4l2_release_context_device(c);
    for (unsigned i = 0; i < 2; i++) { if (surfaces[i].memfd_map) munmap(surfaces[i].memfd_map, surfaces[i].memfd_map_size); if (surfaces[i].memfd_fd >= 0) close(surfaces[i].memfd_fd); }
    vav4l2_set_ioctl_hook(NULL);
}
int main(void)
{
    struct vav4l2_context c; struct vav4l2_driver_data dd; struct vav4l2_surface surfaces[2]; struct stateful_fixture f; stateful_fixture(&f);
    vav4l2_set_ioctl_hook(mock); probe_only = 1; active_coded = V4L2_PIX_FMT_HEVC;
    uint32_t profiles;
    assert(vav4l2_query_h264_profiles("/dev/null", &profiles) == 0 && (profiles & (1u << V4L2_MPEG_VIDEO_H264_PROFILE_HIGH)));
    assert(active_coded == V4L2_PIX_FMT_H264);
    assert(vav4l2_query_profiles("/dev/null", V4L2_CID_MPEG_VIDEO_HEVC_PROFILE, &profiles) == 0 && profiles == (1u << V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN));
    assert(active_coded == V4L2_PIX_FMT_HEVC); vav4l2_set_ioctl_hook(NULL);
    init(&c, &dd, surfaces); submit(&c, &surfaces[0], &f, 0); submit(&c, &surfaces[1], &f, 1000);
    assert(captures == 1 && c.n_free_out == 4 && vav4l2_stateful_pending(&c, 0) && vav4l2_stateful_pending(&c, 1));
    result(1, 1000, 0, 384); result(0, 0, 0, 384); assert(vav4l2_stateful_pump(&c) == 0);
    assert(surfaces[0].status == VASurfaceReady && surfaces[1].status == VASurfaceReady && surfaces[0].buf_index == 0 && surfaces[1].buf_index == 1);
    assert(!stops && !starts && !vav4l2_stateful_pending(&c, 0)); finish(&c, surfaces);
    init(&c, &dd, surfaces); submit(&c, &surfaces[0], &f, 0);
    assert(vav4l2_stateful_sync(&c, &surfaces[0], 0) == VA_STATUS_ERROR_TIMEDOUT && !stops && vav4l2_stateful_pending(&c, 0));
    assert(vav4l2_stateful_sync(&c, &surfaces[0], 100000000) == VA_STATUS_SUCCESS && stops == 1 && starts == 1 && surfaces[0].status == VASurfaceReady); finish(&c, surfaces);
    init(&c, &dd, surfaces); hold_output = 1; submit(&c, &surfaces[0], &f, 0);
    assert(vav4l2_stateful_sync(&c, &surfaces[0], 100000000) == VA_STATUS_SUCCESS && stops == 1 && !starts && c.n_free_out == 3);
    hold_output = 0; assert(vav4l2_stateful_pump(&c) == 0 && starts == 1 && c.n_free_out == 4); finish(&c, surfaces);
    init(&c, &dd, surfaces); submit(&c, &surfaces[0], &f, 0); no_complete = 1;
    assert(vav4l2_stateful_sync(&c, &surfaces[0], 1000000) == VA_STATUS_ERROR_TIMEDOUT && vav4l2_stateful_pending(&c, 0) && c.v4l2_fd >= 0);
    result(0, 0, 0, 384); result(1, 0, V4L2_BUF_FLAG_LAST, 0); assert(vav4l2_stateful_sync(&c, &surfaces[0], 100000000) == VA_STATUS_SUCCESS && starts == 1); finish(&c, surfaces);
    init(&c, &dd, surfaces); submit(&c, &surfaces[0], &f, 0);
    result(0, 0, 0, 384); assert(vav4l2_stateful_pump(&c) == 0 && surfaces[0].cap_view);
    source = 1; result(1, 0, V4L2_BUF_FLAG_LAST, 0);
    assert(vav4l2_stateful_pump(&c) == 0 && captures == 2 && !output_stops && surfaces[0].has_pic &&
        !surfaces[0].cap_view && surfaces[0].buf_index == -1 && surfaces[0].memfd_map); finish(&c, surfaces);
    init(&c, &dd, surfaces); submit(&c, &surfaces[0], &f, 0); stop_error = 1;
    assert(vav4l2_stateful_sync(&c, &surfaces[0], 100000000) == VA_STATUS_ERROR_DECODING_ERROR && c.v4l2_fd == -1); finish(&c, surfaces);
    for (unsigned error = 0; error < 5; error++) {
        init(&c, &dd, surfaces); submit(&c, &surfaces[0], &f, 0);
        if (error == 0) result(0, 999000, 0, 384);
        if (error == 1) { result(0, 0, 0, 384); short_buffer = 1; }
        if (error == 2) result(0, 0, V4L2_BUF_FLAG_ERROR, 384);
        if (error == 3) result(0, 0, V4L2_BUF_FLAG_LAST, 0);
        if (error == 4) extra_event = 1;
        assert(vav4l2_stateful_pump(&c) < 0 && c.v4l2_fd == -1 && surfaces[0].status == VASurfaceSkipped); finish(&c, surfaces);
    }
    struct VADriverVTable table = {0}; struct VADriverVTableVPP vpp = {0};
    struct VADriverContext driver = {.vtable = &table, .vtable_vpp = &vpp};
    assert(__vaDriverInit_1_20(&driver) == VA_STATUS_SUCCESS); void *original = driver.pDriverData;
    init(&c, &dd, surfaces); c.context_id = 1; dd.contexts = &c; driver.pDriverData = &dd;
    submit(&c, &surfaces[0], &f, 0); VASurfaceID id = 0; VASurfaceStatus status;
    assert(table.vaBeginPicture(&driver, 1, 0) == VA_STATUS_ERROR_SURFACE_BUSY);
    assert(table.vaDestroySurfaces(&driver, &id, 1) == VA_STATUS_ERROR_SURFACE_BUSY && dd.surfaces[0] == &surfaces[0]);
    assert(table.vaSyncSurface2(&driver, 0, 0) == VA_STATUS_ERROR_TIMEDOUT && !stops);
    assert(table.vaQuerySurfaceStatus(&driver, 0, &status) == VA_STATUS_SUCCESS && status == VASurfaceRendering);
    result(0, 0, 0, 384);
    assert(table.vaQuerySurfaceStatus(&driver, 0, &status) == VA_STATUS_SUCCESS && status == VASurfaceReady);
    finish(&c, surfaces); driver.pDriverData = original; table.vaTerminate(&driver);
    puts("stateful queue order, drain, deadlines, reconfiguration and VA lifetime: PASS"); return 0;
}
