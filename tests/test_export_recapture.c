/*
 * Drive shipped surface-export and capture-reconfigure paths.
 * Export must succeed without DQBUF; small→large recapture must STREAMOFF,
 * drop exported fds, REQBUFS(0), then allocate a larger capture set.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <linux/media.h>
#include <va/va.h>
#include <va/va_dec_av1.h>
#include <va/va_dec_hevc.h>
#include <va/va_dec_vp8.h>
#include <va/va_drmcommon.h>

#include "va_v4l2.h"

extern VAStatus __vaDriverInit_1_20(VADriverContextP ctx);
extern int __real_vav4l2_open_device(const char *path);
extern int __real_vav4l2_open_media_for_device(const char *path);
static int g_context_fd = -1;

int __wrap_vav4l2_open_device(const char *path)
{
    return g_context_fd >= 0 ? dup(g_context_fd) : __real_vav4l2_open_device(path);
}

int __wrap_vav4l2_open_media_for_device(const char *path)
{
    return g_context_fd >= 0 ? dup(g_context_fd) : __real_vav4l2_open_media_for_device(path);
}

static int g_fail;

static void expect_true(int cond, const char *tag)
{
    if (!cond) {
        fprintf(stderr, "FAIL %s\n", tag);
        g_fail++;
    } else {
        printf("OK %s\n", tag);
    }
}

static int g_control_request_fd, g_control_calls;

static int mock_request_controls(int fd, unsigned long request, void *arg)
{
    struct v4l2_ext_controls *ctrls = arg;

    if (fd != 17 || request != VIDIOC_S_EXT_CTRLS)
        return -1;
    g_control_calls++;
    if (ctrls->which != (g_control_request_fd < 0 ? V4L2_CTRL_WHICH_CUR_VAL : V4L2_CTRL_WHICH_REQUEST_VAL) ||
        ctrls->request_fd != (g_control_request_fd < 0 ? 0 : g_control_request_fd)) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static void test_request_control_binding(void)
{
    struct v4l2_ctrl_h264_sps sps = {0};
    struct v4l2_ext_control control = {.id = V4L2_CID_STATELESS_H264_SPS, .size = sizeof(sps), .ptr = &sps};
    struct v4l2_ext_controls ctrls = {.count = 1, .controls = &control};
    const int requests[] = {13, 65537};

    g_control_calls = 0;
    vav4l2_set_ioctl_hook(mock_request_controls);
    for (unsigned i = 0; i < sizeof(requests) / sizeof(requests[0]); i++) {
        g_control_request_fd = requests[i];
        expect_true(vav4l2_set_request_controls(requests[i], 17, &ctrls) == 0, "controls-bound-to-request");
    }
    g_control_request_fd = -1;
    expect_true(vav4l2_set_global_controls(17, &ctrls) == 0, "global-controls-clear-request");
    expect_true(g_control_calls == 3 && ctrls.controls == &control && control.ptr == &sps,
                "controls-payload-preserved");
    vav4l2_set_ioctl_hook(NULL);
}

static char g_ops[32][24];
static int g_nops;
static uint32_t g_last_cap_w, g_last_cap_h;
static int g_streamoff, g_reqbufs0, g_reqbufs_n, g_sfmt_cap;
static int g_fds_live_at_reqbufs0;
static int g_small_output, g_capture_query_error, g_require_hevc_sps_first;
static int g_clamp_capture, g_bad_capture_format, g_clamp_get_format;
static unsigned g_queued_cap, g_queued_out;
static int g_export_short, g_export_fd = -1;
static struct vav4l2_context *g_ctx;

static void log_op(const char *op)
{
    if (g_nops < 32) {
        snprintf(g_ops[g_nops], sizeof(g_ops[0]), "%s", op);
        g_nops++;
    }
}

static int op_index(const char *op)
{
    int i;
    for (i = 0; i < g_nops; i++) {
        if (strcmp(g_ops[i], op) == 0)
            return i;
    }
    return -1;
}

static int mock_ioctl(int fd, unsigned long request, void *arg)
{
    (void)fd;

    if (request == VIDIOC_ENUM_FMT) {
        struct v4l2_fmtdesc *f = arg;
        if (!f->index) { f->pixelformat = V4L2_PIX_FMT_HEVC_SLICE; return 0; }
        errno = EINVAL; return -1;
    }
    if (request == VIDIOC_CREATE_BUFS) { ((struct v4l2_create_buffers *)arg)->capabilities = V4L2_BUF_CAP_SUPPORTS_REQUESTS; return 0; }
    if (request == VIDIOC_QUERY_EXT_CTRL) return 0;
    if (request == VIDIOC_QUERYCTRL) { ((struct v4l2_queryctrl *)arg)->maximum = 4; return 0; }
    if (request == VIDIOC_QUERYMENU) return 0;
    if (request == VIDIOC_STREAMOFF) {
        g_streamoff++;
        log_op("STREAMOFF");
        return 0;
    }
    if (request == VIDIOC_REQBUFS) {
        struct v4l2_requestbuffers *req = arg;

        if (req->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE && req->count == 0) {
            int i;

            g_reqbufs0++;
            g_fds_live_at_reqbufs0 = 0;
            if (g_ctx && g_ctx->driver_data) {
                for (i = 0; i < g_ctx->num_render_targets; i++) {
                    VASurfaceID id = g_ctx->render_targets[i];
                    struct vav4l2_surface *s;

                    if (id == VA_INVALID_ID || (unsigned)id >= 4096)
                        continue;
                    s = g_ctx->driver_data->surfaces[id];
                    if (s && s->memfd_fd >= 0)
                        g_fds_live_at_reqbufs0++;
                }
            }
            log_op("REQBUFS0");
            return 0;
        }
        if (req->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            g_reqbufs_n++;
            if (req->count > 4)
                req->count = 4;
            log_op("REQBUFSN");
            return 0;
        }
        return 0;
    }
    if (request == VIDIOC_S_FMT) {
        struct v4l2_format *fmt = arg;

        if (fmt->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            g_sfmt_cap++;
            if (g_clamp_capture) {
                fmt->fmt.pix_mp.width = 1920;
                fmt->fmt.pix_mp.height = 1088;
            }
            if (g_bad_capture_format)
                fmt->fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
            g_last_cap_w = fmt->fmt.pix_mp.width;
            g_last_cap_h = fmt->fmt.pix_mp.height;
            log_op("S_FMT_CAP");
        }
        return 0;
    }
    if (request == VIDIOC_G_FMT) {
        struct v4l2_format *fmt = arg;

        if (fmt->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            fmt->fmt.pix_mp.width = g_last_cap_w ? g_last_cap_w : 320;
            fmt->fmt.pix_mp.height = g_last_cap_h ? g_last_cap_h : 240;
            if (g_clamp_get_format) {
                fmt->fmt.pix_mp.width = 1920;
                fmt->fmt.pix_mp.height = 1088;
            }
            fmt->fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
            fmt->fmt.pix_mp.num_planes = 1;
            fmt->fmt.pix_mp.plane_fmt[0].bytesperline = fmt->fmt.pix_mp.width;
            fmt->fmt.pix_mp.plane_fmt[0].sizeimage =
                fmt->fmt.pix_mp.width * fmt->fmt.pix_mp.height * 3 / 2;
        } else {
            /* Capture-only tests keep the coded queue large enough. */
            fmt->fmt.pix_mp.width = g_small_output ? 320 : 4096;
            fmt->fmt.pix_mp.height = g_small_output ? 240 : 4096;
            fmt->fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264_SLICE;
            fmt->fmt.pix_mp.num_planes = 1;
        }
        return 0;
    }
    if (request == VIDIOC_EXPBUF) {
        struct v4l2_exportbuffer *exp = arg;
        int mfd = memfd_create("vav4l2-exp", MFD_CLOEXEC);

        if (mfd < 0)
            return -1;
        {
            uint32_t sz = g_export_short ? 1 : g_last_cap_w * g_last_cap_h * 3 / 2 + 64;
            if (sz && ftruncate(mfd, (off_t)sz) < 0) {
                close(mfd);
                return -1;
            }
        }
        exp->fd = mfd;
        g_export_fd = mfd;
        log_op("EXPBUF");
        return 0;
    }
    if (request == VIDIOC_STREAMON) {
        log_op("STREAMON");
        return 0;
    }
    if (request == VIDIOC_S_EXT_CTRLS) {
        struct v4l2_ext_controls *ctrls = arg;

        if (g_require_hevc_sps_first && ctrls->which == V4L2_CTRL_WHICH_CUR_VAL && ctrls->count &&
            ctrls->controls[0].id == V4L2_CID_STATELESS_HEVC_SPS) {
            if (g_ctx && g_ctx->capture_bufs_allocd > 0) {
                errno = EBUSY;
                return -1;
            }
            log_op("HEVC_SPS");
        }
        log_op("S_EXT_CTRLS");
        return 0;
    }
    if (request == VIDIOC_QUERYBUF) {
        struct v4l2_buffer *buf = arg;

        if (buf->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE && g_capture_query_error) {
            errno = EIO;
            return -1;
        }
        if (buf->m.planes)
            buf->m.planes[0].length = buf->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
                ? g_last_cap_w * g_last_cap_h * 2 : 65536;
        return 0;
    }
    if (request == VIDIOC_QBUF) {
        struct v4l2_buffer *buf = arg;
        if (buf->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) g_queued_cap = buf->index;
        else g_queued_out = buf->index;
        return 0;
    }
    if (request == VIDIOC_DQBUF) {
        struct v4l2_buffer *buf = arg;

        buf->index = buf->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE ? g_queued_cap : g_queued_out;
        return 0;
    }
