/* Shared V4L2 device, queue and capture storage helpers. */

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/media.h>
#include <linux/videodev2.h>
#include <va/va_drmcommon.h>
#include <drm_fourcc.h>

#include "va_v4l2.h"
#include "probe.h"
#include "internal.h"
#include "memory/gpu_copy.h"

static vav4l2_ioctl_fn g_ioctl_hook;

void vav4l2_set_ioctl_hook(vav4l2_ioctl_fn fn)
{
    g_ioctl_hook = fn;
}

/* Optional structured evidence,independent of stderr. Never change the
 * caller's errno or ioctl result when opening/writing diagnostics fails. */
static void decode_trace_write(const struct vav4l2_context *ctx, int fd, const char *stage, int status, uint64_t argument, const struct v4l2_buffer *buffer)
{
    int saved = errno; const char *path = getenv("VAV4L2_DECODE_TRACE");
    if (path && *path) {
        int out = open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
        if (out >= 0) {
            const struct vav4l2_surface *surf = ctx ? ctx->current_surface : NULL;
            unsigned bytes = buffer ? buffer->bytesused : 0;
            if (buffer && (buffer->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE || buffer->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) && buffer->length && buffer->m.planes)
                bytes = buffer->m.planes[0].bytesused;
            char line[1024];
            int n = snprintf(line, sizeof(line), "STAGE=%s\tPID=%ld\tFD=%d\tCONTEXT=%u\tCODEC=%d\tFRAME=%llu\tSURFACE=%u\tSTATUS=%d\tARG=%llu\tERRNO=%d\tALLOCATED=%d\tFREE=%d\tMEMORY=%u\tWIDTH=%d\tHEIGHT=%d\tSTRIDE=%u\tCAP_HEIGHT=%u\tSIZEIMAGE=%u\tINDEX=%d\tWRITER=%d\tBORROWS=%u\tPOOL=%d\tTYPE=%u\tBUFFER_INDEX=%u\tFLAGS=%u\tBYTESUSED=%u\tTS_SEC=%lld\tTS_USEC=%lld\n",
                stage, (long)getpid(), fd, ctx ? ctx->context_id : VA_INVALID_ID, ctx ? (int)ctx->codec : -1,
                (unsigned long long)(ctx ? ctx->frame_count : 0), surf ? surf->surface_id : VA_INVALID_ID, status,
                (unsigned long long)argument, status < 0 ? saved : 0, ctx ? ctx->capture_bufs_allocd : 0, ctx ? ctx->n_free_cap : 0,
                ctx ? ctx->capture_memory : 0, ctx ? ctx->width : 0, ctx ? ctx->height : 0, ctx ? ctx->cap_stride : 0,
                ctx ? ctx->cap_height : 0, ctx ? ctx->cap_sizeimage : 0, surf ? surf->buf_index : -1,
                surf ? (int)surf->last_writer : -1, surf ? surf->memfd_borrows : 0, ctx ? ctx->capture_internal : 0,
                buffer ? buffer->type : 0, buffer ? buffer->index : 0, buffer ? buffer->flags : 0, bytes,
                (long long)(buffer ? buffer->timestamp.tv_sec : 0), (long long)(buffer ? buffer->timestamp.tv_usec : 0));
            if (n > 0 && (size_t)n < sizeof(line)) { ssize_t written; do { written = write(out, line, n); } while (written < 0 && errno == EINTR); }
            close(out);
        }
    }
    errno = saved;
}
void vav4l2_decode_trace(const struct vav4l2_context *ctx, const char *stage, int status, uint64_t argument)
{
    decode_trace_write(ctx, ctx ? ctx->v4l2_fd : -1, stage, status, argument, NULL);
}

static int raw_ioctl(int fd, unsigned long request, void *arg) { return ioctl(fd, request, arg); }
int vav4l2_ioctl_once(int fd, unsigned long request, void *arg)
{
    int result = vav4l2_queue_ioctl(fd, request, arg, g_ioctl_hook ? g_ioctl_hook : raw_ioctl);
    decode_trace_write(NULL, fd, "ioctl", result, request,
        !result && (request == VIDIOC_QBUF || request == VIDIOC_DQBUF) ? arg : NULL);
    return result;
}

/* Helper to call ioctl with retry (shared via the header). */
int vav4l2_xioctl(int fd, unsigned long request, void *arg)
{
    int r;
    do {
        r = vav4l2_ioctl_once(fd, request, arg);
    } while (r == -1 && errno == EINTR);
    return r;
}

#define xioctl(fd, req, arg) vav4l2_xioctl((fd), (req), (arg))

/* poll() with EINTR retry — a stray signal (profiler, SIGWINCH) must never
 * be mistaken for a decode timeout; the decode path turns timeouts into
 * full STREAMOFF/STREAMON queue resets. */
int vav4l2_poll_intr(struct pollfd *fds, nfds_t n, int timeout)
{
    struct timespec start, now;
    if (timeout >= 0 && clock_gettime(CLOCK_MONOTONIC, &start) < 0) return -1;
    int remaining = timeout;
    for (;;) {
        int r = poll(fds, n, remaining);
        if (r >= 0 || errno != EINTR) {
            decode_trace_write(NULL, n ? fds[0].fd : -1, "poll", r, ((uint64_t)(uint32_t)timeout << 32) | (n ? (unsigned short)fds[0].revents : 0), NULL);
            return r;
        }
        if (timeout < 0) continue;
        if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return -1;
        int64_t left = (int64_t)timeout * 1000000 - ((int64_t)(now.tv_sec - start.tv_sec) * 1000000000 + now.tv_nsec - start.tv_nsec);
        if (left <= 0) { for (nfds_t i = 0; i < n; i++) fds[i].revents = 0; return 0; }
        remaining = (left + 999999) / 1000000;
    }
}

/*
 * Open a V4L2 video device and verify M2M streaming capabilities
 */
int vav4l2_open_device(const char *path)
{
    int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "v4l2: open %s: %s\n", path, strerror(errno));
        return -1;
    }

    /* Verify capabilities */
    struct v4l2_capability cap = { 0 };
    if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
        fprintf(stderr, "v4l2: QUERYCAP failed on %s: %s\n", path, strerror(errno));
        close(fd);
        return -1;
    }

    uint32_t caps = cap.capabilities & V4L2_CAP_DEVICE_CAPS ? cap.device_caps : cap.capabilities;
    if (!(caps & (V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_VIDEO_M2M)) || !(caps & V4L2_CAP_STREAMING)) {
        fprintf(stderr, "v4l2: %s is not an M2M streaming device\n", path);
        close(fd);
        return -1;
    }

    fprintf(stderr, "v4l2: opened %s (%s)\n", path, cap.card);
    return fd;
}

