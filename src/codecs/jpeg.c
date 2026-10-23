/*
 * JPEG encode through discovered V4L2 memory-to-memory encoders.
 *
 * Input is the BeginPicture surface (8-bit 420 CPU image or NV12 capture).
 * Output lands in VAEncCodedBufferType as a VACodedBufferSegment list.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <linux/v4l2-controls.h>
#include <va/va.h>
#include <va/va_enc_jpeg.h>
#include <drm_fourcc.h>

#include "va_request.h"

#define xioctl(fd, req, arg) vareq_xioctl((fd), (req), (arg))

#include "jpeg_dmabuf.h"

static struct vareq_buffer *
buffer_by_id(struct vareq_context *ctx, VABufferID id)
{
    struct vareq_buffer *b = ctx->buffers;
    while (b) {
        if (b->buffer_id == id)
            return b;
        b = b->next;
    }
    return NULL;
}

/* Borrow the latest 8-bit 420 storage; CPU replacement wins over capture
 * metadata. The surface owns persistent mappings and their lifetime. */
static int surface_420(struct vareq_surface *s, struct vareq_image_view *view)
{
    if (!s || s->status == VASurfaceSkipped) return 0;
    *view = (struct vareq_image_view){.width = s->width, .height = s->height};
    if (s->cpu_ptr && (s->last_writer == VAREQ_WRITER_CPU || !s->has_pic)) {
        view->data = s->cpu_ptr; view->fourcc = s->format;
        view->stride = s->cpu_stride; view->alloc_height = s->height; view->size = s->cpu_size;
    } else {
        if (!s->has_pic || s->cap_fourcc != V4L2_PIX_FMT_NV12) return 0;
        view->fourcc = VA_FOURCC_NV12; view->stride = s->stride;
        view->alloc_height = s->aligned_h ? s->aligned_h : s->height;
        view->size = vareq_capture_plane_size(V4L2_PIX_FMT_NV12, view->stride, view->alloc_height);
        if (!view->size) return 0;
        if (vareq_expbuf_export_wanted() && s->last_writer != VAREQ_WRITER_BO && s->cap_view && s->buf_index >= 0)
            view->data = s->cap_view;
        else {
            if (vareq_surface_ensure_memfd(s) < 0 || s->memfd_fd < 0) return 0;
            view->data = vareq_surface_map_memfd(s, view->size);
        }
    }
    int planar = view->fourcc == VA_FOURCC_I420 || view->fourcc == VA_FOURCC_YV12;
    if (!planar && view->fourcc != VA_FOURCC_NV12 && view->fourcc != VA_FOURCC_NV21) return 0;
    uint64_t row = (uint64_t)view->width + (!planar && (view->width & 1));
    uint32_t need = vareq_va_image_size(view->fourcc, view->stride, view->alloc_height);
    return view->data && view->width && view->height && view->alloc_height >= view->height &&
           view->stride >= row && need && need <= view->size;
}

/* Hantro selects its raw defaults when coded CAPTURE is configured. */
static int set_output_420(struct vareq_context *ctx, struct v4l2_format *format, unsigned width, unsigned height, int planar)
{
    const uint32_t candidates[] = {V4L2_PIX_FMT_YUV420M, V4L2_PIX_FMT_NV12M, V4L2_PIX_FMT_NV12};
    for (unsigned i = planar ? 0 : 1; i < 3; i++) {
        memset(format, 0, sizeof(*format));
        format->type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        format->fmt.pix_mp.width = width; format->fmt.pix_mp.height = height;
        format->fmt.pix_mp.pixelformat = candidates[i]; format->fmt.pix_mp.num_planes = 3 - i;
        format->fmt.pix_mp.field = V4L2_FIELD_NONE;
        if (vareq_codec_format(ctx, format) == 0 && format->fmt.pix_mp.pixelformat == candidates[i]) return 0;
    }
    return -1;
}

