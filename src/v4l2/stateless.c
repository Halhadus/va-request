/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Stateless Request decoder setup, submission and recovery. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <linux/media.h>
#include "va_v4l2.h"
#include "probe.h"
#include "internal.h"

/*
 * Allocate a V4L2 request
 */
int vav4l2_request_alloc(int media_fd)
{
    int request_fd = -1;
    if (vav4l2_xioctl(media_fd, MEDIA_IOC_REQUEST_ALLOC, &request_fd) < 0) {
        fprintf(stderr, "v4l2: REQUEST_ALLOC failed: %s\n", strerror(errno));
        return -1;
    }
    return request_fd;
}

int vav4l2_stateless_init(struct vav4l2_context *context)
{
    int picture_width = context->width, picture_height = context->height;
    int num_render_targets = context->num_render_targets;

    /* Open the associated media device for Request API. */
    context->media_fd = vav4l2_open_media_for_device(context->device_path);
    if (context->media_fd < 0) {
        fprintf(stderr, "v4l2: no media request node for %s\n",
                context->device_path);
        return -1;
    }

    /* Determine V4L2 codec format */
    uint32_t v4l2_format;
    switch (context->codec) {
    case VAV4L2_CODEC_H264:  v4l2_format = V4L2_PIX_FMT_H264_SLICE; break;
    case VAV4L2_CODEC_HEVC:  v4l2_format = V4L2_PIX_FMT_HEVC_SLICE; break;
    case VAV4L2_CODEC_AV1:   v4l2_format = V4L2_PIX_FMT_AV1_FRAME; break;
    case VAV4L2_CODEC_VP8:   v4l2_format = V4L2_PIX_FMT_VP8_FRAME; break;
    case VAV4L2_CODEC_MPEG2: v4l2_format = V4L2_PIX_FMT_MPEG2_SLICE; break;
    default: v4l2_format = V4L2_PIX_FMT_H264_SLICE; break;
    }

    /* Setup output queue (compressed input) */
    int n_out = vav4l2_setup_codec_output(context, v4l2_format,
                                          picture_width, picture_height);
    if (n_out <= 0) {
        fprintf(stderr, "v4l2: failed to setup output queue\n");
        return -1;
    }
    context->output_bufs_allocd = n_out;
    /* Mmap the output buffers for writing compressed data */
    if (vav4l2_mmap_output_buffers(context->v4l2_fd, n_out,
                                   context->output_buf_ptr,
                                   &context->output_buf_size) < 0) {
        fprintf(stderr, "v4l2: failed to mmap output buffers\n");
        return -1;
    }

    /* Capture: S_FMT + REQBUFS (degrades the buffer count under CMA
     * pressure). Failing the context outright beats a zombie fd that
     * errors on every frame — ffmpeg/Chrome fall back to software.
     *
     * H.264 and HEVC defer capture until the first picture: rkvdec's
     * SPS control selects the capture format and returns EBUSY if capture
     * buffers already exist. Set SPS before capture S_FMT/REQBUFS. */
    const char *capture_dma = getenv("VAV4L2_CAPTURE_DMABUF");
    int dynamic_dma = capture_dma && strcmp(capture_dma, "0") && !num_render_targets &&
        (context->codec == VAV4L2_CODEC_VP8 || context->codec == VAV4L2_CODEC_MPEG2);
    if (context->codec != VAV4L2_CODEC_H264 && context->codec != VAV4L2_CODEC_HEVC && !dynamic_dma) {
        uint32_t cap = vav4l2_capture_fourcc_from_rt(context->rt_format);
        int ok = vav4l2_ensure_capture(context, picture_width, picture_height,
                                       cap) == 0;

        if (!ok && cap != V4L2_PIX_FMT_NV12) {
            /* SPS not set yet — fall back to NV12, first picture will
             * renegotiate once bit depth is known. */
            ok = vav4l2_ensure_capture(context, picture_width, picture_height,
                                       V4L2_PIX_FMT_NV12) == 0;
        }
        if (!ok) {
            fprintf(stderr, "v4l2: capture queue setup failed\n");
            return -1;
        }
    }

    /* Start queues after sequence-dependent CAPTURE setup. */
    if (context->codec != VAV4L2_CODEC_HEVC &&
        context->codec != VAV4L2_CODEC_AV1 &&
        context->codec != VAV4L2_CODEC_MPEG2 &&
        context->codec != VAV4L2_CODEC_H264 &&
        context->output_bufs_allocd > 0 &&
        context->capture_bufs_allocd > 0) {
        if (vav4l2_streamon(context->v4l2_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) < 0 ||
            vav4l2_streamon(context->v4l2_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) < 0)
            fprintf(stderr, "v4l2: warning: STREAMON failed, decode will not work\n");
        else
            context->streamed = 1;
    }

    /* All output buffers start free. Capture free list is filled by
     * vav4l2_ensure_capture. */
    for (int i = 0; i < context->output_bufs_allocd &&
                    i < VAV4L2_NUM_OUTPUT_BUFS; i++)
        vav4l2_out_pool_push(context, i);

    vav4l2_av1_dpb_model_reset(context);
    return 0;
}