int vav4l2_codec_backend_init(struct vav4l2_context *ctx)
{
    if (!ctx || ctx->v4l2_fd < 0) { errno = EINVAL; return -1; }
    if (ctx->codec == VAV4L2_CODEC_VPP || !vav4l2_compat_backend_needed()) return 0;
    struct v4l2_capability cap = {0};
    if (xioctl(ctx->v4l2_fd, VIDIOC_QUERYCAP, &cap) < 0) return -1;
    struct vav4l2_compat_identity identity; vav4l2_compat_identity(&cap, &identity);
    struct vav4l2_compat_codec codec = {.direction = ctx->codec == VAV4L2_CODEC_JPEG_ENC ? VAV4L2_COMPAT_ENCODE : VAV4L2_COMPAT_DECODE,
        .va_profile = ctx->profile, .profile_known = 1};
    if (codec.direction == VAV4L2_COMPAT_ENCODE) codec.coded_format = V4L2_PIX_FMT_JPEG;
    else if (ctx->coded_format) codec.coded_format = ctx->coded_format;
    else if (vav4l2_codec_coded_fourcc(ctx->codec, &codec.coded_format) < 0) { errno = ENOTSUP; return -1; }
    return vav4l2_compat_backend_init(&ctx->compat_backend, &identity, &codec, ctx->v4l2_fd);
}

void vav4l2_close_context_device(struct vav4l2_context *ctx)
{
    int saved = errno;
    vav4l2_compat_backend_finish(&ctx->compat_backend, ctx->v4l2_fd);
    if (ctx->v4l2_fd >= 0) { vav4l2_queue_forget(ctx->v4l2_fd); close(ctx->v4l2_fd); }
    ctx->v4l2_fd = -1; errno = saved;
}

int vav4l2_codec_format(struct vav4l2_context *ctx, struct v4l2_format *format)
{
    if (vav4l2_compat_prepare_format(&ctx->compat_backend, format) < 0) return -1;
    return xioctl(ctx->v4l2_fd, VIDIOC_S_FMT, format);
}

static int codec_control(struct vav4l2_context *ctx, int fd, struct v4l2_control *control)
{
    if (ctx) {
        struct v4l2_ext_control item = {.id = control->id, .value = control->value};
        struct v4l2_ext_controls controls = {.which = V4L2_CTRL_WHICH_CUR_VAL, .count = 1, .controls = &item};
        if (vav4l2_compat_prepare_controls(&ctx->compat_backend, &controls) < 0) return -1;
        control->value = item.value;
    }
    return xioctl(fd, VIDIOC_S_CTRL, control);
}

int vav4l2_codec_control(struct vav4l2_context *ctx, struct v4l2_control *control)
{
    return codec_control(ctx, ctx->v4l2_fd, control);
}

int vav4l2_codec_prepare_controls(struct vav4l2_context *ctx, int request_fd, struct v4l2_ext_controls *controls)
{
    controls->which = request_fd >= 0 ? V4L2_CTRL_WHICH_REQUEST_VAL : V4L2_CTRL_WHICH_CUR_VAL;
    controls->request_fd = request_fd >= 0 ? request_fd : 0;
    return vav4l2_compat_prepare_controls(&ctx->compat_backend, controls);
}

int vav4l2_codec_controls(struct vav4l2_context *ctx, int request_fd, struct v4l2_ext_controls *controls)
{
    if (request_fd < 0 && vav4l2_request_drain(ctx) < 0) return -1;
    if (vav4l2_codec_prepare_controls(ctx, request_fd, controls) < 0) return -1;
    return request_fd >= 0 ? vav4l2_set_request_controls(request_fd, ctx->v4l2_fd, controls) : vav4l2_set_global_controls(ctx->v4l2_fd, controls);
}

/*
 * Open the media device associated with a V4L2 video device
 * Match the media graph interface to the video node.
 */
