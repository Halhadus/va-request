/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include "va_v4l2.h"
#include "probe.h"
static unsigned calls;
static int raw(int fd, unsigned long cmd, void *arg)
{
    (void)fd; calls++;
    if (cmd == VIDIOC_QUERYCAP) {
        struct v4l2_capability *c = arg; c->capabilities = V4L2_CAP_DEVICE_CAPS;
        c->device_caps = V4L2_CAP_STREAMING | V4L2_CAP_VIDEO_M2M; return 0;
    }
    if (cmd == VIDIOC_S_FMT || cmd == VIDIOC_G_FMT) {
        struct v4l2_format *f = arg; assert(f->type == V4L2_BUF_TYPE_VIDEO_CAPTURE);
        f->fmt.pix.width = 320; f->fmt.pix.height = 240; f->fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
        f->fmt.pix.bytesperline = 384; f->fmt.pix.sizeimage = 384 * 360; return 0;
    }
    if (cmd == VIDIOC_CREATE_BUFS) {
        struct v4l2_create_buffers *b = arg; assert(b->format.type == V4L2_BUF_TYPE_VIDEO_CAPTURE);
        b->capabilities = V4L2_BUF_CAP_SUPPORTS_REQUESTS; return 0;
    }
    if (cmd == VIDIOC_QBUF || cmd == VIDIOC_DQBUF || cmd == VIDIOC_QUERYBUF) {
        struct v4l2_buffer *b = arg; assert(b->type == V4L2_BUF_TYPE_VIDEO_OUTPUT && b->memory == V4L2_MEMORY_DMABUF);
        assert(b->m.fd == 31 && b->length == 4096 && b->bytesused == 32 && b->request_fd == 44);
        assert((b->flags & V4L2_BUF_FLAG_REQUEST_FD) && b->timestamp.tv_usec == 17);
        b->index = 2; b->bytesused = 48; b->length = 8192; return 0;
    }
    if (cmd == VIDIOC_REQBUFS) { assert(((struct v4l2_requestbuffers *)arg)->type == V4L2_BUF_TYPE_VIDEO_CAPTURE); return 0; }
    if (cmd == VIDIOC_STREAMON || cmd == VIDIOC_STREAMOFF) { assert(*(unsigned *)arg == V4L2_BUF_TYPE_VIDEO_OUTPUT); return 0; }
    if (cmd == VIDIOC_EXPBUF) { struct v4l2_exportbuffer *b = arg; assert(b->type == V4L2_BUF_TYPE_VIDEO_CAPTURE && !b->plane); b->fd = 17; return 0; }
    assert(!"unexpected ioctl"); return -1;
}
int main(void)
{
    int fd = open("/dev/null", O_RDWR | O_CLOEXEC); assert(fd >= 0);
    struct v4l2_capability cap = {0}; assert(!vav4l2_queue_ioctl(fd, VIDIOC_QUERYCAP, &cap, raw)); assert(vav4l2_node_is_m2m(&cap));
    struct v4l2_format f = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE};
    assert(!vav4l2_queue_ioctl(fd, VIDIOC_G_FMT, &f, raw));
    assert(f.type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE && f.fmt.pix_mp.num_planes == 1 &&
        f.fmt.pix_mp.width == 320 && f.fmt.pix_mp.plane_fmt[0].bytesperline == 384 && f.fmt.pix_mp.plane_fmt[0].sizeimage == 384 * 360);
    assert(!vav4l2_queue_ioctl(fd, VIDIOC_S_FMT, &f, raw));
    struct v4l2_create_buffers create = {.format = f};
    assert(!vav4l2_queue_ioctl(fd, VIDIOC_CREATE_BUFS, &create, raw) && create.format.type == f.type && create.format.fmt.pix_mp.width == 320);
    struct v4l2_plane plane = {.length = 4096, .bytesused = 32, .m.fd = 31};
    struct v4l2_buffer b = {.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, .memory = V4L2_MEMORY_DMABUF,
        .length = 1, .m.planes = &plane, .request_fd = 44, .flags = V4L2_BUF_FLAG_REQUEST_FD, .timestamp.tv_usec = 17};
    assert(!vav4l2_queue_ioctl(fd, VIDIOC_QBUF, &b, raw));
    assert(b.type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE && b.length == 1 && b.m.planes == &plane && plane.m.fd == 31 &&
        plane.length == 8192 && plane.bytesused == 48 && b.index == 2);
    unsigned before = calls; plane.data_offset = 1;
    assert(vav4l2_queue_ioctl(fd, VIDIOC_QBUF, &b, raw) < 0 && errno == ENOTSUP && calls == before);
    struct v4l2_plane query_planes[8] = {{.length = 4096, .bytesused = 32, .m.fd = 31}};
    struct v4l2_buffer query = {.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, .memory = V4L2_MEMORY_DMABUF,
        .length = 8, .m.planes = query_planes, .request_fd = 44, .flags = V4L2_BUF_FLAG_REQUEST_FD, .timestamp.tv_usec = 17};
    assert(!vav4l2_queue_ioctl(fd, VIDIOC_QUERYBUF, &query, raw) && query.length == 1 && query_planes[0].length == 8192);
    before = calls;
    f.fmt.pix_mp.num_planes = 2; assert(vav4l2_queue_ioctl(fd, VIDIOC_S_FMT, &f, raw) < 0 && calls == before);
    struct v4l2_requestbuffers req = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE};
    assert(!vav4l2_queue_ioctl(fd, VIDIOC_REQBUFS, &req, raw) && req.type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    assert(!vav4l2_queue_ioctl(fd, VIDIOC_STREAMON, &type, raw) && type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
    struct v4l2_exportbuffer export = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE};
    assert(!vav4l2_queue_ioctl(fd, VIDIOC_EXPBUF, &export, raw) && export.fd == 17 && export.type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
    vav4l2_queue_forget(fd); close(fd);
    vav4l2_set_ioctl_hook(raw); fd = vav4l2_open_device("/dev/null"); assert(fd >= 0);
    f = (struct v4l2_format){.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE};
    assert(!vav4l2_xioctl(fd, VIDIOC_G_FMT, &f) && f.fmt.pix_mp.num_planes == 1 && f.fmt.pix_mp.plane_fmt[0].bytesperline == 384);
    vav4l2_queue_forget(fd); close(fd); vav4l2_set_ioctl_hook(NULL);
    puts("Single-planar formats, queue buffers, Request flags and DMA-BUF export: PASS"); return 0;
}
