/* Independent Y/U/V oracle across native and fallback JPEG OUTPUT layouts. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <va/va.h>
#include <va/va_enc_jpeg.h>
#include "va_v4l2.h"

static int failures, fallback, fault, calls, setups, submitted;
static struct v4l2_format raw;
static void expect(int ok, const char *tag)
{
    if (!ok) { fprintf(stderr, "FAIL %s fallback=%d fault=%d\n", tag, fallback, fault); failures++; }
}
static unsigned ysample(unsigned frame, unsigned x, unsigned y) { return 17 + frame * 3 + x * 7 + y * 11; }
static unsigned usample(unsigned frame, unsigned x, unsigned y) { return 51 + frame * 5 + x + y; }
static unsigned vsample(unsigned frame, unsigned x, unsigned y) { return 173 + frame * 7 + x + y; }
static int jpeg_ioctl(int fd, unsigned long request, void *arg)
{
    (void)fd; calls++;
    if (request == VIDIOC_QUERY_EXT_CTRL || request == VIDIOC_S_EXT_CTRLS)
        expect(0, "native420-does-not-invent-extra-kernel-controls");
    if (request == VIDIOC_S_CTRL) {
        struct v4l2_control *c = arg;
        expect(c->id == V4L2_CID_JPEG_COMPRESSION_QUALITY && c->value == 50, "native-quality-policy-preserved");
    }
    if (request == VIDIOC_S_FMT) {
        struct v4l2_format *f = arg;
        if (f->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) { setups++; return 0; }
        if ((fallback && f->fmt.pix_mp.pixelformat == V4L2_PIX_FMT_YUV420M) ||
            (fallback == 2 && f->fmt.pix_mp.pixelformat == V4L2_PIX_FMT_NV12M)) { errno = EINVAL; return -1; }
        raw = *f; raw.fmt.pix_mp.height = 6;
        unsigned n = raw.fmt.pix_mp.num_planes;
        raw.fmt.pix_mp.plane_fmt[0].bytesperline = 8; raw.fmt.pix_mp.plane_fmt[0].sizeimage = n == 1 ? 72 : 48;
        raw.fmt.pix_mp.plane_fmt[1].bytesperline = n == 3 ? 5 : 8;
        raw.fmt.pix_mp.plane_fmt[1].sizeimage = n == 3 ? 15 : 24;
        raw.fmt.pix_mp.plane_fmt[2].bytesperline = fault == 2 ? 1 : 7;
        raw.fmt.pix_mp.plane_fmt[2].sizeimage = fault == 3 ? 22 : fault == 4 ? 20 : 21;
        if (fault == 5) raw.fmt.pix_mp.num_planes = 2;
        *f = raw; return 0;
    }
    if (request == VIDIOC_G_FMT) { *(struct v4l2_format *)arg = raw; return 0; }
    if (request == VIDIOC_QUERYBUF) {
        struct v4l2_buffer *b = arg;
        if (b->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
            b->length = raw.fmt.pix_mp.num_planes;
            for (unsigned p = 0; p < b->length; p++) {
                b->m.planes[p].m.mem_offset = p * 4096;
                b->m.planes[p].length = p == 0 ? (b->length == 1 ? 72 : 48) : p == 1 ? (b->length == 3 ? 15 : 24) : fault == 1 ? 20 : 21;
            }
        } else { b->length = 1; b->m.planes[0].length = 64; b->m.planes[0].m.mem_offset = 12288; }
    }
    if (request == VIDIOC_QBUF) {
        struct v4l2_buffer *b = arg;
        if (b->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
            submitted++;
            expect(b->length == raw.fmt.pix_mp.num_planes, "qbuf-has-negotiated-memory-plane-count");
            for (unsigned p = 0; p < b->length; p++)
                expect(b->m.planes[p].bytesused <= b->m.planes[p].length, "qbuf-bytesused-fits-every-memory-plane");
        }
    }
    if (request == VIDIOC_DQBUF) {
        struct v4l2_buffer *b = arg;
        if (b->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) { b->m.planes[0].bytesused = 48; b->m.planes[0].data_offset = 4; }
    }
    return 0;
}
static void source_pixels(unsigned char data[48], unsigned fourcc, unsigned frame)
{
    memset(data, 0xa5, 48);
    for (unsigned y = 0; y < 4; y++) for (unsigned x = 0; x < 4; x++) data[y * 8 + x] = ysample(frame, x, y);
    for (unsigned y = 0; y < 2; y++) for (unsigned x = 0; x < 2; x++) {
        if (fourcc == VA_FOURCC_I420 || fourcc == VA_FOURCC_YV12) {
            data[32 + y * 4 + x] = fourcc == VA_FOURCC_YV12 ? vsample(frame, x, y) : usample(frame, x, y);
            data[40 + y * 4 + x] = fourcc == VA_FOURCC_YV12 ? usample(frame, x, y) : vsample(frame, x, y);
        } else {
            data[32 + y * 8 + x * 2] = fourcc == VA_FOURCC_NV21 ? vsample(frame, x, y) : usample(frame, x, y);
            data[33 + y * 8 + x * 2] = fourcc == VA_FOURCC_NV21 ? usample(frame, x, y) : vsample(frame, x, y);
        }
    }
}
static void run_case(int route, int broken)
{
    fallback = route; fault = broken; calls = setups = submitted = 0;
    unsigned char pixels[48], saved[48], payload[64]; memset(payload, 0x9b, sizeof(payload));
    struct vav4l2_surface src = {.width = 4, .height = 4, .cpu_ptr = pixels, .cpu_stride = 8,
        .cpu_size = sizeof(pixels), .last_writer = VAV4L2_WRITER_CPU, .memfd_fd = -1};
    struct vav4l2_context ctx = {.codec = VAV4L2_CODEC_JPEG_ENC, .current_surface = &src};
    ctx.v4l2_fd = memfd_create("jpeg420-queues", MFD_CLOEXEC);
    if (ctx.v4l2_fd < 0 || ftruncate(ctx.v4l2_fd, 16384) || pwrite(ctx.v4l2_fd, payload, 64, 12288) != 64) exit(1);
    struct vav4l2_buffer coded = {.buffer_id = 1, .type = VAEncCodedBufferType, .size = 64, .num_elements = 1};
    coded.data = calloc(1, sizeof(VACodedBufferSegment) + 64); if (!coded.data) exit(1); ctx.buffers = &coded;
    VAEncPictureParameterBufferJPEG pic = {.coded_buf = 1, .picture_width = 4, .picture_height = 4, .quality = 50};
    struct vav4l2_buffer params = {.type = VAEncPictureParameterBufferType, .data = &pic, .size = sizeof(pic)};
    struct vav4l2_buffer *buffers[] = {&params};
    const unsigned formats[] = {VA_FOURCC_I420, VA_FOURCC_YV12, VA_FOURCC_NV21, VA_FOURCC_NV12};
    vav4l2_set_ioctl_hook(jpeg_ioctl);
    for (unsigned frame = 0; frame < (broken ? 1u : 4u); frame++) {
        src.format = formats[frame]; source_pixels(pixels, src.format, frame); memcpy(saved, pixels, sizeof(saved));
        if (broken == 6) src.cpu_size--;
        if (broken == 7) { src.format = VA_FOURCC_BGRA; src.has_pic = 1; src.cap_fourcc = V4L2_PIX_FMT_NV12; src.cap_view = payload; src.buf_index = 0; src.stride = src.aligned_h = 4; }
        if (broken == 8) src.format = VA_FOURCC_P010;
        VAStatus result = vav4l2_jpeg_encode(&ctx, buffers, 1);
        if (broken) {
            expect(result == (broken >= 6 ? VA_STATUS_ERROR_INVALID_SURFACE : VA_STATUS_ERROR_OPERATION_FAILED), "bad-420-source-or-negotiated-plane-is-rejected");
            expect(!submitted, "invalid-layout-never-submits-output");
            if (broken >= 6) expect(!calls, "invalid-latest-source-is-rejected-before-ioctl");
            else expect(!ctx.jpeg_q.valid && !ctx.jpeg_q.out_map[2], "failed-three-plane-setup-releases-every-mapping");
            break;
        }
        expect(result == VA_STATUS_SUCCESS, "jpeg420-native-or-fallback-encode");
        if (result) break;
        struct vav4l2_m2m_state *q = &ctx.jpeg_q;
        unsigned n = route == 2 ? 1 : !route && frame < 2 ? 3 : 2;
        expect((unsigned)q->out_planes == n && !memcmp(pixels, saved, sizeof(saved)), "native-plane-count-and-source-ownership");
        uint8_t *yplane = q->out_map[0], *u = n > 1 ? q->out_map[1] : yplane + 48;
        uint8_t *v = n == 3 ? q->out_map[2] : u + 1;
        unsigned step = n == 3 ? 1 : 2, upitch = n == 3 ? 5 : 8, vpitch = n == 3 ? 7 : 8;
        for (unsigned y = 0; y < 4; y++) for (unsigned x = 0; x < 4; x++)
            expect(yplane[y * 8 + x] == ysample(frame, x, y), "source-luma-and-output-padding-strides-are-independent");
        for (unsigned y = 0; y < 2; y++) for (unsigned x = 0; x < 2; x++)
            expect(u[y * upitch + x * step] == usample(frame, x, y) && v[y * vpitch + x * step] == vsample(frame, x, y),
                   "source-order-and-three-independent-plane-strides-preserve-distinct-u-v");
        VACodedBufferSegment *segment = coded.data;
        expect(segment->size == 44 && !segment->status && ((uint8_t *)segment->buf)[43] == 0x9b, "coded-buffer-completion-contract-is-preserved");
    }
    if (!broken) expect(setups == 2 && submitted == 4, "warm-family-cache-reuses-planes-and-refreshes-current-source-pixels");
    vav4l2_m2m_teardown(&ctx, &ctx.jpeg_q); vav4l2_set_ioctl_hook(NULL);
    free(coded.data); if (ctx.v4l2_fd >= 0) close(ctx.v4l2_fd);
}
int main(void)
{
    for (int route = 0; route < 3; route++) run_case(route, 0);
    for (int broken = 1; broken <= 8; broken++) run_case(0, broken);
    return failures != 0;
}