int vav4l2_open_media_for_device(const char *video_path)
{
    char media_path[128];
    int fd;

    if (vav4l2_find_media_path(video_path, media_path, sizeof(media_path)) < 0) {
        fprintf(stderr, "v4l2: no media graph interface for %s\n", video_path);
        return -1;
    }

    fd = open(media_path, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        fprintf(stderr, "v4l2: open %s (for %s): %s\n",
                media_path, video_path, strerror(errno));
    else
        fprintf(stderr, "v4l2: media for %s is %s\n", video_path, media_path);
    return fd;
}

/*
 * Setup output queue (compressed bitstream input)
 * Returns 0 on success, -1 on error
 */
static int setup_output_queue(struct vav4l2_context *ctx, int fd, uint32_t codec_format, int width, int height)
{
    if (width <= 0 || height <= 0) return -1;
    /* Set output format (compressed input) */
    struct v4l2_format fmt = { 0 };
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    fmt.fmt.pix_mp.width = width;
    fmt.fmt.pix_mp.height = height;
    fmt.fmt.pix_mp.pixelformat = codec_format;
    fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    fmt.fmt.pix_mp.num_planes = 1;
    /* hantro coded formats want sizeimage ~= luma size, not a 4MB guess.
     * GStreamer sizes H.264 OUTPUT as (width*height*pixel_bitdepth)/8;
     * 4:2:0 8-bit=12, 10-bit=15. A luma-sized buffer is too small for
     * 10-bit High10 and rkvdec then writes an empty NV15 capture. */
    uint64_t coded_size = 0;
    if (codec_format == V4L2_PIX_FMT_AV1_FRAME ||
        codec_format == V4L2_PIX_FMT_VP8_FRAME ||
        codec_format == V4L2_PIX_FMT_VP9_FRAME ||
        codec_format == V4L2_PIX_FMT_MPEG2_SLICE)
        coded_size = (uint64_t)width * (uint64_t)height;
    else if (codec_format == V4L2_PIX_FMT_H264_SLICE)
        coded_size = (uint64_t)width * (uint64_t)height * 15 / 8;
    else if (codec_format == V4L2_PIX_FMT_H264 || codec_format == V4L2_PIX_FMT_HEVC)
        coded_size = (uint64_t)width * (uint64_t)height * 2 + 8192;
    else if (codec_format == V4L2_PIX_FMT_AV1)
        coded_size = (uint64_t)width * (uint64_t)height * 4 + 16384;
    if (coded_size > UINT32_MAX) return -1;
    fmt.fmt.pix_mp.plane_fmt[0].sizeimage = coded_size;

    if ((ctx ? vav4l2_codec_format(ctx, &fmt) : xioctl(fd, VIDIOC_S_FMT, &fmt)) < 0) {
        fprintf(stderr, "v4l2: S_FMT output failed: %s\n", strerror(errno));
        return -1;
    }
    if (fmt.fmt.pix_mp.pixelformat != codec_format || fmt.fmt.pix_mp.num_planes != 1 ||
        fmt.fmt.pix_mp.width < (unsigned)width || fmt.fmt.pix_mp.height < (unsigned)height) {
        fprintf(stderr, "v4l2: coded format/geometry changed by the selected decoder\n");
        return -1;
    }

    /* Set codec-specific controls */
    struct v4l2_control ctrl = { 0 };

    if (codec_format == V4L2_PIX_FMT_H264_SLICE || codec_format == V4L2_PIX_FMT_HEVC_SLICE) {
        enum vav4l2_codec codec = codec_format == V4L2_PIX_FMT_H264_SLICE ? VAV4L2_CODEC_H264 : VAV4L2_CODEC_HEVC;
        int slice, raw;
        if (vav4l2_request_modes(fd, codec, &slice, &raw) < 0) return -1;
        ctrl.id = codec == VAV4L2_CODEC_H264 ? V4L2_CID_STATELESS_H264_DECODE_MODE : V4L2_CID_STATELESS_HEVC_DECODE_MODE;
        ctrl.value = slice ? 0 : 1;
        if (codec_control(ctx, fd, &ctrl) < 0) return -1;
        ctrl.id = codec == VAV4L2_CODEC_H264 ? V4L2_CID_STATELESS_H264_START_CODE : V4L2_CID_STATELESS_HEVC_START_CODE;
        ctrl.value = raw ? 0 : 1;
        if (codec_control(ctx, fd, &ctrl) < 0) return -1;
        if (ctx) { ctx->request_slice_based = slice; ctx->request_no_start_code = raw; }
    }

    /* Decode waits for OUTPUT DQBUF before the next submission. Four slots
     * are an optimization,not a DPB requirement;retry smaller pools only for
     * allocation pressure. Never shrink an individual coded buffer. */
    for (unsigned count = VAV4L2_NUM_OUTPUT_BUFS; count; count /= 2) {
        struct v4l2_requestbuffers req = {.count = count, .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, .memory = V4L2_MEMORY_MMAP};
        int result = xioctl(fd, VIDIOC_REQBUFS, &req);
        decode_trace_write(NULL, fd, "output-allocate", result, ((uint64_t)count << 32) | req.count, NULL);
        if (result == 0 && req.count) {
            fprintf(stderr, "v4l2: output queue: %u buffers\n", req.count);
            return req.count < VAV4L2_NUM_OUTPUT_BUFS ? req.count : VAV4L2_NUM_OUTPUT_BUFS;
        }
        int error = result < 0 ? errno : ENOMEM;
        if (error != ENOMEM) {
            fprintf(stderr, "v4l2: REQBUFS output failed: %s\n", strerror(error));
            errno = error; return -1;
        }
        struct v4l2_requestbuffers release = {.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, .memory = V4L2_MEMORY_MMAP};
        if (xioctl(fd, VIDIOC_REQBUFS, &release) < 0) {
            fprintf(stderr, "v4l2: REQBUFS(0) output retry failed: %s\n", strerror(errno));
            return -1;
        }
        if (count > 1) fprintf(stderr, "v4l2: retrying OUTPUT allocation with %u buffers\n", count / 2);
    }
    fprintf(stderr, "v4l2: REQBUFS output exhausted allocation retries\n");
    errno = ENOMEM; return -1;
}

int vav4l2_setup_output_queue(int fd, uint32_t codec_format, int width, int height)
{
    return setup_output_queue(NULL, fd, codec_format, width, height);
}

int vav4l2_setup_codec_output(struct vav4l2_context *ctx, uint32_t codec_format, int width, int height)
{
    return setup_output_queue(ctx, ctx->v4l2_fd, codec_format, width, height);
}

/*
 * Setup capture queue (decoded frames output)
 * Returns 0 on success, -1 on error
 */
int vav4l2_setup_capture_queue(int fd, int width, int height, uint32_t pixelformat)
{
    return vav4l2_setup_capture_queue_count(fd, width, height, pixelformat,
                                            VAV4L2_NUM_CAPTURE_BUFS);
}

static int setup_capture_queue(struct vav4l2_context *ctx, int fd, int width, int height,
                                     uint32_t pixelformat, int count)
{
    if (width <= 0 || height <= 0 || !vav4l2_drm_fourcc_for_capture(pixelformat)) { errno = ENOTSUP; return -1; }
    /* Set capture format (decoded output). Default NV12; 10-bit/422 use
     * NV15/NV16/NV20 which rkvdec only accepts after a matching SPS. */
    struct v4l2_format fmt = { 0 };
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.width = width;
    fmt.fmt.pix_mp.height = height;
    fmt.fmt.pix_mp.pixelformat = pixelformat ? pixelformat : V4L2_PIX_FMT_NV12;
    fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    fmt.fmt.pix_mp.num_planes = 1;

    int formatted = ctx ? vav4l2_codec_format(ctx, &fmt) : xioctl(fd, VIDIOC_S_FMT, &fmt);
    if (formatted < 0 && ctx && (errno == EINVAL || errno == ENOTSUP)) {
        uint32_t menu[VAV4L2_MAX_FORMATS]; int n = vav4l2_enum_capture_fourccs(fd, menu, VAV4L2_MAX_FORMATS);
        for (int i = 0; i < n; i++) {
            if (vav4l2_capture_layout_format(menu[i]) != pixelformat) continue;
            fmt.fmt.pix_mp.pixelformat = menu[i]; fmt.fmt.pix_mp.num_planes = vav4l2_capture_layout_planes(menu[i]);
            formatted = vav4l2_codec_format(ctx, &fmt);
            if (formatted == 0 || (errno != EINVAL && errno != ENOTSUP)) break;
        }
    }
    if (formatted < 0 || fmt.fmt.pix_mp.width < (unsigned)width || fmt.fmt.pix_mp.height < (unsigned)height) return -1;
    if (ctx && vav4l2_capture_layout_format(fmt.fmt.pix_mp.pixelformat) == pixelformat) {
        if (ctx->capture_bufs_allocd) { errno = EBUSY; return -1; }
        vav4l2_capture_layout_release(ctx);
        if (vav4l2_capture_layout_set(ctx, &fmt.fmt.pix_mp) < 0) return -1;
    } else if (fmt.fmt.pix_mp.pixelformat != (pixelformat ? pixelformat : V4L2_PIX_FMT_NV12) || fmt.fmt.pix_mp.num_planes != 1) {
        errno = ENOTSUP; return -1;
    }

    /* Request capture buffers */
    struct v4l2_requestbuffers req = { 0 };
    req.count = count > 0 ? count : VAV4L2_NUM_CAPTURE_BUFS;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;

    if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
        fprintf(stderr, "v4l2: REQBUFS capture failed: %s\n", strerror(errno));
        return -1;
    }

    fprintf(stderr, "v4l2: capture queue: %d buffers\n", req.count);
    return req.count < VAV4L2_MAX_CAPTURE_BUFS ? req.count : VAV4L2_MAX_CAPTURE_BUFS;
}