#ifdef MEDIA_REQUEST_IOC_QUEUE
    if (request == MEDIA_REQUEST_IOC_QUEUE)
        return 0;
#endif
    return 0;
}

static void test_recapture_small_to_large(void)
{
    struct vav4l2_driver_data dd;
    struct vav4l2_context ctx;
    struct vav4l2_surface *s1, *s2;
    VASurfaceID targets[2] = { 1, 2 };
    int i0, i1, i2, i3, nexp;

    memset(&dd, 0, sizeof(dd));
    memset(&ctx, 0, sizeof(ctx));
    s1 = calloc(1, sizeof(*s1));
    s2 = calloc(1, sizeof(*s2));
    if (!s1 || !s2) {
        fprintf(stderr, "FAIL calloc surfaces\n");
        g_fail++;
        free(s1);
        free(s2);
        return;
    }

    s1->width = 320;
    s1->height = 240;
    s1->format = VA_FOURCC_NV12;
    s1->buf_index = -1;
    s1->memfd_fd = -1;
    s1->cpu_stride = 320;
    s2->width = 320;
    s2->height = 240;
    s2->format = VA_FOURCC_NV12;
    s2->buf_index = -1;
    s2->memfd_fd = -1;
    s2->cpu_stride = 320;
    expect_true(vav4l2_surface_alloc_export_fd(s1) == 0, "recap-s1-memfd");
    expect_true(vav4l2_surface_alloc_export_fd(s2) == 0, "recap-s2-memfd");

    dd.surfaces[1] = s1;
    dd.surfaces[2] = s2;

    ctx.driver_data = &dd;
    ctx.v4l2_fd = 7;
    ctx.media_fd = -1;
    ctx.request_fd = -1;
    ctx.width = 320;
    ctx.height = 240;
    ctx.cap_width = 320;
    ctx.cap_height = 240;
    ctx.cap_stride = 320;
    ctx.cap_sizeimage = 320 * 240 * 3 / 2;
    ctx.cap_pixelformat = V4L2_PIX_FMT_NV12;
    ctx.capture_bufs_allocd = 24;
    ctx.output_bufs_allocd = 2;
    ctx.streamed = 1;
    ctx.n_free_cap = 24;
    ctx.render_targets = targets;
    ctx.num_render_targets = 2;

    g_ctx = &ctx;
    g_nops = 0;
    g_streamoff = g_reqbufs0 = g_reqbufs_n = g_sfmt_cap = 0;
    g_fds_live_at_reqbufs0 = -1;
    g_last_cap_w = 320;
    g_last_cap_h = 240;
    vav4l2_set_ioctl_hook(mock_ioctl);

    expect_true(vav4l2_ensure_capture(&ctx, 1920, 1080, V4L2_PIX_FMT_NV12) == 0,
                "ensure-capture-4k");
    expect_true(g_streamoff >= 2, "streamoff-both-queues");
    expect_true(g_reqbufs0 == 1, "reqbufs0-once");
    expect_true(g_fds_live_at_reqbufs0 == 2, "memfd-kept-at-reqbufs0");
    expect_true(g_sfmt_cap >= 1, "sfmt-capture");
    expect_true(g_reqbufs_n >= 1, "reqbufs-new");
    expect_true(ctx.cap_width >= 1920 && ctx.cap_height >= 1080,
                "capture-holds-larger");
    expect_true(ctx.streamed == 0, "streamed-cleared");

    i0 = op_index("STREAMOFF");
    i1 = op_index("REQBUFS0");
    i2 = op_index("S_FMT_CAP");
    i3 = op_index("REQBUFSN");
    expect_true(i0 >= 0 && i1 > i0, "streamoff-before-reqbufs0");
    expect_true(i1 >= 0 && i2 > i1, "reqbufs0-before-sfmt");
    expect_true(i2 >= 0 && i3 > i2, "sfmt-before-reqbufsn");

    nexp = 0;
    for (i0 = 0; i0 < g_nops; i0++) {
        if (strcmp(g_ops[i0], "EXPBUF") == 0)
            nexp++;
    }
    expect_true(s1->memfd_fd >= 0, "s1-memfd-kept");
    expect_true(s2->memfd_fd >= 0, "s2-memfd-kept");
    expect_true(nexp == 0, "ensure-capture-no-expbuf");

    if (s1->memfd_fd >= 0)
        close(s1->memfd_fd);
    if (s2->memfd_fd >= 0)
        close(s2->memfd_fd);
    free(s1);
    free(s2);
    vav4l2_set_ioctl_hook(NULL);
    g_ctx = NULL;
}