VAStatus vav4l2_stateless_translate(struct vav4l2_context *ctx, struct vav4l2_buffer **buffers, int count)
{
    switch (ctx->codec) {
    case VAV4L2_CODEC_H264: return vav4l2_h264_translate(ctx, buffers, count);
    case VAV4L2_CODEC_HEVC: return vav4l2_hevc_translate(ctx, buffers, count);
    case VAV4L2_CODEC_AV1: return vav4l2_av1_translate(ctx, buffers, count);
    case VAV4L2_CODEC_VP8: return vav4l2_vp8_translate(ctx, buffers, count);
    case VAV4L2_CODEC_MPEG2: return vav4l2_mpeg2_translate(ctx, buffers, count);
    default: return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
    }
}

/* Rebuild both free pools from scratch. Capture indices still claimed by a
 * render-target surface (surf->buf_index) stay claimed — begin_picture
 * returns them when the surface is re-targeted. */
static void rebuild_free_pools(struct vav4l2_context *ctx)
{
    int claimed[VAV4L2_MAX_CAPTURE_BUFS] = { 0 };
    int i;

    ctx->n_free_out = 0;
    for (i = 0; i < ctx->output_bufs_allocd && i < VAV4L2_NUM_OUTPUT_BUFS; i++)
        vav4l2_out_pool_push(ctx, i);

    if (ctx->driver_data) {
        for (i = 0; i < VAV4L2_MAX_SURFACES; i++) {
            struct vav4l2_surface *s = ctx->driver_data->surfaces[i];
            if (vav4l2_surface_capture_matches(ctx, s))
                claimed[s->buf_index] = 1;
        }
    }

    ctx->n_free_cap = 0;
    for (i = 0; i < ctx->capture_bufs_allocd && i < VAV4L2_MAX_CAPTURE_BUFS; i++)
        if (!claimed[i])
            vav4l2_cap_pool_push(ctx, i);
}

static void close_failed_decoder(struct vav4l2_context *ctx)
{
    if (ctx->request_fd >= 0) { close(ctx->request_fd); ctx->request_fd = -1; }
    ctx->streamed = 0;
    vav4l2_av1_dpb_model_reset(ctx);
    /* Ownership is unknown after a failed stop. Closing the device
     * cancels its queues; never recycle those indices as free. */
    vav4l2_close_context_device(ctx);
    ctx->n_free_out = ctx->n_free_cap = 0; ctx->g_ctrl_valid = 0;
    if (ctx->driver_data) for (int i = 0; i < VAV4L2_MAX_SURFACES; i++) {
        struct vav4l2_surface *s = ctx->driver_data->surfaces[i];
        if (!vav4l2_surface_capture_matches(ctx, s)) continue;
        if (s->last_writer != VAV4L2_WRITER_CPU) { s->has_pic = 0; s->status = VASurfaceSkipped; }
        s->buf_index = -1; s->cap_view = NULL; s->capture_ctx = NULL;
    }
    fprintf(stderr, "v4l2: failed STREAMOFF closed decoder; recreate the context\n");
}