int vav4l2_setup_capture_queue_count(int fd, int width, int height, uint32_t pixelformat, int count)
{
    return setup_capture_queue(NULL, fd, width, height, pixelformat, count);
}

int vav4l2_setup_codec_capture(struct vav4l2_context *ctx, int width, int height, uint32_t pixelformat, int count)
{
    return setup_capture_queue(ctx, ctx->v4l2_fd, width, height, pixelformat, count);
}

/*
 * Start streaming on a queue — required after REQBUFS and before QBUF
 * Returns 0 on success, -1 on error
 */
int vav4l2_streamon(int fd, enum v4l2_buf_type type)
{
    if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) {
        fprintf(stderr, "v4l2: STREAMON(type=%u) failed: %s\n",
                type, strerror(errno));
        return -1;
    }
    return 0;
}

int vav4l2_streamoff(int fd, enum v4l2_buf_type type)
{
    if (xioctl(fd, VIDIOC_STREAMOFF, &type) < 0) {
        fprintf(stderr, "v4l2: STREAMOFF(type=%u) failed: %s\n",
                type, strerror(errno));
        return -1;
    }
    return 0;
}

static int vav4l2_mmap_one_capture(struct vav4l2_context *ctx, int index);

int vav4l2_release_queue(int fd, enum v4l2_buf_type type, unsigned memory)
{
    struct v4l2_requestbuffers req = { 0 };

    req.count = 0;
    req.type = type;
    req.memory = memory ? memory : V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
        fprintf(stderr, "v4l2: REQBUFS(0) type=%u failed: %s\n",
                type, strerror(errno));
        return -1;
    }
    return 0;
}

static int reqbufs_zero(int fd, enum v4l2_buf_type type)
{
    return vav4l2_release_queue(fd, type, V4L2_MEMORY_MMAP);
}

int vav4l2_m2m_finish_output(struct vav4l2_context *ctx, const struct vav4l2_m2m_state *q)
{
    if (!ctx || !q || q->out_planes < 1 || q->out_planes > 3) return -1;
    struct v4l2_plane planes[3] = {{0}};
    struct v4l2_buffer buffer = {.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
        .memory = q->out_memory ? q->out_memory : V4L2_MEMORY_MMAP, .length = q->out_planes, .m.planes = planes};
    int result = xioctl(ctx->v4l2_fd, VIDIOC_DQBUF, &buffer);
    if (result < 0 && errno == EAGAIN) {
        struct pollfd pfd = {.fd = ctx->v4l2_fd, .events = POLLOUT};
        if (vav4l2_poll_intr(&pfd, 1, 2000) > 0 && (pfd.revents & POLLOUT) &&
            !(pfd.revents & (POLLERR | POLLHUP | POLLNVAL)))
            result = xioctl(ctx->v4l2_fd, VIDIOC_DQBUF, &buffer);
    }
    if (result < 0 || buffer.index != 0 || buffer.length != (unsigned)q->out_planes ||
        buffer.memory != (q->out_memory ? q->out_memory : V4L2_MEMORY_MMAP) ||
        (buffer.flags & V4L2_BUF_FLAG_ERROR)) {
        fprintf(stderr, "v4l2: M2M OUTPUT completion failed (index=%u flags=0x%x planes=%u)\n",
                buffer.index, buffer.flags, buffer.length);
        return -1;
    }
    return 0;
}