struct recap_env {
    struct vav4l2_driver_data dd;
    struct vav4l2_context ctx;
    struct vav4l2_surface *surf;
    VASurfaceID targets[1];
    uint8_t outbuf[8192];
    int sp[2];
};

static int recap_env_open(struct recap_env *e, int cap_w, int cap_h)
{
    memset(e, 0, sizeof(*e));
    e->sp[0] = e->sp[1] = -1;
    e->surf = calloc(1, sizeof(*e->surf));
    if (!e->surf)
        return -1;
    if (pipe(e->sp) < 0) {
        free(e->surf);
        e->surf = NULL;
        return -1;
    }
    if (write(e->sp[1], "x", 1) != 1) {
        close(e->sp[0]);
        close(e->sp[1]);
        free(e->surf);
        e->surf = NULL;
        return -1;
    }
    e->surf->width = cap_w;
    e->surf->height = cap_h;
    e->surf->format = VA_FOURCC_NV12;
    e->surf->buf_index = -1;
    e->surf->memfd_fd = -1;
    e->surf->cpu_stride = (uint32_t)cap_w;
    if (vav4l2_surface_alloc_export_fd(e->surf) < 0)
        return -1;
    e->targets[0] = 1;
    e->dd.surfaces[1] = e->surf;
    e->ctx.driver_data = &e->dd;
    e->ctx.v4l2_fd = e->sp[0];
    e->ctx.request_fd = e->sp[1];
    e->ctx.media_fd = -1;
    e->ctx.width = cap_w;
    e->ctx.height = cap_h;
    e->ctx.cap_width = (uint32_t)cap_w;
    e->ctx.cap_height = (uint32_t)cap_h;
    e->ctx.cap_stride = (uint32_t)cap_w;
    e->ctx.cap_sizeimage = (uint32_t)cap_w * (uint32_t)cap_h * 3 / 2;
    e->ctx.cap_pixelformat = V4L2_PIX_FMT_NV12;
    e->ctx.capture_bufs_allocd = 24;
    e->ctx.output_bufs_allocd = 2;
    e->ctx.streamed = 1;
    e->ctx.n_free_out = 1;
    e->ctx.free_out_bufs[0] = 0;
    e->ctx.output_buf_ptr[0] = e->outbuf;
    e->ctx.output_buf_size = sizeof(e->outbuf);
    e->ctx.render_targets = e->targets;
    e->ctx.num_render_targets = 1;
    e->ctx.current_surface = e->surf;
    g_ctx = &e->ctx;
    g_capture_query_error = 0;
    g_nops = 0;
    g_streamoff = g_reqbufs0 = g_reqbufs_n = g_sfmt_cap = 0;
    g_fds_live_at_reqbufs0 = -1;
    g_last_cap_w = (uint32_t)cap_w;
    g_last_cap_h = (uint32_t)cap_h;
    vav4l2_set_ioctl_hook(mock_ioctl);
    return 0;
}

