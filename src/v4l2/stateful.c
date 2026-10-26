/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Stateful decoder queues are independent; timestamps identify VA pictures. */
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "va_v4l2.h"
#include "codecs/bytestream.h"

#define PENDING_MAX 128
struct vav4l2_stateful {
    struct { VASurfaceID id; uint64_t timestamp; int active; } pending[PENDING_MAX];
    unsigned output_busy[VAV4L2_NUM_OUTPUT_BUFS], capture_busy[VAV4L2_MAX_CAPTURE_BUFS];
    unsigned rt_format;
    int output_streaming, capture_streaming, source_change, draining, last, failed, zero_delay;
};
static uint64_t monotonic_ns(void)
{
    struct timespec t; if (clock_gettime(CLOCK_MONOTONIC, &t) < 0) return UINT64_MAX;
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + t.tv_nsec;
}
static int command(struct vav4l2_context *ctx, unsigned cmd)
{
    struct v4l2_decoder_cmd c = {.cmd = cmd};
    int result = vav4l2_xioctl(ctx->v4l2_fd, VIDIOC_DECODER_CMD, &c);
    vav4l2_decode_trace(ctx, cmd == V4L2_DEC_CMD_STOP ? "stateful-drain" : "stateful-resume", result, cmd);
    return result;
}
static int fail(struct vav4l2_context *ctx)
{
    int error = errno ? errno : EIO;
    struct vav4l2_stateful *s = ctx->stateful;
    s->failed = 1;
    for (unsigned i = 0; i < PENDING_MAX; i++) if (s->pending[i].active) {
        struct vav4l2_surface *surface = vav4l2_surface_by_id(ctx->driver_data, s->pending[i].id);
        if (surface) { surface->status = VASurfaceSkipped; surface->has_pic = 0; }
        s->pending[i].active = 0;
    }
    /* Closing resolves uncertain queue ownership; the context is not reused. */
    vav4l2_close_context_device(ctx); ctx->streamed = 0; ctx->n_free_out = ctx->n_free_cap = 0;
    vav4l2_decode_trace(ctx, "stateful-failed", -1, error); errno = error; return -1;
}
int vav4l2_stateful_pending(const struct vav4l2_context *ctx, VASurfaceID id)
{
    if (!ctx || !ctx->stateful) return 0;
    for (unsigned i = 0; i < PENDING_MAX; i++) if (ctx->stateful->pending[i].active && ctx->stateful->pending[i].id == id) return 1;
    return 0;
}
int vav4l2_stateful_configure_output(struct vav4l2_context *ctx)
{
    const uint32_t ids[] = {V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY, V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY_ENABLE};
    const int values[] = {0, 1};
    struct v4l2_ext_control controls[2] = {{.id = ids[0], .value = 0}, {.id = ids[1], .value = 1}};
    for (unsigned i = 0; i < 2; i++) {
        struct v4l2_queryctrl q = {.id = ids[i]};
        if (vav4l2_xioctl(ctx->v4l2_fd, VIDIOC_QUERYCTRL, &q) < 0) {
            if (errno == EINVAL || errno == ENOTTY) return 0;
            return -1;
        }
        if ((q.flags & (V4L2_CTRL_FLAG_DISABLED | V4L2_CTRL_FLAG_READ_ONLY | V4L2_CTRL_FLAG_INACTIVE | V4L2_CTRL_FLAG_GRABBED)) ||
            q.type != (i ? V4L2_CTRL_TYPE_BOOLEAN : V4L2_CTRL_TYPE_INTEGER) || q.minimum > values[i] ||
            q.maximum < values[i] || q.step <= 0 || ((int64_t)values[i] - q.minimum) % q.step) return 0;
    }
    struct v4l2_ext_controls set = {.which = V4L2_CTRL_WHICH_CUR_VAL, .count = 2, .controls = controls};
    if (vav4l2_codec_controls(ctx, -1, &set) < 0) return -1;
    ctx->stateful->zero_delay = 1;
    vav4l2_decode_trace(ctx, "stateful-display-delay", 0, 0);
    return 0;
}
int vav4l2_stateful_init(struct vav4l2_context *ctx)
{
    if (!ctx || !vav4l2_stateful_fourcc(ctx->codec) || ctx->stateful) return -1;
    const char *direct = getenv("VAV4L2_CAPTURE_DMABUF"), *pool = getenv("VAV4L2_CAPTURE_POOL_DMABUF");
    if ((direct && !strcmp(direct, "required")) || (pool && !strcmp(pool, "required"))) { errno = ENOTSUP; return -1; }
    ctx->stateful = calloc(1, sizeof(*ctx->stateful)); if (!ctx->stateful) return -1;
    struct v4l2_event_subscription subscription = {.type = V4L2_EVENT_SOURCE_CHANGE};
    if (vav4l2_xioctl(ctx->v4l2_fd, VIDIOC_SUBSCRIBE_EVENT, &subscription) < 0) return -1;
    int count = vav4l2_setup_codec_output(ctx, ctx->coded_format, ctx->width, ctx->height);
    if (count <= 0) return -1;
    ctx->output_bufs_allocd = count;
    if (vav4l2_mmap_output_buffers(ctx->v4l2_fd, count, ctx->output_buf_ptr, &ctx->output_buf_size) < 0) return -1;
    for (int i = 0; i < count; i++) vav4l2_out_pool_push(ctx, i);
    if (vav4l2_streamon(ctx->v4l2_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) < 0) return -1;
    ctx->stateful->output_streaming = 1;
    return 0;
}
void vav4l2_stateful_finish(struct vav4l2_context *ctx)
{
    if (!ctx || !ctx->stateful) return;
    struct vav4l2_stateful *s = ctx->stateful;
    if (ctx->v4l2_fd >= 0) {
        int a = !s->output_streaming || vav4l2_streamoff(ctx->v4l2_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) == 0;
        int b = !s->capture_streaming || vav4l2_streamoff(ctx->v4l2_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) == 0;
        if (!a || !b) vav4l2_close_context_device(ctx);
    }
    for (unsigned i = 0; i < PENDING_MAX; i++) if (s->pending[i].active) {
        struct vav4l2_surface *surface = vav4l2_surface_by_id(ctx->driver_data, s->pending[i].id);
        if (surface) surface->status = VASurfaceSkipped;
    }
    free(s); ctx->stateful = NULL; ctx->streamed = 0;
    vav4l2_packet_state_release(&ctx->packet_state);
}
static int queue_capture(struct vav4l2_context *ctx)
{
    struct vav4l2_stateful *s = ctx->stateful;
    while (ctx->n_free_cap) {
        int index = ctx->free_cap_bufs[ctx->n_free_cap - 1];
        if (index < 0 || index >= ctx->capture_bufs_allocd || s->capture_busy[index]) { errno = EINVAL; return -1; }
        if (vav4l2_decoder_queue_capture(ctx, index) < 0) return -1;
        ctx->n_free_cap--; s->capture_busy[index] = 1;
        vav4l2_decode_trace(ctx, "stateful-capture-qbuf", 0, index);
    }
    return 0;
}
static int setup_capture(struct vav4l2_context *ctx)
{
    struct vav4l2_stateful *s = ctx->stateful;
    if (ctx->capture_bufs_allocd) {
        if (s->capture_streaming && vav4l2_streamoff(ctx->v4l2_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) < 0) return -1;
        s->capture_streaming = 0;
        if (vav4l2_release_decoder_capture(ctx) < 0) return -1;
    }
    struct v4l2_format f = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE};
    if (vav4l2_xioctl(ctx->v4l2_fd, VIDIOC_G_FMT, &f) < 0) return -1;
    /* Enumerate after parsing headers; choose implemented LINEAR storage. */
    uint32_t wanted = s->rt_format == VA_RT_FORMAT_YUV420 ? V4L2_PIX_FMT_NV12 :
        s->rt_format == VA_RT_FORMAT_YUV420_10 ? V4L2_PIX_FMT_P010 : 0;
    struct v4l2_fmtdesc menu = {.type = f.type}; int found = 0, planar = 0; uint32_t tiled = 0;
    for (menu.index = 0; menu.index < VAV4L2_MAX_FORMATS; menu.index++) {
        if (vav4l2_xioctl(ctx->v4l2_fd, VIDIOC_ENUM_FMT, &menu) < 0) { if (errno != EINVAL) return -1; break; }
        if (menu.pixelformat == wanted) found = 1;
        if (!planar && vav4l2_capture_layout_format(menu.pixelformat) == wanted) planar = menu.pixelformat;
        if (!tiled && vav4l2_capture_tile(menu.pixelformat) && vav4l2_va_fourcc_for_capture(menu.pixelformat) == vav4l2_va_fourcc_for_capture(wanted)) tiled = menu.pixelformat;
    }
    if (!wanted || (!found && !planar && !tiled)) { errno = ENOTSUP; return -1; }
    uint32_t selected = found ? wanted : planar ? (uint32_t)planar : tiled;
    f.fmt.pix_mp.pixelformat = selected;
    f.fmt.pix_mp.num_planes = planar && selected == (uint32_t)planar ? vav4l2_capture_layout_planes(selected) : 1;
    if (vav4l2_codec_format(ctx, &f) < 0) return -1;
    struct v4l2_pix_format_mplane *p = &f.fmt.pix_mp;
    if (p->width < (unsigned)ctx->width || p->height < (unsigned)ctx->height) { errno = ENOTSUP; return -1; }
    if (vav4l2_capture_layout_format(p->pixelformat) == wanted) {
        if (vav4l2_capture_layout_set(ctx, p) < 0) return -1;
    } else {
        uint32_t need = vav4l2_capture_plane_size(p->pixelformat, p->plane_fmt[0].bytesperline, p->height);
        if (p->pixelformat != selected || p->num_planes != 1 || p->field != V4L2_FIELD_NONE || p->height > 8192 ||
            (wanted == V4L2_PIX_FMT_P010 && (p->plane_fmt[0].bytesperline & 1)) ||
            p->plane_fmt[0].bytesperline < ((((unsigned)ctx->width + 1u) & ~1u) * (wanted == V4L2_PIX_FMT_P010 ? 2u : 1u)) ||
            !need || p->plane_fmt[0].sizeimage < need) { errno = ENOTSUP; return -1; }
    }
    struct v4l2_selection selection = {.type = f.type, .target = V4L2_SEL_TGT_COMPOSE};
    if (vav4l2_xioctl(ctx->v4l2_fd, VIDIOC_G_SELECTION, &selection) == 0) {
        if (selection.r.left || selection.r.top || selection.r.width != (unsigned)ctx->width || selection.r.height != (unsigned)ctx->height) { errno = ENOTSUP; return -1; }
    } else if (errno != EINVAL && errno != ENOTTY) return -1;
    struct v4l2_control minimum = {.id = V4L2_CID_MIN_BUFFERS_FOR_CAPTURE};
    unsigned count = VAV4L2_NUM_CAPTURE_BUFS;
    if (vav4l2_xioctl(ctx->v4l2_fd, VIDIOC_G_CTRL, &minimum) == 0) {
        if (minimum.value < 1 || minimum.value > VAV4L2_MAX_CAPTURE_BUFS - 4) { errno = ENOBUFS; return -1; }
        count = minimum.value + 16;
        if (count > VAV4L2_MAX_CAPTURE_BUFS) count = VAV4L2_MAX_CAPTURE_BUFS;
    } else if (errno != EINVAL && errno != ENOTTY) return -1;
    struct v4l2_requestbuffers request = {.type = f.type, .memory = V4L2_MEMORY_MMAP, .count = count};
    if (vav4l2_xioctl(ctx->v4l2_fd, VIDIOC_REQBUFS, &request) < 0) return -1;
    if (!request.count || request.count > VAV4L2_MAX_CAPTURE_BUFS || request.count < (unsigned)(minimum.value > 0 ? minimum.value + 4 : 4)) { errno = ENOBUFS; return -1; }
    ctx->capture_bufs_allocd = request.count; ctx->capture_memory = V4L2_MEMORY_MMAP;
    ctx->cap_width = p->width; ctx->cap_height = p->height; ctx->cap_stride = p->plane_fmt[0].bytesperline;
    if (!ctx->capture_layout) { ctx->cap_sizeimage = p->plane_fmt[0].sizeimage; ctx->cap_pixelformat = p->pixelformat; }
    memset(s->capture_busy, 0, sizeof(s->capture_busy)); ctx->n_free_cap = 0;
    for (unsigned i = 0; i < request.count; i++) vav4l2_cap_pool_push(ctx, i);
    if (queue_capture(ctx) < 0 || vav4l2_streamon(ctx->v4l2_fd, f.type) < 0) return -1;
    /* CAPTURE restart also aborts an explicit drain interrupted by DRC. */
    s->capture_streaming = 1; s->source_change = s->last = s->draining = 0;
    vav4l2_decode_trace(ctx, "stateful-capture-ready", 0, request.count);
    return 0;
}
int vav4l2_stateful_pump(struct vav4l2_context *ctx)
{
    if (!ctx || !ctx->stateful || ctx->stateful->failed || ctx->v4l2_fd < 0) return -1;
    struct vav4l2_stateful *s = ctx->stateful;
    struct v4l2_event event;
    for (;;) {
        memset(&event, 0, sizeof(event));
        if (vav4l2_xioctl(ctx->v4l2_fd, VIDIOC_DQEVENT, &event) < 0) { if (errno == EAGAIN || errno == ENOENT) break; return fail(ctx); }
        if (event.type != V4L2_EVENT_SOURCE_CHANGE || !(event.u.src_change.changes & V4L2_EVENT_SRC_CH_RESOLUTION)) { errno = ENOTSUP; return fail(ctx); }
        s->source_change = 1; vav4l2_decode_trace(ctx, "stateful-source-change", 0, event.u.src_change.changes);
    }
    if (s->source_change && !ctx->capture_bufs_allocd && setup_capture(ctx) < 0) return fail(ctx);
    for (;;) {
        struct v4l2_plane plane = {0};
        struct v4l2_buffer b = {.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, .memory = V4L2_MEMORY_MMAP, .length = 1, .m.planes = &plane};
        if (vav4l2_xioctl(ctx->v4l2_fd, VIDIOC_DQBUF, &b) < 0) { if (errno == EAGAIN) break; return fail(ctx); }
        if (b.index >= (unsigned)ctx->output_bufs_allocd || !s->output_busy[b.index] || (b.flags & V4L2_BUF_FLAG_ERROR)) { errno = EIO; return fail(ctx); }
        s->output_busy[b.index] = 0; vav4l2_out_pool_push(ctx, b.index);
        vav4l2_decode_trace(ctx, "stateful-output-done", 0, b.index);
    }
    if (s->capture_streaming && !s->last) for (;;) {
        struct v4l2_buffer b = {0};
        if (vav4l2_decoder_dequeue_capture(ctx, &b) < 0) { if (errno == EAGAIN) break; return fail(ctx); }
        if (!s->capture_busy[b.index] || (b.flags & V4L2_BUF_FLAG_ERROR)) { errno = EIO; return fail(ctx); }
        s->capture_busy[b.index] = 0;
        if (b.bytesused) {
            uint32_t need = vav4l2_capture_plane_size(ctx->cap_pixelformat, ctx->cap_stride, ctx->cap_height);
            if (!need || b.bytesused < need || b.bytesused > ctx->cap_sizeimage || b.timestamp.tv_sec < 0 || b.timestamp.tv_usec < 0 || b.timestamp.tv_usec >= 1000000) { errno = EIO; return fail(ctx); }
            uint64_t timestamp = ((uint64_t)b.timestamp.tv_sec * 1000000 + b.timestamp.tv_usec) * 1000;
            int found = -1;
            for (unsigned i = 0; i < PENDING_MAX; i++) if (s->pending[i].active && s->pending[i].timestamp == timestamp) { found = i; break; }
            struct vav4l2_surface *target = found < 0 ? NULL : vav4l2_surface_by_id(ctx->driver_data, s->pending[found].id);
            if (!target || target->timestamp != timestamp || vav4l2_surface_pull_capture(ctx, target, b.index) < 0) { errno = EIO; return fail(ctx); }
            target->status = VASurfaceReady; s->pending[found].active = 0;
            vav4l2_decode_trace(ctx, "stateful-capture-done", 0, timestamp);
        } else {
            if (!(b.flags & V4L2_BUF_FLAG_LAST)) { errno = EIO; return fail(ctx); }
            vav4l2_cap_pool_push(ctx, b.index);
        }
        if (b.flags & V4L2_BUF_FLAG_LAST) { s->last = 1; break; }
    }
    if (s->last && s->source_change) {
        if (setup_capture(ctx) < 0) return fail(ctx);
    } else if (s->last && s->draining) {
        int output_pending = 0;
        for (int i = 0; i < ctx->output_bufs_allocd; i++) output_pending |= s->output_busy[i];
        if (!output_pending) {
            for (unsigned i = 0; i < PENDING_MAX; i++) if (s->pending[i].active) { errno = EIO; return fail(ctx); }
            if (command(ctx, V4L2_DEC_CMD_START) < 0) return fail(ctx);
            s->last = s->draining = 0;
        }
    } else if (s->last) { errno = EIO; return fail(ctx); }
    if (s->capture_streaming && queue_capture(ctx) < 0) return fail(ctx);
    return 0;
}
static int wait_progress(struct vav4l2_context *ctx, uint64_t deadline)
{
    uint64_t now = monotonic_ns(); if (now == UINT64_MAX) return -1;
    if (deadline != UINT64_MAX && now >= deadline) { errno = EAGAIN; return 0; }
    /* Poll only for outstanding work; POLLOUT with no OUTPUT in flight spins. */
    short events = POLLPRI;
    if (ctx->stateful->capture_streaming && !ctx->stateful->last) events |= POLLIN;
    for (int i = 0; i < ctx->output_bufs_allocd; i++) if (ctx->stateful->output_busy[i]) events |= POLLOUT;
    uint64_t left = deadline == UINT64_MAX ? 1000000000 : deadline - now;
    int ms = left / 1000000 > 1000 ? 1000 : (int)((left + 999999) / 1000000);
    struct pollfd fd = {.fd = ctx->v4l2_fd, .events = events};
    int result = poll(&fd, 1, ms);
    if (result < 0 && errno == EINTR) return 1;
    if (result < 0 || (fd.revents & (POLLNVAL | POLLHUP))) return -1;
    /* POLLERR is expected before CAPTURE initialization, alongside POLLPRI. */
    return 1;
}
VAStatus vav4l2_stateful_sync(struct vav4l2_context *ctx, struct vav4l2_surface *surface, uint64_t timeout)
{
    if (!ctx || !ctx->stateful || !surface) return VA_STATUS_ERROR_INVALID_PARAMETER;
    uint64_t start = monotonic_ns(); if (start == UINT64_MAX) return VA_STATUS_ERROR_OPERATION_FAILED;
    uint64_t deadline = timeout == VA_TIMEOUT_INFINITE || timeout > UINT64_MAX - start ? UINT64_MAX : start + timeout;
    for (;;) {
        if (vav4l2_stateful_pump(ctx) < 0) return VA_STATUS_ERROR_DECODING_ERROR;
        if (!vav4l2_stateful_pending(ctx, surface->surface_id)) return surface->status == VASurfaceSkipped ? VA_STATUS_ERROR_DECODING_ERROR : VA_STATUS_SUCCESS;
        if (!timeout) return VA_STATUS_ERROR_TIMEDOUT;
        /* VA has no end-of-stream callback. Drain submitted pictures when a
         * client needs one; START resumes without a seek or DPB destruction. */
        if (!ctx->stateful->zero_delay && ctx->stateful->capture_streaming && !ctx->stateful->draining && !ctx->stateful->source_change) {
            if (command(ctx, V4L2_DEC_CMD_STOP) < 0) { fail(ctx); return VA_STATUS_ERROR_DECODING_ERROR; }
            ctx->stateful->draining = 1;
        }
        int ready = wait_progress(ctx, deadline);
        if (ready < 0) { fail(ctx); return VA_STATUS_ERROR_DECODING_ERROR; }
        if (!ready) { vav4l2_decode_trace(ctx, "stateful-sync-timeout", 0, timeout); return VA_STATUS_ERROR_TIMEDOUT; }
    }
}
static VAStatus reference(struct vav4l2_context *ctx, VASurfaceID id)
{
    struct vav4l2_surface *s = vav4l2_surface_by_id(ctx->driver_data, id);
    if (!s || s == ctx->current_surface || s->status == VASurfaceSkipped || (s->last_writer == VAV4L2_WRITER_CPU && !s->prime_capture_valid) ||
        (!vav4l2_stateful_pending(ctx, id) && (!s->has_pic || s->capture_ctx != ctx))) return VA_STATUS_ERROR_INVALID_SURFACE;
    return VA_STATUS_SUCCESS;
}
VAStatus vav4l2_stateful_translate(struct vav4l2_context *ctx, struct vav4l2_buffer **buffers, int count)
{
    if (!ctx || !ctx->current_surface || vav4l2_stateful_pump(ctx) < 0) return VA_STATUS_ERROR_DECODING_ERROR;
    if (!vav4l2_packet_buffers_valid(ctx->codec, buffers, count)) return VA_STATUS_ERROR_INVALID_PARAMETER;
    struct vav4l2_stateful *s = ctx->stateful;
    struct vav4l2_collected cb; vav4l2_collect_decode_buffers(buffers, count, &cb);
    if (!cb.pic) return VA_STATUS_ERROR_INVALID_PARAMETER;
    unsigned rt = VA_RT_FORMAT_YUV420;
    if (ctx->codec == VAV4L2_CODEC_HEVC && ((const VAPictureParameterBufferHEVC *)cb.pic)->bit_depth_luma_minus8) rt = VA_RT_FORMAT_YUV420_10;
    if (ctx->codec == VAV4L2_CODEC_VP9 && ((const VADecPictureParameterBufferVP9 *)cb.pic)->bit_depth > 8) rt = VA_RT_FORMAT_YUV420_10;
    if (ctx->codec == VAV4L2_CODEC_AV1 && ((const VADecPictureParameterBufferAV1 *)cb.pic)->bit_depth_idx) rt = VA_RT_FORMAT_YUV420_10;
    if (!(ctx->rt_format & rt) || (s->rt_format && s->rt_format != rt) ||
        ctx->current_surface->format != (rt == VA_RT_FORMAT_YUV420_10 ? VA_FOURCC_P010 : VA_FOURCC_NV12))
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    if (ctx->codec == VAV4L2_CODEC_H264) {
        const VAPictureParameterBufferH264 *p = cb.pic;
        for (unsigned i = 0; i < 16; i++) if (!(p->ReferenceFrames[i].flags & VA_PICTURE_H264_INVALID) &&
            (p->ReferenceFrames[i].flags & (VA_PICTURE_H264_SHORT_TERM_REFERENCE | VA_PICTURE_H264_LONG_TERM_REFERENCE))) {
            VAStatus status = reference(ctx, p->ReferenceFrames[i].picture_id); if (status != VA_STATUS_SUCCESS) return status;
        }
    } else if (ctx->codec == VAV4L2_CODEC_HEVC) {
        const VAPictureParameterBufferHEVC *p = cb.pic;
        for (unsigned i = 0; i < 15; i++) if (!(p->ReferenceFrames[i].flags & VA_PICTURE_HEVC_INVALID)) {
            VAStatus status = reference(ctx, p->ReferenceFrames[i].picture_id); if (status != VA_STATUS_SUCCESS) return status;
        }
    } else if (ctx->codec == VAV4L2_CODEC_VP9) {
        const VADecPictureParameterBufferVP9 *p = cb.pic;
        if (p->pic_fields.bits.frame_type && !p->pic_fields.bits.intra_only) {
            unsigned slots[] = {p->pic_fields.bits.last_ref_frame, p->pic_fields.bits.golden_ref_frame, p->pic_fields.bits.alt_ref_frame};
            for (unsigned i = 0; i < 3; i++) {
                VAStatus status = reference(ctx, p->reference_frames[slots[i]]); if (status != VA_STATUS_SUCCESS) return status;
            }
        }
    } else if (ctx->codec == VAV4L2_CODEC_AV1) {
        const VADecPictureParameterBufferAV1 *p = cb.pic;
        if (p->pic_info_fields.bits.frame_type == 1 || p->pic_info_fields.bits.frame_type == 3)
            for (unsigned i = 0; i < 7; i++) {
                if (p->ref_frame_idx[i] >= 8) return VA_STATUS_ERROR_INVALID_PARAMETER;
                VAStatus status = reference(ctx, p->ref_frame_map[p->ref_frame_idx[i]]); if (status != VA_STATUS_SUCCESS) return status;
            }
    }
    int slot = -1;
    for (unsigned i = 0; i < PENDING_MAX; i++) if (!s->pending[i].active) { slot = i; break; }
    if (slot < 0) return VA_STATUS_ERROR_SURFACE_BUSY;
    uint64_t start = monotonic_ns(), deadline = start + UINT64_C(5000000000);
    while (!ctx->n_free_out || s->draining || s->source_change) {
        if (vav4l2_stateful_pump(ctx) < 0) return VA_STATUS_ERROR_DECODING_ERROR;
        if (ctx->n_free_out && !s->draining && !s->source_change) break;
        int ready = wait_progress(ctx, deadline);
        if (ready <= 0) { fail(ctx); return VA_STATUS_ERROR_DECODING_ERROR; }
    }
    int index = ctx->free_out_bufs[ctx->n_free_out - 1]; size_t bytes = 0;
    if (index < 0 || index >= ctx->output_bufs_allocd || s->output_busy[index]) return VA_STATUS_ERROR_OPERATION_FAILED;
    struct vav4l2_packet_state prepared;
    struct vav4l2_packet_layout layout;
    VAStatus status = vav4l2_stateful_packet_prepare(ctx, buffers, count, ctx->output_buf_ptr[index], ctx->output_buf_size, &bytes, &prepared, &layout);
    if (status != VA_STATUS_SUCCESS) return status;
    if (!layout.frame_size[0] || layout.frame_size[0] > bytes || layout.frame_size[1] != bytes - layout.frame_size[0]) {
        vav4l2_packet_state_release(&prepared); return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    /* Save before QBUF transfers ownership, including when only one OUTPUT
     * slot was granted and must be dequeued before show-existing is queued. */
    uint8_t *extra = layout.frame_size[1] ? malloc(layout.frame_size[1]) : NULL;
    if (layout.frame_size[1] && !extra) { vav4l2_packet_state_release(&prepared); return VA_STATUS_ERROR_ALLOCATION_FAILED; }
    if (extra) memcpy(extra, (uint8_t *)ctx->output_buf_ptr[index] + layout.frame_size[0], layout.frame_size[1]);
    uint64_t us = ctx->current_surface->timestamp / 1000;
    s->rt_format = rt;
    s->pending[slot].id = ctx->current_surface_id; s->pending[slot].timestamp = ctx->current_surface->timestamp; s->pending[slot].active = 1;
    ctx->current_surface->has_pic = 0;
    for (unsigned frame = 0; frame < 2 && layout.frame_size[frame]; frame++) {
        while (!ctx->n_free_out) {
            if (vav4l2_stateful_pump(ctx) < 0) { free(extra); vav4l2_packet_state_release(&prepared); return VA_STATUS_ERROR_DECODING_ERROR; }
            if (ctx->n_free_out) break;
            if (wait_progress(ctx, deadline) <= 0) { free(extra); vav4l2_packet_state_release(&prepared); fail(ctx); return VA_STATUS_ERROR_DECODING_ERROR; }
        }
        index = ctx->free_out_bufs[ctx->n_free_out - 1];
        if (index < 0 || index >= ctx->output_bufs_allocd || s->output_busy[index]) { free(extra); vav4l2_packet_state_release(&prepared); errno = EIO; fail(ctx); return VA_STATUS_ERROR_DECODING_ERROR; }
        if (frame) { memcpy(ctx->output_buf_ptr[index], extra, layout.frame_size[frame]); free(extra); extra = NULL; }
        struct v4l2_plane plane = {.bytesused = layout.frame_size[frame], .length = ctx->output_buf_size};
        struct v4l2_buffer b = {.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, .memory = V4L2_MEMORY_MMAP, .index = index,
            .length = 1, .m.planes = &plane, .flags = V4L2_BUF_FLAG_TIMESTAMP_COPY};
        b.timestamp.tv_sec = us / 1000000; b.timestamp.tv_usec = us % 1000000;
        if (vav4l2_xioctl(ctx->v4l2_fd, VIDIOC_QBUF, &b) < 0) { free(extra); vav4l2_packet_state_release(&prepared); fail(ctx); return VA_STATUS_ERROR_DECODING_ERROR; }
        ctx->n_free_out--; s->output_busy[index] = 1;
        vav4l2_decode_trace(ctx, "stateful-output-qbuf", 0, ctx->current_surface->timestamp);
    }
    vav4l2_packet_state_commit(ctx, &prepared);
    return vav4l2_stateful_pump(ctx) < 0 ? VA_STATUS_ERROR_DECODING_ERROR : VA_STATUS_SUCCESS;
}