VAStatus vareq_jpeg_encode(struct vareq_context *ctx,
                            struct vareq_buffer **buffers, int num_buffers)
{
    VAEncPictureParameterBufferJPEG *pic = NULL;
    int i, quality = 50;
    int fd = ctx->v4l2_fd;
    struct vareq_surface *src = ctx->current_surface;
    struct vareq_buffer *coded = NULL;
    VACodedBufferSegment *seg;
    struct vareq_image_view source;
    int w, h;
    struct v4l2_format ofmt, cfmt;
    struct v4l2_requestbuffers oreq, creq;
    struct v4l2_buffer obuf, cbuf;
    struct v4l2_plane oplanes[8], cplanes[8];
    struct v4l2_control ctrl;
    struct pollfd pfd;
    int jpeg_size = 0;
    VAStatus st;
    VADRMPRIMESurfaceDescriptor dma = {0}; unsigned dma_raw = 0;
    const char *dma_mode = getenv("VAREQ_JPEG_DMABUF");
    int dma_disabled = dma_mode && !strcmp(dma_mode, "0");
    int dma_required = dma_mode && !strcmp(dma_mode, "required"), direct = 0;

    for (i = 0; i < num_buffers; i++) {
        if (!buffers[i] || !buffers[i]->data)
            continue;
        if (buffers[i]->type == VAEncPictureParameterBufferType)
            pic = buffers[i]->data;
        else if (buffers[i]->type == VAEncMiscParameterBufferType) {
            VAEncMiscParameterBuffer *m = buffers[i]->data;
            if (m->type == VAEncMiscParameterTypeQualityLevel) {
                const VAEncMiscParameterBufferQualityLevel *q = (const void *)m->data;
                /* Encode preset is not JPEG quantization quality. FFmpeg
                 * also sends UINT32_MAX for its unspecified preset. */
                if (q->quality_level > 1 && q->quality_level != UINT32_MAX)
                    return VA_STATUS_ERROR_INVALID_PARAMETER;
            }
        }
    }
    if (!pic || !src) {
        fprintf(stderr, "v4l2request: JPEG missing pic=%p src=%p nbuf=%d\n",
                (void *)pic, (void *)src, num_buffers);
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    if (pic->quality)
        quality = pic->quality;
    if (quality < 5)
        quality = 5;
    if (quality > 100)
        quality = 100;

    coded = buffer_by_id(ctx, pic->coded_buf);
    if (!coded || !coded->data || coded->type != VAEncCodedBufferType || coded->num_elements != 1) {
        struct vareq_buffer *b;
        fprintf(stderr, "v4l2request: JPEG coded_buf %u not found (ctx buffers:",
                pic->coded_buf);
        for (b = ctx->buffers; b; b = b->next)
            fprintf(stderr, " %u/t%u", b->buffer_id, b->type);
        fprintf(stderr, ")\n");
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    seg = coded->data;
    seg->buf = seg + 1;
    seg->size = seg->status = seg->bit_offset = 0;
    seg->next = NULL;

    w = pic->picture_width ? pic->picture_width : src->width;
    h = pic->picture_height ? pic->picture_height : src->height;

retry_transport:
    direct = !dma_disabled && (unsigned)w == src->width && (unsigned)h == src->height ? jpeg_prime(src, &dma, &dma_raw) : 0;
    if (direct < 0 || (!direct && dma_required)) { st = VA_STATUS_ERROR_OPERATION_FAILED; goto fail; }
    int source_ok = direct;
    if (direct) source = (struct vareq_image_view){.fourcc = src->format};
    else {
        if (vareq_import_refresh(src) < 0) { st = VA_STATUS_ERROR_OPERATION_FAILED; goto fail; }
        source_ok = surface_420(src, &source);
    }
    if (!source_ok || w <= 0 || h <= 0 || (unsigned)w > src->width || (unsigned)h > src->height ||
        (w & 1) || (h & 1)) {
        fprintf(stderr, "v4l2request: JPEG encode: no valid 8-bit 420 source\n");
        jpeg_close_prime(&dma); return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    int planar_source = source.fourcc == VA_FOURCC_I420 || source.fourcc == VA_FOURCC_YV12;

    {
        struct vareq_m2m_state *q = &ctx->jpeg_q;
        uint32_t key[8] = { (uint32_t)w, (uint32_t)h, (uint32_t)quality,
                            (uint32_t)planar_source, direct ? dma_raw : 0,
                            direct ? dma.layers[0].pitch[0] : 0, direct ? dma.layers[0].pitch[1] : 0, direct ? dma.layers[0].pitch[2] : 0 };
        int renegot = !q->valid || memcmp(q->key, key, sizeof(key));
        uint32_t pitch[3] = {0}, plane_used[3] = {0};
        uint64_t need[3] = {0};
        int row;

        if (renegot) {
            vareq_m2m_teardown(ctx, q);
            if (ctx->v4l2_fd < 0) { st = VA_STATUS_ERROR_OPERATION_FAILED; goto fail; }
            q->valid = 1; /* teardown also owns partial setup on failure */

            /* The coded CAPTURE format selects the encoder mode. Hantro
             * resets raw OUTPUT to its default here, so choose raw input only
             * after CAPTURE S_FMT has completed. */
            memset(&cfmt, 0, sizeof(cfmt));
            cfmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            cfmt.fmt.pix_mp.width = w;
            cfmt.fmt.pix_mp.height = h;
            cfmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_JPEG;
            cfmt.fmt.pix_mp.num_planes = 1;
            if (vareq_codec_format(ctx, &cfmt) < 0) {
                fprintf(stderr, "v4l2request: JPEG S_FMT capture: %s\n",
                        strerror(errno));
                st = VA_STATUS_ERROR_OPERATION_FAILED;
                goto fail;
            }

            if ((direct ? jpeg_dma_format(ctx, &ofmt, &dma, dma_raw) : set_output_420(ctx, &ofmt, w, h, planar_source)) < 0) {
                if (direct && (errno == EINVAL || errno == ENOTTY)) goto fallback_dma;
                fprintf(stderr, "v4l2request: JPEG S_FMT output: %s\n", strerror(errno));
                st = VA_STATUS_ERROR_OPERATION_FAILED;
                goto fail;
            }
            ctrl.id = V4L2_CID_JPEG_COMPRESSION_QUALITY;
            ctrl.value = quality;
            if (vareq_codec_control(ctx, &ctrl) < 0) {
                fprintf(stderr, "v4l2request: JPEG quality=%d failed: %s\n", quality, strerror(errno));
                st = VA_STATUS_ERROR_OPERATION_FAILED;
                goto fail;
            }
            /* Validate the final raw layout before allocating queues. */
            if (xioctl(fd, VIDIOC_G_FMT, &ofmt) < 0) {
                fprintf(stderr, "v4l2request: JPEG G_FMT output failed: %s\n", strerror(errno));
                st = VA_STATUS_ERROR_OPERATION_FAILED;
                goto fail;
            }
            q->ofmt = ofmt;
            if (direct && !jpeg_dma_spans(&ofmt, &dma, dma_raw)) goto fallback_dma;

            if ((ofmt.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_NV12 &&
                 ofmt.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_NV12M &&
                 ofmt.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_YUV420M) ||
                ofmt.fmt.pix_mp.num_planes !=
                    (ofmt.fmt.pix_mp.pixelformat == V4L2_PIX_FMT_YUV420M ? 3 :
                     ofmt.fmt.pix_mp.pixelformat == V4L2_PIX_FMT_NV12M ? 2 : 1) ||
                ofmt.fmt.pix_mp.width < (uint32_t)w || ofmt.fmt.pix_mp.height < (uint32_t)h ||
                cfmt.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_JPEG || cfmt.fmt.pix_mp.num_planes != 1 ||
                cfmt.fmt.pix_mp.width != (uint32_t)w || cfmt.fmt.pix_mp.height != (uint32_t)h) {
                fprintf(stderr, "v4l2request: JPEG unsupported negotiated formats: OUTPUT %.4s %ux%u planes=%u, CAPTURE %.4s %ux%u planes=%u\n",
                        (char *)&ofmt.fmt.pix_mp.pixelformat, ofmt.fmt.pix_mp.width, ofmt.fmt.pix_mp.height,
                        ofmt.fmt.pix_mp.num_planes, (char *)&cfmt.fmt.pix_mp.pixelformat, cfmt.fmt.pix_mp.width, cfmt.fmt.pix_mp.height, cfmt.fmt.pix_mp.num_planes);
                st = VA_STATUS_ERROR_OPERATION_FAILED;
                goto fail;
            }

            memset(&oreq, 0, sizeof(oreq));
            oreq.count = 2;
            oreq.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
            oreq.memory = direct ? V4L2_MEMORY_DMABUF : V4L2_MEMORY_MMAP;
            q->out_memory = oreq.memory; q->cap_memory = V4L2_MEMORY_MMAP;
            if (xioctl(fd, VIDIOC_REQBUFS, &oreq) < 0) {
                if (direct && (errno == EINVAL || errno == ENOTTY)) goto fallback_dma;
                fprintf(stderr, "v4l2request: JPEG REQBUFS out: %s\n",
                        strerror(errno));
                st = VA_STATUS_ERROR_OPERATION_FAILED;
                goto fail;
            }
            if (!oreq.count) { st = VA_STATUS_ERROR_ALLOCATION_FAILED; goto fail; }
            memset(&creq, 0, sizeof(creq));
            creq.count = 2;
            creq.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            creq.memory = V4L2_MEMORY_MMAP;
            if (xioctl(fd, VIDIOC_REQBUFS, &creq) < 0) {
                fprintf(stderr, "v4l2request: JPEG REQBUFS cap: %s\n",
                        strerror(errno));
                st = VA_STATUS_ERROR_OPERATION_FAILED;
                goto fail;
            }
            if (!creq.count) { st = VA_STATUS_ERROR_ALLOCATION_FAILED; goto fail; }

            if (direct) {
                q->out_planes = ofmt.fmt.pix_mp.num_planes;
            } else {
            memset(&obuf, 0, sizeof(obuf));
            memset(oplanes, 0, sizeof(oplanes));
            obuf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
            obuf.memory = V4L2_MEMORY_MMAP;
            obuf.index = 0;
            obuf.length = 8;
            obuf.m.planes = oplanes;
            if (xioctl(fd, VIDIOC_QUERYBUF, &obuf) < 0) {
                fprintf(stderr, "v4l2request: JPEG QUERYBUF out: %s nplanes=%u\n",
                        strerror(errno), ofmt.fmt.pix_mp.num_planes);
                st = VA_STATUS_ERROR_OPERATION_FAILED;
                goto fail;
            }
            /* The kernel overwrites buf.length with the queue's real plane
             * count on QUERYBUF — that is the value QBUF will enforce. */
            if (obuf.length != ofmt.fmt.pix_mp.num_planes) {
                fprintf(stderr, "v4l2request: JPEG QUERYBUF plane mismatch: got %u, expected %u\n",
                        obuf.length, ofmt.fmt.pix_mp.num_planes);
                st = VA_STATUS_ERROR_OPERATION_FAILED;
                goto fail;
            }
            q->out_planes = obuf.length;
            for (int plane = 0; plane < q->out_planes; plane++) {
                q->out_len[plane] = oplanes[plane].length;
                q->out_map[plane] = mmap(NULL, oplanes[plane].length, PROT_READ | PROT_WRITE,
                                         MAP_SHARED, fd, oplanes[plane].m.mem_offset);
                if (q->out_map[plane] == MAP_FAILED) {
                    st = VA_STATUS_ERROR_OPERATION_FAILED;
                    goto fail;
                }
            }

            }
            memset(&cbuf, 0, sizeof(cbuf));
            memset(cplanes, 0, sizeof(cplanes));
            cbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            cbuf.memory = V4L2_MEMORY_MMAP;
            cbuf.index = 0;
            cbuf.length = 1;
            cbuf.m.planes = cplanes;
            if (xioctl(fd, VIDIOC_QUERYBUF, &cbuf) < 0) {
                st = VA_STATUS_ERROR_OPERATION_FAILED;
                goto fail;
            }
            q->cap_len = cplanes[0].length;
            q->cap_map = mmap(NULL, cplanes[0].length, PROT_READ,
                              MAP_SHARED, fd, cplanes[0].m.mem_offset);
            if (q->cap_map == MAP_FAILED) {
                st = VA_STATUS_ERROR_OPERATION_FAILED;
                goto fail;
            }
            /* out_planes holds the QUERYBUF writeback (authoritative);
             * fall back to the G_FMT value only if that was unavailable. */
            if (q->out_planes <= 0)
                q->out_planes = ofmt.fmt.pix_mp.num_planes;
            memcpy(q->key, key, sizeof(key));
            q->valid = 1;
        }

        ofmt = q->ofmt;
        unsigned raw = ofmt.fmt.pix_mp.pixelformat;
        int planar_output = raw == V4L2_PIX_FMT_YUV420M;
        int planes = planar_output ? 3 : raw == V4L2_PIX_FMT_NV12M ? 2 : raw == V4L2_PIX_FMT_NV12 ? 1 : 0;
        if (!planes || q->out_planes != planes) { st = VA_STATUS_ERROR_OPERATION_FAILED; goto fail; }
        uint32_t out_h = ofmt.fmt.pix_mp.height;
        if (direct) {
            if (!jpeg_dma_spans(&ofmt, &dma, dma_raw)) goto fallback_dma;
            for (unsigned o = 0; o < dma.num_objects; o++) if (vareq_dmabuf_wait(dma.objects[o].fd, 0) < 0) { st = VA_STATUS_ERROR_OPERATION_FAILED; goto fail; }
            for (int p = 0; p < planes; p++) {
                plane_used[p] = ofmt.fmt.pix_mp.plane_fmt[p].sizeimage;
                q->out_len[p] = dma.objects[dma.layers[0].object_index[jpeg_prime_plane(&dma, p)]].size;
            }
        } else {
        for (int plane = 0; plane < planes; plane++) {
            uint32_t row_bytes = plane && planar_output ? (uint32_t)w / 2 : (uint32_t)w;
            pitch[plane] = ofmt.fmt.pix_mp.plane_fmt[plane].bytesperline;
            if (!pitch[plane]) pitch[plane] = row_bytes;
            need[plane] = (uint64_t)pitch[plane] * (plane ? ((uint64_t)out_h + 1) / 2 : out_h);
            if (planes == 1) need[plane] += (uint64_t)pitch[plane] * (((uint64_t)out_h + 1) / 2);
            uint64_t bytes = ofmt.fmt.pix_mp.plane_fmt[plane].sizeimage;
            if (!bytes) bytes = q->out_len[plane];
            if (ofmt.fmt.pix_mp.width < (uint32_t)w || out_h < (uint32_t)h || pitch[plane] < row_bytes ||
                bytes < need[plane] || bytes > q->out_len[plane] || bytes > UINT32_MAX ||
                !q->out_map[plane] || q->out_map[plane] == MAP_FAILED) {
                fprintf(stderr, "v4l2request: JPEG OUTPUT plane %d has invalid geometry or backing\n", plane);
                st = VA_STATUS_ERROR_OPERATION_FAILED;
                goto fail;
            }
            plane_used[plane] = (uint32_t)bytes;
        }
        uint8_t *du = planes >= 2 ? q->out_map[1] : (uint8_t *)q->out_map[0] + (size_t)pitch[0] * out_h;
        uint8_t *dv = planar_output ? q->out_map[2] : du + 1;
        unsigned ds = planar_output ? 1 : 2, us = planes >= 2 ? pitch[1] : pitch[0], vs = planar_output ? pitch[2] : us;
        const uint8_t *su = (const uint8_t *)source.data + (size_t)source.stride * source.alloc_height;
        uint32_t cs = planar_source ? (source.stride + 1) / 2 : source.stride;
        const uint8_t *sv = planar_source ? su + (size_t)cs * ((source.alloc_height + 1) / 2) : su + 1;
        unsigned ss = planar_source ? 1 : 2;
        if (source.fourcc == VA_FOURCC_YV12 || source.fourcc == VA_FOURCC_NV21) {
            const uint8_t *swap = su; su = sv; sv = swap;
        }
        if (source.fourcc != VA_FOURCC_NV12) {
            for (int plane = 0; plane < planes; plane++) memset(q->out_map[plane], plane ? 128 : 0, plane_used[plane]);
            if (planes == 1) memset(du, 128, plane_used[0] - (size_t)pitch[0] * out_h);
        }
        for (row = 0; row < h; row++)
            memcpy((uint8_t *)q->out_map[0] + (size_t)row * pitch[0],
                   (const uint8_t *)source.data + (size_t)row * source.stride, (size_t)w);
        for (row = 0; row < h / 2; row++) for (int x = 0; x < w / 2; x++) {
            du[(size_t)row * us + x * ds] = su[(size_t)row * cs + x * ss];
            dv[(size_t)row * vs + x * ds] = sv[(size_t)row * cs + x * ss];
        }

        }
        memset(&cbuf, 0, sizeof(cbuf));
        memset(cplanes, 0, sizeof(cplanes));
        cbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        cbuf.memory = V4L2_MEMORY_MMAP;
        cbuf.index = 0;
        cbuf.length = 1;
        cbuf.m.planes = cplanes;
        if (xioctl(fd, VIDIOC_QBUF, &cbuf) < 0) {
            fprintf(stderr, "v4l2request: JPEG QBUF cap: %s\n", strerror(errno));
            st = VA_STATUS_ERROR_OPERATION_FAILED;
            goto fail;
        }
        memset(&obuf, 0, sizeof(obuf));
        memset(oplanes, 0, sizeof(oplanes));
        obuf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        obuf.memory = q->out_memory ? q->out_memory : V4L2_MEMORY_MMAP;
        obuf.index = 0;
        obuf.length = q->out_planes;
        obuf.m.planes = oplanes;
        for (int plane = 0; plane < q->out_planes; plane++) {
            oplanes[plane].bytesused = plane_used[plane];
            oplanes[plane].length = q->out_len[plane];
            if (direct) oplanes[plane].m.fd = dma.objects[dma.layers[0].object_index[jpeg_prime_plane(&dma, plane)]].fd;
        }
        if (xioctl(fd, VIDIOC_QBUF, &obuf) < 0) {
            fprintf(stderr, "v4l2request: JPEG QBUF out: %s (planes=%d yused=%u)\n",
                    strerror(errno), q->out_planes, oplanes[0].bytesused);
            st = VA_STATUS_ERROR_OPERATION_FAILED;
            goto fail;
        }
        {
            enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;

            if (xioctl(fd, VIDIOC_STREAMON, &t) < 0) {
                fprintf(stderr, "v4l2request: JPEG STREAMON out: %s\n",
                        strerror(errno));
                st = VA_STATUS_ERROR_OPERATION_FAILED;
                goto fail;
            }
            q->streaming |= 1;
            t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            if (xioctl(fd, VIDIOC_STREAMON, &t) < 0) {
                fprintf(stderr, "v4l2request: JPEG STREAMON cap: %s\n",
                        strerror(errno));
                st = VA_STATUS_ERROR_OPERATION_FAILED;
                goto fail;
            }
            q->streaming |= 2;
        }

        pfd.fd = fd;
        pfd.events = POLLIN;
        if (vareq_poll_intr(&pfd, 1, 2000) <= 0 || !(pfd.revents & POLLIN) ||
            (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
            fprintf(stderr, "v4l2request: JPEG encode timeout\n");
            st = VA_STATUS_ERROR_OPERATION_FAILED;
            goto fail;
        }

        memset(&cbuf, 0, sizeof(cbuf));
        memset(cplanes, 0, sizeof(cplanes));
        cbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        cbuf.memory = V4L2_MEMORY_MMAP;
        cbuf.length = 1;
        cbuf.m.planes = cplanes;
        if (xioctl(fd, VIDIOC_DQBUF, &cbuf) < 0) {
            fprintf(stderr, "v4l2request: JPEG DQBUF: %s\n", strerror(errno));
            st = VA_STATUS_ERROR_OPERATION_FAILED;
            goto fail;
        }
        if (cbuf.index != 0 || cbuf.length != 1 || (cbuf.flags & V4L2_BUF_FLAG_ERROR) || cplanes[0].data_offset > cplanes[0].bytesused ||
            cplanes[0].bytesused > q->cap_len || cplanes[0].bytesused == cplanes[0].data_offset) {
            seg->status = VA_CODED_BUF_STATUS_BAD_BITSTREAM;
            st = VA_STATUS_ERROR_ENCODING_ERROR;
            goto fail;
        }
        if (vareq_m2m_finish_output(ctx, q) < 0) {
            seg->status = VA_CODED_BUF_STATUS_BAD_BITSTREAM;
            st = VA_STATUS_ERROR_ENCODING_ERROR;
            goto fail;
        }
        uint32_t used = cplanes[0].bytesused - cplanes[0].data_offset;
        if (used > coded->size) {
            seg->status = VA_CODED_BUF_STATUS_FRAME_SIZE_OVERFLOW;
            st = VA_STATUS_ERROR_NOT_ENOUGH_BUFFER;
            goto fail;
        }
        jpeg_size = (int)used;
        memcpy(seg->buf, (uint8_t *)q->cap_map + cplanes[0].data_offset, used);
        seg->size = used;
    }

    st = VA_STATUS_SUCCESS;
    /* Keep the queue: per-frame STREAMOFF returns the buffers while the
     * allocation and mappings stay valid. */
    {
        enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;

        int output_stopped = xioctl(fd, VIDIOC_STREAMOFF, &t);
        if (output_stopped == 0) ctx->jpeg_q.streaming &= ~1u;
        t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        int capture_stopped = xioctl(fd, VIDIOC_STREAMOFF, &t);
        if (capture_stopped == 0) ctx->jpeg_q.streaming &= ~2u;
        if (output_stopped < 0 || capture_stopped < 0) {
            seg->status = VA_CODED_BUF_STATUS_BAD_BITSTREAM;
            st = VA_STATUS_ERROR_ENCODING_ERROR;
            goto fail;
        }
    }
    if (vareq_debug)
        fprintf(stderr, "v4l2request: JPEG encoded %dx%d quality=%d bytes=%d planes=%d\n",
                w, h, quality, jpeg_size, ctx->jpeg_q.out_planes);
    jpeg_trace(ctx, "complete"); jpeg_close_prime(&dma);
    return st;

fallback_dma:
    /* No job has been submitted. Release the partially configured queues
     * before retrying the existing CPU staging path. */
    vareq_m2m_teardown(ctx, &ctx->jpeg_q); jpeg_close_prime(&dma);
    if (dma_required || ctx->v4l2_fd < 0) { st = VA_STATUS_ERROR_OPERATION_FAILED; goto fail; }
    dma_disabled = 1; goto retry_transport;
fail:
    seg->size = 0;
    jpeg_trace(ctx, "failed");
    vareq_m2m_teardown(ctx, &ctx->jpeg_q); jpeg_close_prime(&dma);
    return st;
}

const int vareq_has_jpeg = 1;