static void recap_env_close(struct recap_env *e)
{
    for (int i = 0; i < VAV4L2_MAX_CAPTURE_BUFS; i++) {
        if (e->ctx.capture_buf_ptr[i]) {
            if (e->ctx.capture_buf_anon) free(e->ctx.capture_buf_ptr[i]);
            else munmap(e->ctx.capture_buf_ptr[i], e->ctx.capture_buf_size);
        }
    }
    if (e->surf) {
        if (e->surf->memfd_map) munmap(e->surf->memfd_map, e->surf->memfd_map_size);
        if (e->surf->memfd_retired) munmap(e->surf->memfd_retired, e->surf->memfd_retired_size);
        if (e->surf->memfd_fd >= 0)
            close(e->surf->memfd_fd);
        free(e->surf);
        e->surf = NULL;
    }
    if (e->ctx.request_fd >= 0)
        close(e->ctx.request_fd);
    if (e->sp[0] >= 0)
        close(e->sp[0]);
    e->ctx.request_fd = -1;
    e->sp[0] = e->sp[1] = -1;
    vav4l2_set_ioctl_hook(NULL);
    g_ctx = NULL;
}

static void expect_grew_to(struct recap_env *e, int w, int h, const char *tag)
{
    char t[80];
    int i0, i1, i2, i3, i4;

    snprintf(t, sizeof(t), "%s-status-geom", tag);
    expect_true(e->ctx.cap_width >= (uint32_t)w && e->ctx.cap_height >= (uint32_t)h, t);
    snprintf(t, sizeof(t), "%s-reqbufs0", tag);
    expect_true(g_reqbufs0 == 1, t);
    snprintf(t, sizeof(t), "%s-memfd-kept", tag);
    expect_true(g_fds_live_at_reqbufs0 >= 1, t);
    i0 = op_index("STREAMOFF");
    i1 = op_index("REQBUFS0");
    i2 = op_index("S_FMT_CAP");
    i3 = op_index("REQBUFSN");
    i4 = op_index("STREAMON");
    snprintf(t, sizeof(t), "%s-order", tag);
    expect_true(i0 >= 0 && i1 > i0 && i2 > i1 && i3 > i2 && i4 > i3, t);
}

static void test_av1_translate_recapture(void)
{
    struct recap_env e;
    VADecPictureParameterBufferAV1 pic;
    uint8_t tile[64];
    struct vav4l2_buffer pic_buf, data_buf;
    struct vav4l2_buffer *bufs[2];
    VAStatus st;

    if (recap_env_open(&e, 432, 240) < 0) {
        fprintf(stderr, "FAIL recap_env_open av1\n");
        g_fail++;
        return;
    }
    e.ctx.codec = VAV4L2_CODEC_AV1;

    memset(&pic, 0, sizeof(pic));
    memset(tile, 0xab, sizeof(tile));
    pic.frame_width_minus1 = 1919;
    pic.frame_height_minus1 = 1079;
    pic.bit_depth_idx = 0;
    pic.tile_cols = 1;
    pic.tile_rows = 1;
    pic.seq_info_fields.fields.subsampling_x = 1;
    pic.seq_info_fields.fields.subsampling_y = 1;
    pic.pic_info_fields.bits.frame_type = 0; /* key */
    pic.pic_info_fields.bits.show_frame = 1;
    pic.pic_info_fields.bits.uniform_tile_spacing_flag = 1;

    memset(&pic_buf, 0, sizeof(pic_buf));
    memset(&data_buf, 0, sizeof(data_buf));
    pic_buf.type = VAPictureParameterBufferType;
    pic_buf.data = &pic;
    pic_buf.size = sizeof(pic);
    data_buf.type = VASliceDataBufferType;
    data_buf.data = tile;
    data_buf.size = sizeof(tile);
    bufs[0] = &pic_buf;
    bufs[1] = &data_buf;

    st = vav4l2_av1_translate(&e.ctx, bufs, 2);
    expect_true(st == VA_STATUS_SUCCESS, "av1-translate-status");
    expect_grew_to(&e, 1920, 1080, "av1");
    expect_true(g_last_cap_w == 1920 && g_last_cap_h == 1080,
                "av1-sfm-coded-size");
    expect_true(g_streamoff >= 2, "av1-streamoff");

    recap_env_close(&e);
}