void vav4l2_m2m_teardown(struct vav4l2_context *ctx, struct vav4l2_m2m_state *q)
{
    int i;

    if (!ctx || !q || !q->valid)
        return;
    if (ctx->v4l2_fd >= 0) {
        int output = vav4l2_streamoff(ctx->v4l2_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
        int capture = vav4l2_streamoff(ctx->v4l2_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
        if ((output < 0 && (q->streaming & 1)) || (capture < 0 && (q->streaming & 2))) {
            vav4l2_close_context_device(ctx);
        }
    }
    for (i = 0; i < 3; i++) {
        if (q->out_map[i] && q->out_map[i] != MAP_FAILED)
            munmap(q->out_map[i], q->out_len[i]);
    }
    if (q->cap_map && q->cap_map != MAP_FAILED)
        munmap(q->cap_map, q->cap_len);
    if (ctx->v4l2_fd >= 0) {
        int output = vav4l2_release_queue(ctx->v4l2_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, q->out_memory);
        int capture = vav4l2_release_queue(ctx->v4l2_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, q->cap_memory);
        if (output < 0 || capture < 0) { vav4l2_close_context_device(ctx); }
    }
    memset(q, 0, sizeof(*q));
}

void vav4l2_release_context_device(struct vav4l2_context *ctx)
{
    int i;

    if (!ctx)
        return;

    vav4l2_h264_discard_field(ctx, 1);
    free(ctx->vp9_state); ctx->vp9_state = NULL;
    vav4l2_stateful_finish(ctx);
    vav4l2_packet_state_release(&ctx->packet_state);
    /* Persistent M2M queues hold V4L2 MMAP mappings — release them while
     * the device fd is still open. */
    vav4l2_m2m_teardown(ctx, &ctx->vpp_q);
    vav4l2_m2m_teardown(ctx, &ctx->jpeg_q);

    if (ctx->v4l2_fd >= 0 && ctx->streamed) {
        vav4l2_streamoff(ctx->v4l2_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
        vav4l2_streamoff(ctx->v4l2_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
        ctx->streamed = 0;
    }

    vav4l2_request_finish(ctx);
    if (ctx->request_fd >= 0) {
        close(ctx->request_fd);
        ctx->request_fd = -1;
    }

    for (i = 0; i < ctx->output_bufs_allocd && i < VAV4L2_NUM_OUTPUT_BUFS; i++) {
        if (ctx->output_buf_ptr[i] && ctx->output_buf_ptr[i] != MAP_FAILED) {
            munmap(ctx->output_buf_ptr[i], ctx->output_buf_size);
            ctx->output_buf_ptr[i] = NULL;
        }
    }
    vav4l2_release_capture_surfaces(ctx);
    vav4l2_unmap_capture_buffers(ctx);

    if (ctx->v4l2_fd >= 0) {
        if (ctx->output_bufs_allocd > 0) {
            reqbufs_zero(ctx->v4l2_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
            ctx->output_bufs_allocd = 0;
            ctx->n_free_out = 0;
        }
        if (ctx->capture_bufs_allocd > 0) {
            vav4l2_release_queue(ctx->v4l2_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, ctx->capture_memory);
            ctx->capture_bufs_allocd = 0;
            ctx->n_free_cap = 0;
        }
        vav4l2_close_context_device(ctx);
    }

    if (ctx->media_fd >= 0) {
        close(ctx->media_fd);
        ctx->media_fd = -1;
    }
}

void vav4l2_release_capture_surfaces(struct vav4l2_context *ctx)
{
    int i;

    if (!ctx || !ctx->driver_data)
        return;
    /* FFmpeg also allocates surfaces outside render_targets. Match live
     * capture views by mapping identity before any mapping is released. */
    for (i = 0; i < VAV4L2_MAX_SURFACES; i++) {
        struct vav4l2_surface *s = ctx->driver_data->surfaces[i];
        if (!vav4l2_surface_capture_matches(ctx, s)) {
            if (s && s->capture_ctx == ctx)
                s->capture_ctx = NULL;
            continue;
        }
        if (s->has_pic && s->cap_view && !s->prime && !s->gbm_imported && !s->gbm_writable && s->last_writer != VAV4L2_WRITER_CPU) {
            uint32_t size = vav4l2_capture_plane_size(s->cap_fourcc, s->stride, s->aligned_h);
            void *snapshot = size && size <= ctx->capture_buf_size && vav4l2_surface_grow_memfd(s, size) == 0
                ? vav4l2_surface_map_memfd(s, size) : NULL;

            if (snapshot) {
                memcpy(snapshot, s->cap_view, size);
                s->last_writer = VAV4L2_WRITER_MEMFD;
            } else {
                fprintf(stderr, "v4l2: cannot preserve capture surface %u on release\n", (unsigned)i);
                s->has_pic = 0;
                s->status = VASurfaceSkipped;
            }
        }
        /* The surface-owned snapshot survives context teardown/recapture. */
        s->buf_index = -1;
        s->cap_view = NULL;
        s->capture_ctx = NULL;
    }
}

/* CAPTURE-only reconfiguration keeps stateful OUTPUT and its DPB protocol alive. */
int vav4l2_release_decoder_capture(struct vav4l2_context *ctx)
{
    vav4l2_release_capture_surfaces(ctx);
    vav4l2_unmap_capture_buffers(ctx);
    if (ctx->capture_bufs_allocd && vav4l2_release_queue(ctx->v4l2_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, ctx->capture_memory) < 0) return -1;
    ctx->capture_bufs_allocd = ctx->n_free_cap = 0;
    ctx->cap_width = ctx->cap_height = ctx->cap_stride = ctx->cap_sizeimage = ctx->cap_pixelformat = 0;
    return 0;
}

/*
 * Read back the driver-chosen capture geometry.
 * The stride can be larger than the width (alignment) — image export must
 * use these values, not the display dimensions.
 */
int vav4l2_get_capture_geometry(int fd, uint32_t *w, uint32_t *h,
                                uint32_t *stride, uint32_t *sizeimage)
{
    struct v4l2_format fmt = { 0 };
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

    if (xioctl(fd, VIDIOC_G_FMT, &fmt) < 0) {
        fprintf(stderr, "v4l2: G_FMT capture failed: %s\n", strerror(errno));
        return -1;
    }

    *w = fmt.fmt.pix_mp.width;
    *h = fmt.fmt.pix_mp.height;
    *stride = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
    *sizeimage = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;
    return 0;
}



void vav4l2_unmap_capture_buffers(struct vav4l2_context *ctx)
{
    int i;

    if (!ctx)
        return;
    vav4l2_capture_exports_release(ctx);
    vav4l2_capture_dma_release(ctx);
    vav4l2_capture_layout_release(ctx);
    for (i = 0; i < VAV4L2_MAX_CAPTURE_BUFS; i++) {
        if (!ctx->capture_buf_ptr[i] || ctx->capture_buf_ptr[i] == MAP_FAILED) {
            ctx->capture_buf_ptr[i] = NULL;
            continue;
        }
        if (ctx->capture_buf_anon)
            free(ctx->capture_buf_ptr[i]);
        else
            munmap(ctx->capture_buf_ptr[i], ctx->capture_buf_size);
        ctx->capture_buf_ptr[i] = NULL;
    }
    ctx->capture_buf_size = 0;
    ctx->capture_buf_anon = 0;
}

static int vav4l2_mmap_one_capture(struct vav4l2_context *ctx, int index)
{
    struct v4l2_buffer buf = { 0 };
    struct v4l2_plane planes[1] = { 0 };
    void *ptr;
    uint32_t length;
    int anon;

    if (!ctx || ctx->v4l2_fd < 0 || index < 0 ||
        index >= ctx->capture_bufs_allocd || index >= VAV4L2_MAX_CAPTURE_BUFS)
        return -1;
    if (ctx->capture_buf_ptr[index] && ctx->capture_buf_ptr[index] != MAP_FAILED)
        return 0;

    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = index;
    buf.length = 1;
    buf.m.planes = planes;
    if (xioctl(ctx->v4l2_fd, VIDIOC_QUERYBUF, &buf) < 0) {
        fprintf(stderr, "v4l2: QUERYBUF capture[%d] failed: %s\n",
                index, strerror(errno));
        return -1;
    }
    length = planes[0].length;
    if (!length)
        return -1;
    anon = g_ioctl_hook ? 1 : 0;
    if (anon) {
        ptr = calloc(1, length);
        if (!ptr)
            return -1;
    } else {
        ptr = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED,
                   ctx->v4l2_fd, planes[0].m.mem_offset);
        if (ptr == MAP_FAILED) {
            fprintf(stderr, "v4l2: mmap capture[%d] failed: %s\n",
                    index, strerror(errno));
            return -1;
        }
        if (vav4l2_debug)
            fprintf(stderr, "v4l2: capture mmap idx=%d size=%u\n",
                    index, length);
    }
    ctx->capture_buf_ptr[index] = ptr;
    ctx->capture_buf_size = length;
    ctx->capture_buf_anon = anon;
    return 0;
}

/*
 * Mmap all output buffers after REQBUFS
 * ptrs[] must be at least VAV4L2_NUM_OUTPUT_BUFS
 * Returns number of mmap'd buffers or -1 on error
 */
int vav4l2_mmap_output_buffers(int fd, int count, void **ptrs, uint32_t *size_out)
{
    if (count > VAV4L2_NUM_OUTPUT_BUFS)
        count = VAV4L2_NUM_OUTPUT_BUFS;

    for (int i = 0; i < count; i++) {
        struct v4l2_buffer buf = { 0 };
        struct v4l2_plane planes[1] = { 0 };

        buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        buf.length = 1;
        buf.m.planes = planes;

        if (xioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
            fprintf(stderr, "v4l2: QUERYBUF output[%d] failed: %s\n", i, strerror(errno));
            return -1;
        }

        void *ptr = mmap(NULL, planes[0].length, PROT_READ | PROT_WRITE,
                         MAP_SHARED, fd, planes[0].m.mem_offset);
        if (ptr == MAP_FAILED) {
            fprintf(stderr, "v4l2: mmap output[%d] failed: %s\n", i, strerror(errno));
            return -1;
        }

        ptrs[i] = ptr;
        if (i == 0 && size_out)
            *size_out = planes[0].length;
    }

    return count;
}

/*
 * Queue a capture buffer (decoded frame output)
 */
int vav4l2_queue_capture(int fd, int buf_index, int request_fd)
{
    struct v4l2_buffer buf = { 0 };
    struct v4l2_plane planes[1] = { 0 };

    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = buf_index;
    buf.length = 1;
    buf.m.planes = planes;
    /* request_fd intentionally NOT set: only OUTPUT (bitstream) buffers are
     * request objects. The CAPTURE queue does not support requests — binding
     * one makes vb2 fail QBUF with EPERM. Capture buffers are queued bare;
     * the decoder fills whichever capture buffer is available when the
     * request on the OUTPUT side completes. */
    (void)request_fd;

    if (xioctl(fd, VIDIOC_QBUF, &buf) < 0) {
        fprintf(stderr, "v4l2: QBUF capture[%d] failed: %s\n", buf_index, strerror(errno));
        return -1;
    }

    return 0;
}

int vav4l2_surface_alloc_export_fd(struct vav4l2_surface *s)
{
    uint32_t stride, h, sz;
    int fd;

    if (!s || s->width == 0 || s->height == 0)
        return -1;
    if (s->memfd_fd >= 0)
        return 0;

    stride = s->cpu_stride ? s->cpu_stride : s->width;
    h = s->height;
    sz = vav4l2_va_image_size(s->format ? s->format : VA_FOURCC_NV12, stride, h);
    if (!sz)
        sz = stride * h * 3 / 2;

    fd = memfd_create("vav4l2-surf", MFD_CLOEXEC);
    if (fd < 0)
        return -1;
    if (ftruncate(fd, (off_t)sz) < 0) {
        close(fd);
        return -1;
    }
    s->memfd_fd = fd;
    s->memfd_size = sz;
    if (!s->stride)
        s->stride = stride;
    if (!s->aligned_h)
        s->aligned_h = h;
    if (!s->cap_fourcc)
        s->cap_fourcc = V4L2_PIX_FMT_NV12;
    return 0;
}

int vav4l2_surface_ensure_cpu(struct vav4l2_surface *s)
{
    if (!s || !s->cpu_size)
        return -1;
    if (!s->cpu_ptr) {
        s->cpu_ptr = calloc(1, s->cpu_size);
        if (!s->cpu_ptr)
            return -1;
    }
    return 0;
}

/* Grow-only: a smaller `size` (resolution step-down) must never ftruncate —
 * clients may still hold mappings or dup'd fds of the larger memfd, and
 * shrinking drops pages under them (SIGBUS). */
int vav4l2_surface_grow_memfd(struct vav4l2_surface *s, uint32_t size)
{
    if (!s)
        return -1;
    if (s->memfd_fd < 0 && vav4l2_surface_alloc_export_fd(s) < 0)
        return -1;
    if (size == 0 || s->memfd_fd < 0 || size <= s->memfd_size)
        return 0;
    if (ftruncate(s->memfd_fd, (off_t)size) < 0)
        return -1;
    s->memfd_size = size;
    return 0;
}

/* Persistent per-surface mapping of memfd_fd (RW shared). Grow-only:
 * when `need` exceeds the cached length the mapping is grown — via
 * mremap, or when derived images still borrow the current mapping (a
 * moving remap would pull the pages from under them) by parking the old
 * one as "retired" until the last borrower releases it. Callers must
 * NOT munmap the result — the surface owns it. NULL on failure. */
void *vav4l2_surface_map_memfd(struct vav4l2_surface *s, uint32_t need)
{
    void *nm;

    if (!s || s->memfd_fd < 0 || need == 0)
        return NULL;
    if (s->memfd_map && s->memfd_map_size >= need)
        return s->memfd_map;
    if (need > s->memfd_size && vav4l2_surface_grow_memfd(s, need) < 0)
        return NULL;
    if (s->memfd_map) {
        if (s->memfd_borrows) {
            if (s->memfd_retired)
                return NULL;
            nm = mmap(NULL, need, PROT_READ | PROT_WRITE, MAP_SHARED,
                      s->memfd_fd, 0);
            if (nm == MAP_FAILED)
                return NULL;
            s->memfd_retired = s->memfd_map;
            s->memfd_retired_size = s->memfd_map_size;
        } else {
            nm = mremap(s->memfd_map, s->memfd_map_size, need,
                        MREMAP_MAYMOVE);
        }
        if (nm == MAP_FAILED)
            return NULL;
    } else {
        nm = mmap(NULL, need, PROT_READ | PROT_WRITE, MAP_SHARED,
                  s->memfd_fd, 0);
        if (nm == MAP_FAILED)
            return NULL;
    }
    s->memfd_map = nm;
    s->memfd_map_size = need;
    return s->memfd_map;
}

int vav4l2_surface_pull_capture(struct vav4l2_context *ctx,
                                struct vav4l2_surface *surf, int buf_index)
{
    if (ctx && surf && vav4l2_request_pending(ctx, surf->surface_id)) {
        surf->buf_index = buf_index; surf->capture_ctx = ctx; surf->has_pic = 0;
        surf->stride = ctx->cap_stride; surf->aligned_h = ctx->cap_height; surf->cap_fourcc = ctx->cap_pixelformat;
        surf->cap_view = NULL; surf->status = VASurfaceRendering; return 0;
    }

    if (!ctx || !surf || ctx->v4l2_fd < 0 || buf_index < 0 || buf_index >= ctx->capture_bufs_allocd) return -1;
    vav4l2_decode_trace(ctx, "pull-begin", 0, buf_index);
    surf->h264_frame_num_valid = 0;
    if (ctx->capture_memory == V4L2_MEMORY_DMABUF) return vav4l2_capture_dma_complete(ctx, surf, buf_index);
    int copied = vav4l2_gpu_copy_capture(ctx, surf, buf_index);
    if (copied) return copied > 0 ? 0 : -1;
    if (ctx->capture_layout) return vav4l2_capture_layout_pull(ctx, surf, buf_index);
    if (vav4l2_mmap_one_capture(ctx, buf_index) < 0) { vav4l2_decode_trace(ctx, "pull-map", -1, buf_index); return -1; }
    void *src = ctx->capture_buf_ptr[buf_index];
    if (!src || src == MAP_FAILED) return -1;
    return vav4l2_surface_store_capture(ctx, surf, buf_index, src, ctx->capture_buf_size, 1);
}

int vav4l2_surface_store_capture(struct vav4l2_context *ctx, struct vav4l2_surface *surf,
                                  int buf_index, void *src, uint32_t sz, int live_view)
{
    uint32_t stride, alh, fcc; void *dst;
    stride = ctx->cap_stride ? ctx->cap_stride : (uint32_t)ctx->width;
    alh = ctx->cap_height ? ctx->cap_height : (uint32_t)ctx->height;
    fcc = ctx->cap_pixelformat ? ctx->cap_pixelformat : V4L2_PIX_FMT_NV12;
    if (ctx->cap_sizeimage && ctx->cap_sizeimage < sz)
        sz = ctx->cap_sizeimage;
    uint32_t need = vav4l2_capture_plane_size(fcc, stride, alh);
    uint32_t row = (fcc == V4L2_PIX_FMT_NV15 || fcc == V4L2_PIX_FMT_NV20)
        ? ((surf->width + 3) / 4) * 5 : ((surf->width + 1) & ~1u) * (vav4l2_va_fourcc_for_capture(fcc) == VA_FOURCC_P010 ? 2u : 1u);
    if (!need || sz < need || stride < row || alh < surf->height) { vav4l2_decode_trace(ctx, "pull-geometry", -1, sz); return -1; }

    if (vav4l2_capture_tile(fcc)) {
        if (surf->format != vav4l2_va_fourcc_for_capture(fcc) || vav4l2_surface_ensure_cpu(surf) < 0 ||
            vav4l2_detile_capture(surf->cpu_ptr, surf->cpu_stride, surf->cpu_size, src, sz, stride, alh, surf->width, surf->height, fcc)) return -1;
        if (surf->prime && vav4l2_prime_write(surf) < 0) return -1;
        if (surf->gbm_bo && vav4l2_gbm_surface_upload(surf, surf->cpu_ptr, surf->cpu_stride, surf->height) < 0) return -1;
        surf->has_pic = surf->prime_capture_valid = 1; surf->buf_index = buf_index; surf->capture_ctx = ctx;
        surf->stride = stride; surf->aligned_h = alh; surf->cap_fourcc = fcc;
        surf->cap_view = NULL; surf->last_writer = VAV4L2_WRITER_CPU; return 0;
    }

    /* A client may still hold the GBM fd exported before EXPBUF became
     * available. That object must receive every new frame in either mode. */
    if ((surf->prime || (surf->gbm_bo && surf->format != VA_FOURCC_NV12)) &&
        vav4l2_prime_capture(surf, src, stride, alh, fcc, sz) < 0) return -1;
    if (surf->gbm_bo && surf->format == VA_FOURCC_NV12 && (fcc != V4L2_PIX_FMT_NV12 ||
        vav4l2_gbm_surface_upload(surf, src, stride, alh) < 0)) {
        surf->gbm_dirty = 1;
        fprintf(stderr, "v4l2: gbm display upload failed\n");
        return -1;
    }

    if (live_view && vav4l2_expbuf_export_wanted()) {
        /* Skip capture→GBM/memfd memcpy. Chrome EXPBUFs this index.
         * GetImage/Derive read capture_buf_ptr[buf_index] while it is live. */
        surf->has_pic = 1;
        surf->buf_index = buf_index;
        surf->stride = stride;
        surf->aligned_h = alh;
        surf->cap_fourcc = fcc;
        surf->cap_view = live_view ? src : NULL;
        surf->capture_ctx = ctx;
        surf->last_writer = VAV4L2_WRITER_MEMFD;
        return 0;
    }

    /*
     * Lazy memfd: when this surface has a GBM display bo, the bo is the
     * per-frame snapshot and the memfd copy is skipped — CPU-readback
     * callers (vaGetImage / vaDeriveImage / VPP source) refill it on
     * demand via vav4l2_surface_ensure_memfd(). Halves the per-frame
     * memcpy traffic for zero-copy clients (Chrome never reads back).
     * An existing exported bo must receive the current pixels. A memfd
     * fallback cannot update handles already held by display clients.
     */
    if (surf->gbm_bo) {
        surf->has_pic = 1;
        surf->buf_index = buf_index;
        surf->stride = stride;
        surf->aligned_h = alh;
        surf->cap_fourcc = fcc;
        surf->cap_view = live_view ? src : NULL;
        surf->capture_ctx = ctx;
        surf->last_writer = VAV4L2_WRITER_BO;
        return 0;
    }

    if (vav4l2_surface_grow_memfd(surf, sz) < 0) { vav4l2_decode_trace(ctx, "pull-memfd-grow", -1, sz); return -1; }
    dst = vav4l2_surface_map_memfd(surf, sz);
    if (!dst) { vav4l2_decode_trace(ctx, "pull-memfd-map", -1, sz); return -1; }
    memcpy(dst, src, sz);

    surf->has_pic = 1;
    surf->buf_index = buf_index;
    surf->stride = stride;
    surf->aligned_h = alh;
    surf->cap_fourcc = fcc;
    surf->cap_view = live_view ? src : NULL;
    surf->capture_ctx = ctx;
    surf->last_writer = VAV4L2_WRITER_MEMFD;
    return 0;
}

int vav4l2_bind_capture_export(struct vav4l2_context *ctx)
{
    int i, n;
    uint32_t sz;

    if (!ctx || !ctx->driver_data || ctx->capture_bufs_allocd <= 0)
        return -1;

    sz = ctx->cap_sizeimage;
    if (!sz)
        sz = vav4l2_capture_plane_size(ctx->cap_pixelformat ? ctx->cap_pixelformat
                                                            : V4L2_PIX_FMT_NV12,
                                       ctx->cap_stride, ctx->cap_height);

    n = ctx->num_render_targets;
    for (i = 0; i < n; i++) {
        VASurfaceID id;
        struct vav4l2_surface *s;

        if (!ctx->render_targets)
            break;
        id = ctx->render_targets[i];
        s = vav4l2_surface_by_id(ctx->driver_data, id);
        if (!s)
            continue;
        if (vav4l2_surface_grow_memfd(s, sz) < 0)
            continue;
        s->stride = ctx->cap_stride;
        s->aligned_h = ctx->cap_height;
        s->cap_fourcc = ctx->cap_pixelformat;
    }
    return 0;
}

void vav4l2_fill_prime_layers(VADRMPRIMESurfaceDescriptor *desc,
                              int fd, uint32_t object_size,
                              uint64_t modifier, uint32_t va_fourcc,
                              uint32_t width, uint32_t height,
                              uint32_t pitch, uint32_t chroma_row,
                              uint32_t combined_fmt, uint32_t luma_fmt,
                              uint32_t chroma_fmt, uint32_t flags)
{
    memset(desc, 0, sizeof(*desc));
    desc->fourcc = va_fourcc;
    desc->width = width;
    desc->height = height;
    desc->num_objects = 1;
    desc->objects[0].fd = fd;
    desc->objects[0].size = object_size;
    desc->objects[0].drm_format_modifier = modifier;

    if (flags & VA_EXPORT_SURFACE_SEPARATE_LAYERS) {
        desc->num_layers = 2;
        desc->layers[0].drm_format = luma_fmt;
        desc->layers[0].num_planes = 1;
        desc->layers[0].object_index[0] = 0;
        desc->layers[0].offset[0] = 0;
        desc->layers[0].pitch[0] = pitch;
        desc->layers[1].drm_format = chroma_fmt;
        desc->layers[1].num_planes = 1;
        desc->layers[1].object_index[0] = 0;
        desc->layers[1].offset[0] = pitch * chroma_row;
        desc->layers[1].pitch[0] = pitch;
    } else {
        desc->num_layers = 1;
        desc->layers[0].drm_format = combined_fmt;
        desc->layers[0].num_planes = 2;
        desc->layers[0].object_index[0] = 0;
        desc->layers[0].object_index[1] = 0;
        desc->layers[0].offset[0] = 0;
        desc->layers[0].offset[1] = pitch * chroma_row;
        desc->layers[0].pitch[0] = pitch;
        desc->layers[0].pitch[1] = pitch;
    }
}

/* DMA-BUF supports SEEK_END/SEEK_SET for its complete object size. Never
 * return an fd with a descriptor that understates its required plane span. */
int vav4l2_export_fd_validate(int fd, uint64_t required, uint32_t *size)
{
    off_t end = lseek(fd, 0, SEEK_END);
    if (!required || end < 0 || (uint64_t)end < required || (uint64_t)end > UINT32_MAX ||
        lseek(fd, 0, SEEK_SET) < 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) return -1;
    *size = (uint32_t)end;
    return 0;
}

/*
 * Dequeue a buffer from the given queue type
 * Returns buffer index or -1 on error
 */
/* flag_error (optional) is set when the dequeued buffer carried
 * V4L2_BUF_FLAG_ERROR — the index is still returned so the caller can
 * recycle the slot; the data itself must not be used. */
int vav4l2_dequeue_buffer(int fd, enum v4l2_buf_type type, int *flag_error)
{
    struct v4l2_buffer buf = { 0 };
    struct v4l2_plane planes[1] = { 0 };

    buf.type = type;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.length = 1;
    buf.m.planes = planes;

    if (flag_error)
        *flag_error = 0;
    if (xioctl(fd, VIDIOC_DQBUF, &buf) < 0) {
        if (errno != EAGAIN)
            fprintf(stderr, "v4l2: DQBUF failed: %s\n", strerror(errno));
        return -1;
    }
    if (buf.flags & V4L2_BUF_FLAG_ERROR) {
        fprintf(stderr, "v4l2: DQBUF type=%u idx=%u ERROR flags=0x%x bytesused=%u\n",
                (unsigned)type, buf.index, buf.flags, planes[0].bytesused);
        if (flag_error)
            *flag_error = 1;
    }

    return buf.index;
}

/*
 * Set extended controls as plain (non-request) values. Some codecs bind
 * stream-level controls globally — e.g. the rkvdec HEVC SPS rejects the
 * request-scoped variant with EINVAL.
 */
int vav4l2_set_global_controls(int v4l2_fd, struct v4l2_ext_controls *ctrls)
{
    ctrls->which = V4L2_CTRL_WHICH_CUR_VAL;
    ctrls->request_fd = 0;

    if (xioctl(v4l2_fd, VIDIOC_S_EXT_CTRLS, ctrls) < 0) {
        fprintf(stderr, "v4l2: S_EXT_CTRLS (global) failed: %s\n",
                strerror(errno));
        return -1;
    }
    return 0;
}

void vav4l2_explog(const char *fmt, ...)
{
    va_list ap;

    if (!vav4l2_debug)
        return;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fflush(stderr);
}

int vav4l2_expbuf_export_wanted(void)
{
    static int once, wanted;
    if (!once) {
        const char *e = getenv("VAV4L2_EXPBUF_EXPORT");

        /* Native export is enabled unless explicitly disabled. */
        wanted = 1;
        if (e && e[0] == '0')
            wanted = 0;
        once = 1;
        if (vav4l2_debug)
            fprintf(stderr, "v4l2: EXPBUF wanted=%d pid=%d\n",
                    wanted, getpid());
    }
    return wanted;
}

int vav4l2_capture_expbuf(struct vav4l2_context *ctx, int buf_index)
{
    if (!ctx || ctx->v4l2_fd < 0 || buf_index < 0)
        return -1;
    if (ctx->capture_internal) {
        if (buf_index >= ctx->capture_bufs_allocd || !ctx->capture_dma[buf_index].active) return -1;
        return fcntl(ctx->capture_dma[buf_index].fd, F_DUPFD_CLOEXEC, 0);
    }
    return vav4l2_capture_export_plane(ctx, buf_index, 0);
}

int vav4l2_claim_capture_for_export(struct vav4l2_context *ctx,
                                    struct vav4l2_surface *surf)
{
    int idx, w, h;
    uint32_t fourcc;

    if (!ctx || !surf || ctx->v4l2_fd < 0 || ctx->stateful)
        return -1;
    w = ctx->width > 0 ? ctx->width : (int)surf->width;
    h = ctx->height > 0 ? ctx->height : (int)surf->height;
    fourcc = ctx->cap_pixelformat ? ctx->cap_pixelformat : V4L2_PIX_FMT_NV12;
    if (vav4l2_ensure_capture(ctx, w, h, fourcc) < 0)
        return -1;
    if (surf->buf_index >= 0)
        return 0;
    if (ctx->n_free_cap == 0) {
        vav4l2_explog("v4l2: claim capture: no free slot surf=%u\n",
                      (unsigned)surf->surface_id);
        return -1;
    }
    idx = ctx->free_cap_bufs[--ctx->n_free_cap];
    surf->buf_index = idx;
    surf->capture_ctx = ctx;
    surf->stride = ctx->cap_stride;
    surf->aligned_h = ctx->cap_height;
    surf->cap_fourcc = ctx->cap_pixelformat;
    /* Do not mmap here: mapping the whole Chrome pool pins ~75MB CMA and
     * has hung this SoC. EXPBUF does not need a userspace mapping. */
    vav4l2_explog("v4l2: claim capture idx=%d surf=%u\n",
                  idx, (unsigned)surf->surface_id);
    return 0;
}

int vav4l2_decode_pending(const struct vav4l2_context *ctx, VASurfaceID id)
{
    return ctx->stateful ? vav4l2_stateful_pending(ctx, id) : vav4l2_request_busy(ctx, id);
}
int vav4l2_decode_pump(struct vav4l2_context *ctx)
{
    return ctx->stateful ? vav4l2_stateful_pump(ctx) : vav4l2_request_pump(ctx);
}
VAStatus vav4l2_decode_sync(struct vav4l2_context *ctx, struct vav4l2_surface *s, uint64_t timeout)
{
    return ctx->stateful ? vav4l2_stateful_sync(ctx, s, timeout) : vav4l2_request_sync(ctx, s, timeout);
}
