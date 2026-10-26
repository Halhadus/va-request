/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Normalize the single-planar UAPI at the device boundary. Internally, queue
 * helpers use the multi-planar structures with one physical plane. */
#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <sys/stat.h>
#include "va_v4l2.h"
struct queue_api { int active, fd; dev_t dev, rdev; ino_t ino; };
static struct queue_api single_planar[VAV4L2_MAX_SURFACES / 16];
static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
static int remember(int fd, int single)
{
    struct stat st;
    if (fstat(fd, &st) < 0) return single ? -1 : 0;
    pthread_mutex_lock(&queue_lock);
    int slot = -1;
    for (unsigned i = 0; i < sizeof(single_planar) / sizeof(*single_planar); i++) {
        if (single_planar[i].active && single_planar[i].fd == fd) { slot = i; break; }
        if (!single_planar[i].active && slot < 0) slot = i;
    }
    if (slot >= 0) single_planar[slot] = (struct queue_api){.active = single, .fd = fd, .dev = st.st_dev, .rdev = st.st_rdev, .ino = st.st_ino};
    pthread_mutex_unlock(&queue_lock);
    if (slot < 0 && single) { errno = ENOSPC; return -1; }
    return 0;
}
void vav4l2_queue_forget(int fd) { int saved = errno; remember(fd, 0); errno = saved; }
static int is_single(int fd)
{
    struct stat st;
    int saved = errno, found = 0;
    pthread_mutex_lock(&queue_lock);
    for (unsigned i = 0; i < sizeof(single_planar) / sizeof(*single_planar); i++) if (single_planar[i].active && single_planar[i].fd == fd) {
        if (fstat(fd, &st) == 0 && st.st_dev == single_planar[i].dev && st.st_ino == single_planar[i].ino && st.st_rdev == single_planar[i].rdev) found = 1;
        else single_planar[i].active = 0;
        break;
    }
    pthread_mutex_unlock(&queue_lock); errno = saved; return found;
}
static unsigned queue_type(unsigned type)
{
    return type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE ? V4L2_BUF_TYPE_VIDEO_OUTPUT :
        type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE ? V4L2_BUF_TYPE_VIDEO_CAPTURE : type;
}
static int format_to_single(const struct v4l2_format *in, struct v4l2_format *out)
{
    *out = *in; out->type = queue_type(in->type);
    if (out->type == in->type) return 0;
    const struct v4l2_pix_format_mplane *m = &in->fmt.pix_mp;
    if (m->num_planes > 1) { errno = ENOTSUP; return -1; }
    out->fmt.pix = (struct v4l2_pix_format){.width = m->width, .height = m->height, .pixelformat = m->pixelformat,
        .field = m->field, .bytesperline = m->plane_fmt[0].bytesperline, .sizeimage = m->plane_fmt[0].sizeimage,
        .colorspace = m->colorspace, .priv = V4L2_PIX_FMT_PRIV_MAGIC, .flags = m->flags, .ycbcr_enc = m->ycbcr_enc, .quantization = m->quantization,
        .xfer_func = m->xfer_func};
    return 0;
}
static void format_from_single(struct v4l2_format *out, const struct v4l2_format *in)
{
    if (queue_type(out->type) == out->type) { *out = *in; return; }
    const struct v4l2_pix_format *s = &in->fmt.pix;
    out->fmt.pix_mp = (struct v4l2_pix_format_mplane){.width = s->width, .height = s->height, .pixelformat = s->pixelformat,
        .field = s->field, .colorspace = s->colorspace, .num_planes = 1, .flags = s->flags,
        .ycbcr_enc = s->ycbcr_enc, .quantization = s->quantization, .xfer_func = s->xfer_func};
    out->fmt.pix_mp.plane_fmt[0].bytesperline = s->bytesperline;
    out->fmt.pix_mp.plane_fmt[0].sizeimage = s->sizeimage;
}
int vav4l2_queue_ioctl(int fd, unsigned long cmd, void *arg, vav4l2_ioctl_fn call)
{
    if (cmd == VIDIOC_QUERYCAP) {
        int rc = call(fd, cmd, arg);
        if (rc < 0 || !arg) return rc;
        const struct v4l2_capability *cap = arg;
        unsigned caps = cap->capabilities & V4L2_CAP_DEVICE_CAPS ? cap->device_caps : cap->capabilities;
        int saved = errno;
        int stored = remember(fd, (caps & V4L2_CAP_VIDEO_M2M) && !(caps & V4L2_CAP_VIDEO_M2M_MPLANE));
        if (!stored) errno = saved;
        return stored;
    }
    if (!is_single(fd)) return call(fd, cmd, arg);
    if (cmd == VIDIOC_G_FMT || cmd == VIDIOC_S_FMT || cmd == VIDIOC_TRY_FMT) {
        struct v4l2_format f;
        if (format_to_single(arg, &f) < 0) return -1;
        int rc = call(fd, cmd, &f); if (rc >= 0) format_from_single(arg, &f); return rc;
    }
    if (cmd == VIDIOC_CREATE_BUFS) {
        struct v4l2_create_buffers *out = arg, b = *out;
        if (format_to_single(&out->format, &b.format) < 0) return -1;
        int rc = call(fd, cmd, &b);
        if (rc >= 0) { unsigned type = out->format.type; *out = b; out->format.type = type; format_from_single(&out->format, &b.format); }
        return rc;
    }
    if (cmd == VIDIOC_QUERYBUF || cmd == VIDIOC_QBUF || cmd == VIDIOC_DQBUF || cmd == VIDIOC_PREPARE_BUF) {
        struct v4l2_buffer *out = arg, b = *out;
        if (queue_type(b.type) == b.type) return call(fd, cmd, arg);
        int query = cmd == VIDIOC_QUERYBUF || cmd == VIDIOC_DQBUF;
        if (!b.length || (!query && b.length != 1) || !b.m.planes || (!query && b.m.planes[0].data_offset)) { errno = ENOTSUP; return -1; }
        struct v4l2_plane *plane = b.m.planes;
        b.type = queue_type(b.type); b.length = plane->length; b.bytesused = plane->bytesused;
        memset(&b.m, 0, sizeof(b.m));
        if (b.memory == V4L2_MEMORY_DMABUF) b.m.fd = plane->m.fd;
        else if (b.memory == V4L2_MEMORY_USERPTR) b.m.userptr = plane->m.userptr;
        else b.m.offset = plane->m.mem_offset;
        int rc = call(fd, cmd, &b);
        if (rc >= 0) {
            unsigned type = out->type; *out = b; out->type = type; out->m.planes = plane; out->length = 1;
            memset(plane, 0, sizeof(*plane)); plane->length = b.length; plane->bytesused = b.bytesused;
            if (b.memory == V4L2_MEMORY_DMABUF) plane->m.fd = b.m.fd;
            else if (b.memory == V4L2_MEMORY_USERPTR) plane->m.userptr = b.m.userptr;
            else plane->m.mem_offset = b.m.offset;
        }
        return rc;
    }
    /* These structures all begin with the queue type, except ENUM_FMT and
     * EXPBUF. Work on a copy so an error cannot mutate the caller's type. */
#define TYPE_CALL(type, field) do { type value = *(type *)arg; unsigned original = value.field; value.field = queue_type(original); \
    int rc = call(fd, cmd, &value); if (rc >= 0) { value.field = original; *(type *)arg = value; } return rc; } while (0)
    if (cmd == VIDIOC_ENUM_FMT) TYPE_CALL(struct v4l2_fmtdesc, type);
    if (cmd == VIDIOC_EXPBUF) TYPE_CALL(struct v4l2_exportbuffer, type);
    if (cmd == VIDIOC_REQBUFS) TYPE_CALL(struct v4l2_requestbuffers, type);
    if (cmd == VIDIOC_STREAMON || cmd == VIDIOC_STREAMOFF) {
        enum v4l2_buf_type type = queue_type(*(enum v4l2_buf_type *)arg); return call(fd, cmd, &type);
    }
    if (cmd == VIDIOC_G_SELECTION || cmd == VIDIOC_S_SELECTION) TYPE_CALL(struct v4l2_selection, type);
#undef TYPE_CALL
    return call(fd, cmd, arg);
}