static void test_ensure_noop_shrink_fmt(void)
{
    struct vav4l2_context ctx;

    memset(&ctx, 0, sizeof(ctx));
    ctx.v4l2_fd = 7;
    ctx.cap_width = 1920;
    ctx.cap_height = 1080;
    ctx.cap_pixelformat = V4L2_PIX_FMT_NV12;
    ctx.capture_bufs_allocd = 4;
    ctx.streamed = 1;
    g_ctx = &ctx;
    g_nops = 0;
    g_streamoff = g_reqbufs0 = g_reqbufs_n = g_sfmt_cap = 0;
    g_last_cap_w = 1920;
    g_last_cap_h = 1080;
    vav4l2_set_ioctl_hook(mock_ioctl);

    expect_true(vav4l2_ensure_capture(&ctx, 1280, 720, V4L2_PIX_FMT_NV12) == 0,
                "shrink-ok");
    expect_true(g_reqbufs0 == 0 && g_streamoff == 0, "shrink-no-recapture");
    expect_true(ctx.cap_width == 1920 && ctx.cap_height == 1080, "shrink-keeps-large");
    expect_true(vav4l2_ensure_capture(&ctx, 1920, 1080, V4L2_PIX_FMT_NV12) == 0,
                "same-ok");
    expect_true(g_reqbufs0 == 0, "same-no-recapture");
    expect_true(vav4l2_ensure_capture(&ctx, 1920, 1080, V4L2_PIX_FMT_NV15) == 0,
                "fmt-change-ok");
    expect_true(g_reqbufs0 == 1, "fmt-reqbufs0");
    expect_true(ctx.cap_pixelformat == V4L2_PIX_FMT_NV15, "fmt-nv15");
    vav4l2_set_ioctl_hook(NULL);
    g_ctx = NULL;
}

static void test_capture_clamping(void)
{
    struct vav4l2_context ctx = {0};
    ctx.v4l2_fd = 7;
    vav4l2_set_ioctl_hook(mock_ioctl);
    g_clamp_capture = 1;
    g_reqbufs_n = 0;
    expect_true(vav4l2_setup_capture_queue_count(7, 3840, 2160, V4L2_PIX_FMT_NV12, 4) < 0 &&
                g_reqbufs_n == 0, "clamped-4k-rejected-before-allocation");
    g_clamp_capture = 0;
    g_bad_capture_format = 1;
    expect_true(vav4l2_setup_capture_queue_count(7, 1280, 720, V4L2_PIX_FMT_NV15, 4) < 0,
                "capture-format-substitution-rejected");
    g_bad_capture_format = 0;
    g_clamp_get_format = 1;
    expect_true(vav4l2_ensure_capture(&ctx, 3840, 2160, V4L2_PIX_FMT_NV12) < 0 &&
                ctx.n_free_cap == 0, "clamped-get-format-rejected-before-decode");
    g_clamp_get_format = 0;
    vav4l2_set_ioctl_hook(NULL);
}

static void test_output_remap_failure(void)
{
    struct vav4l2_context ctx = {0};
    ctx.v4l2_fd = -1;
    /* A pipe is a real fd that cannot be mmap'd as a coded buffer. */
    int fds[2];
    if (pipe(fds) < 0) { g_fail++; return; }
    ctx.v4l2_fd = fds[0];
    ctx.width = ctx.cap_width = 320;
    ctx.height = ctx.cap_height = 240;
    ctx.cap_pixelformat = V4L2_PIX_FMT_NV12;
    ctx.capture_bufs_allocd = 4;
    ctx.output_bufs_allocd = 2;
    ctx.n_free_out = 2;
    ctx.output_plane_len[0] = 8192;
    g_small_output = 1;
    vav4l2_set_ioctl_hook(mock_ioctl);
    expect_true(vav4l2_ensure_capture(&ctx, 1920, 1080, V4L2_PIX_FMT_NV12) < 0,
                "output-remap-failure-is-terminal");
    expect_true(ctx.n_free_out == 0 && ctx.output_plane_len[0] == 0,
                "failed-output-not-recycled-with-stale-length");
    vav4l2_set_ioctl_hook(NULL);
    g_small_output = 0;
    close(fds[0]); close(fds[1]);
}

static void test_h264_translate_recapture(int fail_pull)
{
    struct recap_env e;
    VAPictureParameterBufferH264 pic;
    uint8_t nal[] = { 0, 0, 0, 1, 0x65, 0xb8, 0x40, 0x80 };
    VASliceParameterBufferH264 slice = {.slice_data_size = sizeof(nal), .slice_type = 2};
    struct vav4l2_buffer slice_buf = {.type = VASliceParameterBufferType, .data = &slice, .size = sizeof(slice)};
    struct vav4l2_buffer pic_buf, data_buf;
    struct vav4l2_buffer *bufs[3];
    int i;

    if (recap_env_open(&e, 432, 240) < 0) {
        fprintf(stderr, "FAIL recap_env h264\n");
        g_fail++;
        return;
    }
    memset(&pic, 0, sizeof(pic));
    for (i = 0; i < 16; i++)
        pic.ReferenceFrames[i].flags = VA_PICTURE_H264_INVALID;
    pic.picture_width_in_mbs_minus1 = 119;
    pic.picture_height_in_mbs_minus1 = 67;
    pic.seq_fields.bits.chroma_format_idc = 1;
    pic.seq_fields.bits.frame_mbs_only_flag = 1;
    pic.pic_fields.bits.reference_pic_flag = 1;
    e.ctx.profile = VAProfileH264High;
    e.ctx.codec = VAV4L2_CODEC_H264;
    memset(&pic_buf, 0, sizeof(pic_buf));
    memset(&data_buf, 0, sizeof(data_buf));
    pic_buf.type = VAPictureParameterBufferType;
    pic_buf.data = &pic;
    pic_buf.size = sizeof(pic);
    data_buf.type = VASliceDataBufferType;
    data_buf.data = nal;
    data_buf.size = sizeof(nal);
    bufs[0] = &pic_buf;
    bufs[1] = &slice_buf;
    bufs[2] = &data_buf;
    g_capture_query_error = fail_pull;
    expect_true(vav4l2_h264_translate(&e.ctx, bufs, 3) ==
                (fail_pull ? VA_STATUS_ERROR_OPERATION_FAILED : VA_STATUS_SUCCESS),
                "h264-translate-status");
    expect_true(e.surf->has_pic == !fail_pull, "h264-capture-attached-only-on-success");
    expect_grew_to(&e, 1920, 1088, "h264");
    recap_env_close(&e);
}