VAStatus vav4l2_validate_reference(const struct vav4l2_context *ctx, VASurfaceID id)
{
    if (!ctx || !ctx->driver_data) return VA_STATUS_ERROR_INVALID_CONTEXT;
    const struct vav4l2_surface *s = vav4l2_surface_by_id(ctx->driver_data, id);
    if (!s) return VA_STATUS_ERROR_INVALID_SURFACE;
    if (s->status == VASurfaceRendering) return VA_STATUS_ERROR_SURFACE_BUSY;
    if (!s->has_pic || s->status == VASurfaceSkipped || (s->last_writer == VAV4L2_WRITER_CPU && !s->prime_capture_valid))
        return VA_STATUS_ERROR_DECODING_ERROR;
    if (!vav4l2_surface_capture_matches(ctx, s)) return VA_STATUS_ERROR_INVALID_SURFACE;
    return VA_STATUS_SUCCESS;
}

void vav4l2_decode_reset(struct vav4l2_context *ctx)
{
    vav4l2_h264_discard_field(ctx, 1);
    if (!ctx || ctx->v4l2_fd < 0)
        return;

    fprintf(stderr, "v4l2: resetting decode queues\n");

    int stop_failed = 0;
    if (ctx->streamed) {
        int output = vav4l2_streamoff(ctx->v4l2_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
        int capture = vav4l2_streamoff(ctx->v4l2_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
        stop_failed = output < 0 || capture < 0;
        ctx->streamed = 0;
    }
    /* STREAMOFF also drops every kernel DPB reference. */
    vav4l2_av1_dpb_model_reset(ctx);

    if (ctx->request_fd >= 0) {
        close(ctx->request_fd);
        ctx->request_fd = -1;
    }

    if (stop_failed) {
        close_failed_decoder(ctx);
        return;
    }

    /* STREAMOFF hands every queued buffer back in ERROR state (rkvdec and
     * hantro both do this in stop_streaming), so from this point every
     * allocated index is userspace-owned again. */
    rebuild_free_pools(ctx);
}

static int queue_output_flags(struct vav4l2_context *ctx, int index, uint32_t size, uint64_t timestamp, unsigned flags);

int vav4l2_h264_field_supported(struct vav4l2_context *ctx)
{
    /* CREATE_BUFS(count=0) queries capabilities without releasing an existing
     * queue, unlike REQBUFS(count=0). Do not infer HOLD from the codec fourcc. */
    struct v4l2_create_buffers query = {.memory = V4L2_MEMORY_MMAP};
    query.format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    return ctx && vav4l2_xioctl(ctx->v4l2_fd, VIDIOC_G_FMT, &query.format) == 0 &&
        vav4l2_xioctl(ctx->v4l2_fd, VIDIOC_CREATE_BUFS, &query) == 0 &&
        (query.capabilities & V4L2_BUF_CAP_SUPPORTS_M2M_HOLD_CAPTURE_BUF);
}

static int decode_submit(struct vav4l2_context *ctx, int out_buf_idx,
    uint32_t bytesused, uint64_t timestamp, int field_capture, int hold)
{
    struct pollfd pfd;
    int cap_buf_idx, done_cap, done_out, out_err = 0;
    int claimed = 0;

    if (!ctx || ctx->v4l2_fd < 0 || ctx->request_fd < 0)
        return -1;
    if (out_buf_idx < 0 || out_buf_idx >= ctx->output_bufs_allocd ||
        out_buf_idx >= VAV4L2_NUM_OUTPUT_BUFS || !bytesused || bytesused > ctx->output_buf_size)
        return -1;

    vav4l2_decode_trace(ctx, "submit", 0, bytesused);
    cap_buf_idx = field_capture;
    if (field_capture >= ctx->capture_bufs_allocd) return -1;
    if (field_capture < 0 && (ctx->capture_memory == V4L2_MEMORY_DMABUF || vav4l2_expbuf_export_wanted()) && ctx->current_surface &&
        ctx->current_surface->buf_index >= 0) {
        if (!vav4l2_surface_capture_matches(ctx, ctx->current_surface)) {
            vav4l2_decode_trace(ctx, "capture-owner", -1, out_buf_idx);
            fprintf(stderr, "v4l2: capture slot belongs to another context\n");
            vav4l2_out_pool_push(ctx, out_buf_idx);
            return -1;
        }
        cap_buf_idx = ctx->current_surface->buf_index;
    }
    claimed = cap_buf_idx >= 0;
    if (cap_buf_idx < 0) {
        if (ctx->n_free_cap == 0) {
            vav4l2_decode_trace(ctx, "capture-empty", -1, out_buf_idx);
            fprintf(stderr, "v4l2: no free capture buffer\n");

            vav4l2_out_pool_push(ctx, out_buf_idx);
            return -1;
        }
        cap_buf_idx = ctx->free_cap_bufs[--ctx->n_free_cap];
    }

    if (field_capture < 0 && ctx->capture_memory == V4L2_MEMORY_DMABUF && vav4l2_capture_dma_bind(ctx, cap_buf_idx) < 0) {
        vav4l2_out_pool_push(ctx, out_buf_idx);
        if (!claimed) vav4l2_cap_pool_push(ctx, cap_buf_idx);
        return -1;
    }
    if (queue_output_flags(ctx, out_buf_idx, bytesused, timestamp, hold ? V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF : 0) < 0) {
        vav4l2_decode_trace(ctx, "queue-output", -1, out_buf_idx);
        fprintf(stderr, "v4l2: QBUF output[%d] failed\n", out_buf_idx);
        if (field_capture >= 0) { vav4l2_decode_reset(ctx); return -1; }
        vav4l2_out_pool_push(ctx, out_buf_idx);
        if (!claimed)
            vav4l2_cap_pool_push(ctx, cap_buf_idx);
        return -1;
    }

    /* Capture is queued bare: the stateless decoder spec forbids CAPTURE
     * buffers in a request (vb2 answers EPERM for them). */
    if (ctx->capture_memory == V4L2_MEMORY_DMABUF) vav4l2_prime_device_written(ctx->current_surface, 0);
    if (field_capture < 0 && (ctx->capture_memory == V4L2_MEMORY_DMABUF ? vav4l2_capture_dma_queue(ctx, cap_buf_idx) :
        vav4l2_queue_capture(ctx->v4l2_fd, cap_buf_idx, ctx->request_fd)) < 0) {
        vav4l2_decode_trace(ctx, "queue-capture", -1, out_buf_idx);
        fprintf(stderr, "v4l2: QBUF capture[%d] failed\n", cap_buf_idx);
        /* OUTPUT is still bound to an unqueued request; DQBUF cannot
         * recover it. STREAMOFF cancels both queues and rebuilds pools. */
        vav4l2_decode_reset(ctx);
        return -1;
    }

    if (vav4l2_submit_request(ctx->request_fd) < 0) {
        vav4l2_decode_trace(ctx, "request-queue", -1, out_buf_idx);
        fprintf(stderr, "v4l2: MEDIA_REQUEST_IOC_QUEUE failed\n");
        /* The buffers stay queued kernel-side: full reset, not just a
         * pool push — a request that failed to queue produces nothing. */
        vav4l2_decode_reset(ctx);
        return -1;
    }

    pfd.fd = hold ? ctx->request_fd : ctx->v4l2_fd;
    pfd.events = hold ? POLLPRI : POLLIN;
    pfd.revents = 0;
    if (vav4l2_poll_intr(&pfd, 1, 3000) <= 0 || (hold && !(pfd.revents & POLLPRI))) {
        vav4l2_decode_trace(ctx, "decode-poll", -1, out_buf_idx);
        fprintf(stderr, "v4l2: decode timeout, resetting queues\n");
        vav4l2_decode_reset(ctx);
        return -1;
    }

    done_cap = cap_buf_idx;
    if (!hold) {
        int cap_err = 0;

        done_cap = ctx->capture_memory == V4L2_MEMORY_DMABUF ? vav4l2_capture_dma_dequeue(ctx, &cap_err) :
            vav4l2_dequeue_buffer(ctx->v4l2_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, &cap_err);
        if (done_cap < 0 || done_cap != cap_buf_idx || done_cap >= ctx->capture_bufs_allocd) {
            vav4l2_decode_trace(ctx, "capture-dequeue", -1, out_buf_idx);
            fprintf(stderr, "v4l2: no matching completed capture buffer\n");
            vav4l2_decode_reset(ctx);
            return -1;
        }
        if (cap_err) {
            vav4l2_decode_trace(ctx, "capture-corrupt", VA_STATUS_ERROR_DECODING_ERROR, done_cap);
            /* Corrupt frame: recycle the slot, recover the bitstream buffer
             * via the shared tail, and report -2 ("skipped") — callers mark
             * the surface instead of failing the vaEndPicture entrypoint
             * (Chrome caches VA-API failures for the whole session). */
            if (!claimed)
                vav4l2_cap_pool_push(ctx, done_cap);
            done_cap = -2;
        }
    }

    done_out = vav4l2_dequeue_buffer(ctx->v4l2_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, &out_err);
    if (done_out < 0) {
        pfd.fd = ctx->v4l2_fd;
        pfd.events = POLLOUT;
        if (vav4l2_poll_intr(&pfd, 1, 200) > 0)
            done_out = vav4l2_dequeue_buffer(ctx->v4l2_fd,
                                             V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
                                             &out_err);
    }
    if (done_out != out_buf_idx) {
        vav4l2_decode_trace(ctx, "output-dequeue", -1, out_buf_idx);
        fprintf(stderr, "v4l2: no matching completed output buffer, resetting queues\n");
        vav4l2_decode_reset(ctx);
        return -1;
    }
    vav4l2_out_pool_push(ctx, done_out);
    if (out_err && done_cap >= 0) {
        vav4l2_decode_trace(ctx, "output-corrupt", VA_STATUS_ERROR_DECODING_ERROR, done_out);
        if (hold) { vav4l2_decode_reset(ctx); return -2; }
        if (!claimed)
            vav4l2_cap_pool_push(ctx, done_cap);
        done_cap = -2;
    }

    /* Recycle the request instead of close+realloc every picture — two
     * syscalls per frame saved. Any reinit failure closes this request;
     * begin_picture allocates a fresh, writable one. */
    if (vav4l2_xioctl(ctx->request_fd, MEDIA_REQUEST_IOC_REINIT, NULL) < 0) {
        close(ctx->request_fd);
        ctx->request_fd = -1;
        if (hold) { vav4l2_decode_reset(ctx); return -1; }
    }

    vav4l2_decode_trace(ctx, "submit-done", done_cap < 0 ? done_cap : 0, done_cap >= 0 ? (unsigned)done_cap : UINT32_MAX);
    return done_cap;
}

int vav4l2_decode_submit(struct vav4l2_context *ctx, int out_buf_idx, uint32_t bytesused, uint64_t timestamp)
{
    return decode_submit(ctx, out_buf_idx, bytesused, timestamp, -1, 0);
}

int vav4l2_decode_submit_field(struct vav4l2_context *ctx, int out_buf_idx,
    uint32_t bytesused, uint64_t timestamp, int capture_index, int hold)
{
    return decode_submit(ctx, out_buf_idx, bytesused, timestamp, capture_index, hold);
}

/*
 * Reconfigure capture (and coded OUTPUT size) when bit-depth, chroma or
 * resolution changes. Safe to call before the first STREAMON.
 */
int vav4l2_ensure_capture(struct vav4l2_context *ctx, int width, int height,
                          uint32_t pixelformat)
{
    int n_cap, want_cap;
    uint32_t fourcc = pixelformat;
    int size_changed, fmt_changed;

    if (!ctx || ctx->v4l2_fd < 0 || width <= 0 || height <= 0 || !vav4l2_drm_fourcc_for_capture(fourcc))
        return -1;

    /* Display size from vaCreateContext is often 1920x1080 while the
     * coded size is 1920x1088. Reconfig only if the current capture
     * buffer cannot hold the new frame, or the fourcc changed. */
    size_changed = (ctx->cap_width == 0) ||
                   (width > (int)ctx->cap_width) ||
                   (height > (int)ctx->cap_height);
    fmt_changed = (ctx->cap_pixelformat != fourcc) ||
                  (ctx->capture_bufs_allocd <= 0);

    if (!size_changed && !fmt_changed)
        return 0;

    fprintf(stderr,
            "v4l2: renegotiate capture %dx%d %.4s -> %dx%d %.4s streamed=%d\n",
            ctx->width, ctx->height, (char *)&ctx->cap_pixelformat,
            width, height, (char *)&fourcc, ctx->streamed);

    if (ctx->streamed) {
        int output = vav4l2_streamoff(ctx->v4l2_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
        int capture = vav4l2_streamoff(ctx->v4l2_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
        if (output < 0 || capture < 0) { close_failed_decoder(ctx); return -1; }
        ctx->streamed = 0;
        vav4l2_av1_dpb_model_reset(ctx);
    }

    vav4l2_release_capture_surfaces(ctx);
    vav4l2_unmap_capture_buffers(ctx);

    if (ctx->capture_bufs_allocd > 0) {
        struct v4l2_requestbuffers req = { 0 };
        req.count = 0;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        req.memory = ctx->capture_memory ? ctx->capture_memory : V4L2_MEMORY_MMAP;
        if (vav4l2_xioctl(ctx->v4l2_fd, VIDIOC_REQBUFS, &req) < 0) {
            fprintf(stderr, "v4l2: REQBUFS(0) capture failed: %s\n",
                    strerror(errno));
            return -1;
        }
        ctx->capture_bufs_allocd = 0;
        ctx->capture_memory = 0;
        ctx->n_free_cap = 0;
    }

    if (size_changed) {
        /* A bigger coded size also means bigger bitstream buffers. vb2
         * refuses S_FMT(OUTPUT) with -EBUSY while buffers are allocated,
         * so the OUTPUT queue has to be rebuilt too (spec: dynamic
         * resolution change re-runs the OUTPUT S_FMT step). An unmapped
         * queue cannot receive a bitstream: fail before any decode QBUF. */
        struct v4l2_format ofmt = { 0 };
        struct v4l2_format cur = { 0 };
        int i, n_out;
        uint32_t codec_fcc = 0;

        ofmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        cur.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        if (ctx->output_bufs_allocd > 0 &&
            vav4l2_xioctl(ctx->v4l2_fd, VIDIOC_G_FMT, &cur) == 0 &&
            (cur.fmt.pix_mp.width < (uint32_t)width ||
             cur.fmt.pix_mp.height < (uint32_t)height)) {
            codec_fcc = cur.fmt.pix_mp.pixelformat;

            for (i = 0; i < ctx->output_bufs_allocd &&
                        i < VAV4L2_NUM_OUTPUT_BUFS; i++) {
                if (ctx->output_buf_ptr[i] && ctx->output_buf_ptr[i] != MAP_FAILED) {
                    munmap(ctx->output_buf_ptr[i], ctx->output_buf_size);
                    ctx->output_buf_ptr[i] = NULL;
                }
            }
            ctx->n_free_out = 0;
            memset(ctx->output_plane_len, 0, sizeof(ctx->output_plane_len));
            if (vav4l2_release_queue(ctx->v4l2_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, V4L2_MEMORY_MMAP) == 0) {
                ctx->output_bufs_allocd = 0;

                ofmt = cur;
                ofmt.fmt.pix_mp.width = width;
                ofmt.fmt.pix_mp.height = height;
                if (vav4l2_codec_format(ctx, &ofmt) < 0) {
                    fprintf(stderr, "v4l2: S_FMT output resize failed: %s\n", strerror(errno));
                    return -1;
                }
                n_out = vav4l2_setup_codec_output(ctx, codec_fcc,
                                                  width, height);
                if (n_out > 0) {
                    int j;

                    ctx->output_bufs_allocd = n_out;
                    if (vav4l2_mmap_output_buffers(ctx->v4l2_fd, n_out,
                                                   ctx->output_buf_ptr,
                                                   &ctx->output_buf_size) < 0) {
                        fprintf(stderr, "v4l2: output re-mmap failed\n");
                        return -1;
                    }
                    for (j = 0; j < n_out && j < VAV4L2_NUM_OUTPUT_BUFS; j++)
                        vav4l2_out_pool_push(ctx, j);
                } else {
                    fprintf(stderr,
                            "v4l2: output queue rebuild failed\n");
                    return -1;
                }
            } else {
                return -1;
            }
        }
    }

    /* Native allocations can return fewer slots than requested. Keep full
     * DPB ownership by trying an independently allocated DMA-BUF pool before
     * accepting a degraded native pool. Never recycle live references. */
    want_cap = ctx->codec == VAV4L2_CODEC_AV1 ?
               ((uint64_t)width * (uint64_t)height >= 3840u * 2160u ?
                VAV4L2_NUM_CAPTURE_BUFS_AV1_4K : VAV4L2_NUM_CAPTURE_BUFS_AV1) :
               VAV4L2_NUM_CAPTURE_BUFS;
    n_cap = vav4l2_capture_dma_setup(ctx, width, height, fourcc, want_cap);
    if (n_cap < 0) return -1;
    if (!n_cap) n_cap = vav4l2_setup_codec_capture(ctx, width, height, fourcc, want_cap);
    if (n_cap <= 0 && want_cap > 24) {
        fprintf(stderr, "v4l2: retrying capture REQBUFS with 24 buffers\n");
        n_cap = vav4l2_setup_codec_capture(ctx, width, height,
                                                 fourcc, 24);
    }
    if (n_cap <= 0 && VAV4L2_NUM_CAPTURE_BUFS > 8) {
        fprintf(stderr, "v4l2: retrying capture REQBUFS with 8 buffers\n");
        n_cap = vav4l2_setup_codec_capture(ctx, width, height,
                                                 fourcc, 8);
    }
    if (n_cap <= 0 && VAV4L2_NUM_CAPTURE_BUFS > 4) {
        fprintf(stderr, "v4l2: retrying capture REQBUFS with 4 buffers\n");
        n_cap = vav4l2_setup_codec_capture(ctx, width, height,
                                                 fourcc, 4);
    }
    const char *pool_mode = getenv("VAV4L2_CAPTURE_POOL_DMABUF");
    int pool_required = pool_mode && !strcmp(pool_mode, "required");
    if (ctx->capture_memory != V4L2_MEMORY_DMABUF && (n_cap < want_cap || pool_required) && vav4l2_capture_pool_allowed(ctx)) {
        vav4l2_decode_trace(ctx, "capture-pool-fallback", n_cap, want_cap);
        if (vav4l2_release_queue(ctx->v4l2_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, V4L2_MEMORY_MMAP) < 0) { close_failed_decoder(ctx); return -1; }
        n_cap = vav4l2_capture_pool_setup(ctx, width, height, fourcc, want_cap);
        if (n_cap < 0) { close_failed_decoder(ctx); return -1; }
        if (!n_cap && pool_required) return -1;
        if (!n_cap) n_cap = vav4l2_setup_codec_capture(ctx, width, height, fourcc, want_cap);
        if (n_cap <= 0) return -1;
    }
    if (n_cap <= 0) return -1;
    ctx->capture_bufs_allocd = n_cap;
    if (vav4l2_debug)
        fprintf(stderr, "v4l2: capture pool n=%d\n", n_cap);
    ctx->n_free_cap = 0;
    {
        int i;
        for (i = 0; i < n_cap && i < VAV4L2_MAX_CAPTURE_BUFS; i++)
            vav4l2_cap_pool_push(ctx, i);
    }
    ctx->width = width;
    ctx->height = height;
    ctx->cap_pixelformat = fourcc;

    if (vav4l2_get_capture_geometry(ctx->v4l2_fd, &ctx->cap_width, &ctx->cap_height,
                                    &ctx->cap_stride, &ctx->cap_sizeimage) < 0 ||
        ctx->cap_width < (uint32_t)width || ctx->cap_height < (uint32_t)height ||
        !ctx->cap_stride || !ctx->cap_sizeimage) {
        fprintf(stderr, "v4l2: invalid negotiated capture for requested %dx%d (got %ux%u)\n",
                width, height, ctx->cap_width, ctx->cap_height);
        ctx->n_free_cap = 0;
        ctx->cap_width = ctx->cap_height = 0;
        return -1;
    }
    fprintf(stderr, "v4l2: capture geometry %ux%u stride=%u size=%u fourcc=%.4s\n",
            ctx->cap_width, ctx->cap_height, ctx->cap_stride, ctx->cap_sizeimage, (char *)&fourcc);
    /* Map capture slots lazily after DQBUF; avoid pinning the full pool. */
    vav4l2_bind_capture_export(ctx);
    vav4l2_decode_trace(ctx, "capture-config", 0, fourcc);
    return 0;
}

/*
 * Queue a compressed bitstream buffer to the output queue.
 * The bitstream was already memcpy'd into the pre-mapped output buffer by
 * the caller — only its length travels with the QBUF.
 */
static int queue_output_flags(struct vav4l2_context *ctx, int buf_index,
                        uint32_t size, uint64_t timestamp, unsigned flags)
{
    struct v4l2_buffer buf = { 0 };
    struct v4l2_plane planes[1] = { 0 };
    int fd = ctx->v4l2_fd;
    int request_fd = ctx->request_fd;

    buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = buf_index;
    buf.length = 1;
    buf.m.planes = planes;
    buf.request_fd = request_fd;
    buf.flags = V4L2_BUF_FLAG_REQUEST_FD | flags;

    /* Timestamp is in nanoseconds (µs-aligned). vb2 stores the timeval
     * internally as ns again, so this must round-trip losslessly to match
     * DPB reference_ts values — which live in the same ns domain. */
    buf.timestamp.tv_sec = timestamp / 1000000000ULL;
    buf.timestamp.tv_usec = (timestamp % 1000000000ULL) / 1000ULL;

    /* hantro AV1 rejects QBUF unless plane length matches the mapped size.
     * Lengths are fixed per REQBUFS cycle — memoize per index. */
    if (buf_index >= 0 && buf_index < VAV4L2_NUM_OUTPUT_BUFS &&
        ctx->output_plane_len[buf_index]) {
        planes[0].length = ctx->output_plane_len[buf_index];
    } else {
        struct v4l2_buffer q = { 0 };
        struct v4l2_plane qp[1] = { 0 };
        q.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        q.memory = V4L2_MEMORY_MMAP;
        q.index = buf_index;
        q.length = 1;
        q.m.planes = qp;
        if (vav4l2_xioctl(fd, VIDIOC_QUERYBUF, &q) == 0) {
            planes[0].length = qp[0].length;
            if (buf_index >= 0 && buf_index < VAV4L2_NUM_OUTPUT_BUFS)
                ctx->output_plane_len[buf_index] = qp[0].length;
        }
    }
    planes[0].bytesused = size;

    if (vav4l2_xioctl(fd, VIDIOC_QBUF, &buf) < 0) {
        fprintf(stderr, "v4l2: QBUF output[%d] failed: %s\n", buf_index, strerror(errno));
        return -1;
    }

    return 0;
}

int vav4l2_queue_output(struct vav4l2_context *ctx, int buf_index, uint32_t size, uint64_t timestamp)
{
    return queue_output_flags(ctx, buf_index, size, timestamp, 0);
}

/*
 * Set V4L2 extended controls bound to a request.
 * Uses V4L2_CTRL_WHICH_REQUEST_VAL so the controls apply to this specific request
 * rather than being global defaults.
 *
 * request_fd: from MEDIA_IOC_REQUEST_ALLOC
 * v4l2_fd:    the video device fd (required by V4L2 to identify the device)
 * ctrls:      caller-allocated v4l2_ext_controls (may contain multiple controls)
 *
 * Returns 0 on success, -1 on error.
 */
int vav4l2_set_request_controls(int request_fd, int v4l2_fd,
                                struct v4l2_ext_controls *ctrls)
{
    ctrls->which = V4L2_CTRL_WHICH_REQUEST_VAL;
    ctrls->request_fd = request_fd;

    if (vav4l2_xioctl(v4l2_fd, VIDIOC_S_EXT_CTRLS, ctrls) < 0) {
        fprintf(stderr, "v4l2: S_EXT_CTRLS (request_fd=%d) failed: %s "
                "(%d controls, first_id=0x%x)\n",
                request_fd, strerror(errno),
                ctrls->count, ctrls->count > 0 ? ctrls->controls[0].id : 0);
        return -1;
    }

    return 0;
}

/* Queue the request; the decoder consumes the controls bound to it.
 * Returns 0 on success, -1 on error. */
int vav4l2_submit_request(int request_fd)
{
    if (vav4l2_xioctl(request_fd, MEDIA_REQUEST_IOC_QUEUE, NULL) < 0) {
        fprintf(stderr, "v4l2: REQUEST_IOC_QUEUE failed: %s\n", strerror(errno));
        return -1;
    }

    return 0;
}