static void test_hevc_translate_recapture(int fail_pull)
{
    struct recap_env e;
    VAPictureParameterBufferHEVC pic;
    uint8_t nal[] = { 0, 0, 0, 1, 0x26, 1, 0xa0, 0 };
    VASliceParameterBufferHEVC slice = {.slice_data_size = sizeof(nal)};
    struct vav4l2_buffer slice_buf = {.type = VASliceParameterBufferType, .data = &slice, .size = sizeof(slice)};
    struct vav4l2_buffer pic_buf, data_buf;
    struct vav4l2_buffer *bufs[3];
    int i;

    if (recap_env_open(&e, 432, 240) < 0) {
        fprintf(stderr, "FAIL recap_env hevc\n");
        g_fail++;
        return;
    }
    memset(&pic, 0, sizeof(pic));
    for (i = 0; i < 15; i++)
        pic.ReferenceFrames[i].flags = VA_PICTURE_HEVC_INVALID;
    pic.slice_parsing_fields.bits.IdrPicFlag = pic.slice_parsing_fields.bits.RapPicFlag = 1;
    pic.pic_width_in_luma_samples = 1920;
    pic.pic_height_in_luma_samples = 1080;
    pic.pic_fields.bits.chroma_format_idc = 1;
    e.ctx.codec = VAV4L2_CODEC_HEVC;
    memset(&pic_buf, 0, sizeof(pic_buf));
    memset(&data_buf, 0, sizeof(data_buf));
    pic_buf.type = VAPictureParameterBufferType;
    pic_buf.data = &pic;
    pic_buf.size = sizeof(pic);
    data_buf.type = VASliceDataBufferType;
    data_buf.data = nal;
    data_buf.size = sizeof(nal);
    bufs[0] = &pic_buf;
    bufs[1] = &slice_buf;
    bufs[2] = &data_buf;
    g_capture_query_error = fail_pull;
    expect_true(vav4l2_hevc_translate(&e.ctx, bufs, 3) ==
                (fail_pull ? VA_STATUS_ERROR_OPERATION_FAILED : VA_STATUS_SUCCESS),
                "hevc-translate-status");
    expect_true(e.surf->has_pic == !fail_pull, "hevc-capture-attached-only-on-success");
    expect_grew_to(&e, 1920, 1080, "hevc");
    recap_env_close(&e);
}

static void test_vp8_translate_recapture(void)
{
    struct recap_env e;
    VAPictureParameterBufferVP8 pic;
    VASliceParameterBufferVP8 slice;
    uint8_t data[64];
    struct vav4l2_buffer pic_buf, sl_buf, data_buf;
    struct vav4l2_buffer *bufs[3];

    if (recap_env_open(&e, 320, 240) < 0) {
        fprintf(stderr, "FAIL recap_env vp8\n");
        g_fail++;
        return;
    }
    memset(&pic, 0, sizeof(pic));
    memset(&slice, 0, sizeof(slice));
    memset(data, 0x11, sizeof(data));
    pic.frame_width = 1280;
    pic.frame_height = 720;
    pic.last_ref_frame = VA_INVALID_ID;
    pic.golden_ref_frame = VA_INVALID_ID;
    pic.alt_ref_frame = VA_INVALID_ID;
    pic.pic_fields.bits.key_frame = 0;
    slice.slice_data_size = sizeof(data);
    slice.num_of_partitions = 2;
    slice.partition_size[0] = 8;
    slice.partition_size[1] = 8;
    e.ctx.codec = VAV4L2_CODEC_VP8;
    memset(&pic_buf, 0, sizeof(pic_buf));
    memset(&sl_buf, 0, sizeof(sl_buf));
    memset(&data_buf, 0, sizeof(data_buf));
    pic_buf.type = VAPictureParameterBufferType;
    pic_buf.data = &pic;
    pic_buf.size = sizeof(pic);
    sl_buf.type = VASliceParameterBufferType;
    sl_buf.data = &slice;
    sl_buf.size = sizeof(slice);
    data_buf.type = VASliceDataBufferType;
    data_buf.data = data;
    data_buf.size = sizeof(data);
    bufs[0] = &pic_buf;
    bufs[1] = &sl_buf;
    bufs[2] = &data_buf;
    expect_true(vav4l2_vp8_translate(&e.ctx, bufs, 3) == VA_STATUS_SUCCESS,
                "vp8-translate-status");
    expect_grew_to(&e, 1280, 720, "vp8");
    recap_env_close(&e);
}

static void test_mpeg2_translate_recapture(void)
{
    struct recap_env e;
    VAPictureParameterBufferMPEG2 pic;
    uint8_t nal[] = { 0, 0, 1, 0x00, 0x00 };
    struct vav4l2_buffer pic_buf, data_buf;
    VASliceParameterBufferMPEG2 slice = {.slice_data_size = sizeof(nal)};
    struct vav4l2_buffer params = {.type = VASliceParameterBufferType, .data = &slice, .size = sizeof(slice)};
    struct vav4l2_buffer *bufs[3];

    if (recap_env_open(&e, 720, 480) < 0) {
        fprintf(stderr, "FAIL recap_env mpeg2\n");
        g_fail++;
        return;
    }
    memset(&pic, 0, sizeof(pic));
    pic.horizontal_size = 1920;
    pic.vertical_size = 1080;
    pic.forward_reference_picture = VA_INVALID_ID;
    pic.backward_reference_picture = VA_INVALID_ID;
    pic.picture_coding_type = 1;
    pic.picture_coding_extension.bits.picture_structure = 3;
    pic.picture_coding_extension.bits.progressive_frame = 1;
    e.ctx.codec = VAV4L2_CODEC_MPEG2;
    e.ctx.profile = VAProfileMPEG2Main;
    memset(&pic_buf, 0, sizeof(pic_buf));
    memset(&data_buf, 0, sizeof(data_buf));
    pic_buf.type = VAPictureParameterBufferType;
    pic_buf.data = &pic;
    pic_buf.size = sizeof(pic);
    data_buf.type = VASliceDataBufferType;
    data_buf.data = nal;
    data_buf.size = sizeof(nal);
    bufs[0] = &pic_buf;
    bufs[1] = &params; bufs[2] = &data_buf;
    expect_true(vav4l2_mpeg2_translate(&e.ctx, bufs, 3) == VA_STATUS_SUCCESS,
                "mpeg2-translate-status");
    expect_grew_to(&e, 1920, 1080, "mpeg2");
    recap_env_close(&e);
}

static void test_hevc_first_picture_order(void)
{
    struct VADriverVTable table = {0};
    struct VADriverContext driver = {.vtable = &table};
    struct vav4l2_surface surface = {.surface_id = 1, .width = 1920, .height = 1088,
        .format = VA_FOURCC_NV12, .buf_index = -1, .memfd_fd = -1};
    VASurfaceID target = 1;
    VAContextID id = VA_INVALID_ID;
    VAPictureParameterBufferHEVC pic = {.pic_width_in_luma_samples = 1920, .pic_height_in_luma_samples = 1088};
    pic.slice_parsing_fields.bits.IdrPicFlag = pic.slice_parsing_fields.bits.RapPicFlag = 1;
    uint8_t nal[] = {0, 0, 0, 1, 0x26, 1, 0xa0, 0};
    struct vav4l2_buffer picture = {.type = VAPictureParameterBufferType, .data = &pic, .size = sizeof(pic)};
    struct vav4l2_buffer data = {.type = VASliceDataBufferType, .data = nal, .size = sizeof(nal)};
    VASliceParameterBufferHEVC slice = {.slice_data_size = sizeof(nal)};
    struct vav4l2_buffer params = {.type = VASliceParameterBufferType, .data = &slice, .size = sizeof(slice)};
    struct vav4l2_buffer *buffers[] = {&picture, &params, &data};

    setenv("VAV4L2_PROBE_NOCACHE", "1", 1);
    if (__vaDriverInit_1_20(&driver) != VA_STATUS_SUCCESS) {
        expect_true(0, "hevc-startup-driver");
        return;
    }
    struct vav4l2_driver_data *dd = driver.pDriverData;
    struct vav4l2_config *config = calloc(1, sizeof(*config));
    if (!config) {
        expect_true(0, "hevc-startup-config");
        table.vaTerminate(&driver);
        return;
    }
    *config = (struct vav4l2_config){.config_id = 1, .codec = VAV4L2_CODEC_HEVC,
        .profile = VAProfileHEVCMain, .rt_format = VA_RT_FORMAT_YUV420,
        .entrypoint = VAEntrypointVLD, .device_path = "mock-hevc"};
    dd->configs = config;
    dd->surfaces[1] = &surface;
    g_context_fd = memfd_create("hevc-startup", MFD_CLOEXEC);
    if (g_context_fd < 0 || ftruncate(g_context_fd, 65536) < 0) {
        expect_true(0, "hevc-startup-device");
        goto out;
    }
    g_nops = g_reqbufs_n = g_sfmt_cap = 0;
    g_capture_query_error = 0;
    g_require_hevc_sps_first = 1;
    vav4l2_set_ioctl_hook(mock_ioctl);
    VAStatus status = table.vaCreateContext(&driver, 1, 1920, 1088, 0, &target, 1, &id);
    expect_true(status == VA_STATUS_SUCCESS, "hevc-startup-context");
    if (status == VA_STATUS_SUCCESS) {
        g_ctx = dd->contexts;
        expect_true(g_ctx->capture_bufs_allocd == 0 && g_reqbufs_n == 0 && !g_ctx->streamed,
                    "hevc-no-capture-before-sps");
        g_ctx->request_fd = dup(g_context_fd);
        g_ctx->current_surface = &surface;
        pic.pic_fields.bits.chroma_format_idc = 1;
        for (int i = 0; i < 15; i++) pic.ReferenceFrames[i].flags = VA_PICTURE_HEVC_INVALID;
        expect_true(vav4l2_hevc_translate(g_ctx, buffers, 3) == VA_STATUS_SUCCESS, "hevc-first-picture-sps-accepted");
        expect_true(op_index("HEVC_SPS") >= 0 && op_index("HEVC_SPS") < op_index("S_FMT_CAP") &&
                    op_index("S_FMT_CAP") < op_index("REQBUFSN") && op_index("REQBUFSN") < op_index("STREAMON"),
                    "hevc-sps-before-capture-before-streamon");
        table.vaDestroyContext(&driver, id);
        g_ctx = NULL;
    }
out:
    g_require_hevc_sps_first = 0;
    vav4l2_set_ioctl_hook(NULL);
    if (g_context_fd >= 0) close(g_context_fd);
    g_context_fd = -1;
    if (surface.memfd_fd >= 0) close(surface.memfd_fd);
    dd->surfaces[1] = NULL;
    table.vaTerminate(&driver);
}

static void test_translate_missing_params(void)
{
    struct vav4l2_context ctx;
    struct vav4l2_buffer *none[1] = { NULL };

    memset(&ctx, 0, sizeof(ctx));
    ctx.v4l2_fd = -1;
    ctx.request_fd = -1;
    expect_true(vav4l2_h264_translate(&ctx, none, 0) == VA_STATUS_ERROR_INVALID_PARAMETER,
                "h264-missing-pic");
    expect_true(vav4l2_hevc_translate(&ctx, none, 0) == VA_STATUS_ERROR_INVALID_PARAMETER,
                "hevc-missing-pic");
    expect_true(vav4l2_av1_translate(&ctx, none, 0) == VA_STATUS_ERROR_INVALID_PARAMETER,
                "av1-missing-pic");
    expect_true(vav4l2_vp8_translate(&ctx, none, 0) == VA_STATUS_ERROR_INVALID_PARAMETER,
                "vp8-missing-pic");
    expect_true(vav4l2_mpeg2_translate(&ctx, none, 0) == VA_STATUS_ERROR_INVALID_PARAMETER,
                "mpeg2-missing-pic");
}

static void test_export_fd_ownership(void)
{
    struct VADriverVTable table = {0};
    struct VADriverContext driver = {.vtable = &table};
    VADRMPRIMESurfaceDescriptor first = {0}, second = {0};
    VASurfaceID sid;
    setenv("VAV4L2_PROBE_NOCACHE", "1", 1);
    if (__vaDriverInit_1_20(&driver) != VA_STATUS_SUCCESS) { g_fail++; return; }
    struct vav4l2_driver_data *dd = driver.pDriverData;
    struct vav4l2_context *c = calloc(1, sizeof(*c));
    if (!c) { g_fail++; table.vaTerminate(&driver); return; }
    c->context_id = 1; c->entrypoint = VAEntrypointVLD; c->codec = VAV4L2_CODEC_H264;
    c->v4l2_fd = open("/dev/null", O_RDWR | O_CLOEXEC); c->media_fd = c->request_fd = -1;
    c->driver_data = dd; c->width = c->cap_width = c->cap_stride = 16;
    c->height = c->cap_height = 16; c->cap_pixelformat = V4L2_PIX_FMT_NV12;
    c->capture_bufs_allocd = c->n_free_cap = 1; c->free_cap_bufs[0] = 0; dd->contexts = c;
    g_last_cap_w = g_last_cap_h = 16;
    expect_true(table.vaCreateSurfaces(&driver, 16, 16, VA_RT_FORMAT_YUV420, 1, &sid) == VA_STATUS_SUCCESS,
                "export-fd-surface");
    vav4l2_set_ioctl_hook(mock_ioctl);
    expect_true(table.vaExportSurfaceHandle(&driver, sid, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME, 0, &first) ==
                VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE, "legacy-export-layout-rejected");
    expect_true(table.vaExportSurfaceHandle(&driver, sid, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, 0, &first) == VA_STATUS_SUCCESS,
                "export-first-fd");
    expect_true(first.objects[0].size == 448 && (fcntl(first.objects[0].fd, F_GETFD) & FD_CLOEXEC),
                "native-export-complete-object-size-and-cloexec");
    expect_true(table.vaExportSurfaceHandle(&driver, sid, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, 0, &second) == VA_STATUS_SUCCESS &&
                first.objects[0].fd != second.objects[0].fd && fcntl(first.objects[0].fd, F_GETFD) >= 0,
                "repeat-export-keeps-first-client-fd-open");
    VADRMPRIMESurfaceDescriptor rejected = first;
    g_export_short = 1;
    expect_true(table.vaExportSurfaceHandle(&driver, sid, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, 0, &rejected) == VA_STATUS_ERROR_OPERATION_FAILED &&
                !memcmp(&rejected, &first, sizeof(first)) && fcntl(g_export_fd, F_GETFD) < 0 &&
                fcntl(first.objects[0].fd, F_GETFD) >= 0, "short-native-export-closes-only-new-fd-and-preserves-client-descriptor");
    g_export_short = 0;
    expect_true(table.vaDestroySurfaces(&driver, &sid, 1) == VA_STATUS_SUCCESS &&
                fcntl(second.objects[0].fd, F_GETFD) >= 0 && c->n_free_cap == 1,
                "surface-destroy-keeps-client-fd-and-recycles-unlisted-slot");
    close(first.objects[0].fd); close(second.objects[0].fd);
    table.vaDestroyContext(&driver, 1);
    vav4l2_set_ioctl_hook(NULL);
    table.vaTerminate(&driver);
}

int main(void)
{
    /* This fixture checks native MMAP teardown counts;pool fallback has its own suite. */
    setenv("VAV4L2_CAPTURE_POOL_DMABUF", "0", 1);
    setenv("VAV4L2_EXPBUF_EXPORT", "1", 1); /* Mock capture/EXPBUF fds are not GBM objects. */
    test_export_fd_ownership();
    test_request_control_binding();
    test_recapture_small_to_large();
    test_ensure_noop_shrink_fmt();
    test_av1_translate_recapture();
    test_output_remap_failure();
    test_capture_clamping();
    test_h264_translate_recapture(0);
    test_h264_translate_recapture(1);
    test_hevc_translate_recapture(0);
    test_hevc_translate_recapture(1);
    test_vp8_translate_recapture();
    test_mpeg2_translate_recapture();
    test_hevc_first_picture_order();
    test_translate_missing_params();
    return g_fail ? 1 : 0;
}
