/* JPEG completion bounds and VPP plane geometry on mocked M2M queues. */
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

static int failures, mode, releases, stops;
static unsigned out_bytes;
static struct v4l2_format jpeg_raw;
static int jpeg_capture_set, jpeg_output_after_capture, jpeg_output_submitted, jpeg_quality_value;
static void expect(int ok, const char *tag)
{
    if (!ok) { fprintf(stderr, "FAIL %s mode=%d\n", tag, mode); failures++; }
}
static void *mapping(size_t size)
{
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { perror("test mmap"); exit(1); }
    return p;
}
static int m2m_ioctl(int fd, unsigned long request, void *arg)
{
    (void)fd;
    if (mode == 18 && (request == VIDIOC_QUERY_EXT_CTRL || request == VIDIOC_S_EXT_CTRLS))
        expect(0, "jpeg-legacy-client-never-needs-an-extra-kernel-control");
    if (request == VIDIOC_S_CTRL) {
        struct v4l2_control *c = arg;
        if (c->id == V4L2_CID_JPEG_COMPRESSION_QUALITY) jpeg_quality_value = c->value;
    }
    if (request == VIDIOC_STREAMOFF) {
        stops++;
        unsigned bit = *(enum v4l2_buf_type *)arg == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE ? 1 : 2;
        if (mode >= 14 && mode <= 16 && ((unsigned)(mode - 13) & bit)) { errno = EIO; return -1; }
    }
    if ((mode == 6 || mode == 13 || mode == 18) && request == VIDIOC_S_FMT) {
        struct v4l2_format *f = arg;
        if (f->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            jpeg_capture_set = 1;
            if (mode == 13) f->fmt.pix_mp.width += 4;
            jpeg_raw = (struct v4l2_format){.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE};
            jpeg_raw.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_YUV420M;
            jpeg_raw.fmt.pix_mp.num_planes = 3; /* Hantro resets the raw format. */
            jpeg_raw.fmt.pix_mp.width = jpeg_raw.fmt.pix_mp.height = 4;
        } else {
            jpeg_output_after_capture = jpeg_capture_set;
            jpeg_raw = *f;
            jpeg_raw.fmt.pix_mp.height = 6;
            jpeg_raw.fmt.pix_mp.plane_fmt[0].bytesperline = 8;
            jpeg_raw.fmt.pix_mp.plane_fmt[1].bytesperline = 8;
            jpeg_raw.fmt.pix_mp.plane_fmt[0].sizeimage = 48;
            jpeg_raw.fmt.pix_mp.plane_fmt[1].sizeimage = 24;
            *f = jpeg_raw;
        }
        return 0;
    }
    if ((mode == 6 || mode == 13 || mode == 18) && request == VIDIOC_G_FMT) {
        *(struct v4l2_format *)arg = jpeg_raw;
        return 0;
    }
    if ((mode == 6 || mode == 13 || mode == 18) && request == VIDIOC_QUERYBUF) {
        struct v4l2_buffer *b = arg;
        if (b->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
            b->length = jpeg_raw.fmt.pix_mp.num_planes;
            b->m.planes[0].length = 48;
            b->m.planes[0].m.mem_offset = 0;
            b->m.planes[1].length = 24;
            b->m.planes[1].m.mem_offset = 4096;
        } else {
            b->length = 1;
            b->m.planes[0].length = 64;
            b->m.planes[0].m.mem_offset = 8192;
        }
        return 0;
    }
    if (request == VIDIOC_REQBUFS) {
        struct v4l2_requestbuffers *r = arg;
        if (!r->count) releases++;
        if (!r->count && mode == 17) { errno = EBUSY; return -1; }
        return 0;
    }
    if (request == VIDIOC_S_FMT && mode == 4) {
        struct v4l2_format *f = arg;
        if (f->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) { errno = EIO; return -1; }
        return 0;
    }
    if (request == VIDIOC_QBUF) {
        struct v4l2_buffer *b = arg;
        if (b->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
            out_bytes = b->m.planes[0].bytesused;
            if (mode == 6 || mode == 13 || mode == 18) {
                expect(b->length == 2 && b->m.planes[1].length == 24 && b->m.planes[1].bytesused == 24,
                       "jpeg-negotiated-two-plane-output");
                jpeg_output_submitted++;
            }
        }
        return 0;
    }
    if (request == VIDIOC_DQBUF) {
        struct v4l2_buffer *b = arg;
        if (b->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
            if (mode == 7) b->flags |= V4L2_BUF_FLAG_ERROR;
            if (mode == 8) { errno = EAGAIN; return -1; }
            if (mode == 9) b->index = 1;
            return 0;
        }
        b->m.planes[0].bytesused = mode == 2 ? 65 : 48;
        b->m.planes[0].data_offset = mode == 5 ? 0 : 4;
        if (mode == 1) b->flags |= V4L2_BUF_FLAG_ERROR;
        return 0;
    }
    return 0;
}
static void jpeg_case(int test_mode)
{
    mode = test_mode; releases = 0; jpeg_quality_value = 0;
    uint8_t live[24]; memset(live, 0x6a, sizeof(live));
    struct vav4l2_surface src = {.width = 4, .height = 4, .format = VA_FOURCC_NV12,
        .has_pic = 1, .cap_fourcc = V4L2_PIX_FMT_NV12, .buf_index = 0,
        .cap_view = live, .stride = 4, .aligned_h = 4, .memfd_fd = -1};
    struct vav4l2_context c = {.codec = VAV4L2_CODEC_JPEG_ENC, .current_surface = &src};
    c.v4l2_fd = (mode == 6 || mode == 13 || mode == 18) ? memfd_create("jpeg-negotiation", MFD_CLOEXEC) : open("/dev/null", O_RDWR | O_CLOEXEC);
    if (mode == 6 || mode == 13 || mode == 18) {
        uint8_t payload[64]; memset(payload, 0x9b, sizeof(payload));
        if (c.v4l2_fd < 0 || ftruncate(c.v4l2_fd, 12288) < 0 || pwrite(c.v4l2_fd, payload, sizeof(payload), 8192) != (ssize_t)sizeof(payload)) {
            perror("jpeg test backing"); exit(1);
        }
        jpeg_capture_set = jpeg_output_after_capture = jpeg_output_submitted = 0;
    }
    unsigned cap = mode == 3 ? 16 : 64;
    struct vav4l2_buffer coded = {.buffer_id = 1, .type = VAEncCodedBufferType, .size = cap, .num_elements = 1};
    coded.data = calloc(1, sizeof(VACodedBufferSegment) + cap);
    if (!coded.data) exit(1);
    VACodedBufferSegment *seg = coded.data; seg->buf = seg + 1;
    c.buffers = &coded;
    VAEncPictureParameterBufferJPEG pic = {.coded_buf = 1, .picture_width = 4, .picture_height = 4, .quality = 50};
    struct vav4l2_buffer params = {.type = VAEncPictureParameterBufferType, .data = &pic, .size = sizeof(pic)};
    struct { VAEncMiscParameterType type; VAEncMiscParameterBufferQualityLevel level; } preset = {
        .type = VAEncMiscParameterTypeQualityLevel, .level.quality_level = 1};
    struct vav4l2_buffer preset_buf = {.type = VAEncMiscParameterBufferType, .data = &preset, .size = sizeof(preset)};
    /* FFmpeg8 default quality80 matrices are already scaled by truncation;
     * its picture quality is50. Preserve the established native50 path. */
    VAQMatrixBufferJPEG matrix = {.load_lum_quantiser_matrix = 1, .load_chroma_quantiser_matrix = 1,
        .lum_quantiser_matrix = {6,4,4,5,4,4,6,5,5,5,7,6,6,7,9,16,10,9,8,8,9,19,14,14,11,16,23,20,24,24,22,20,22,22,25,28,36,31,25,27,34,27,22,22,32,43,32,34,38,39,41,41,41,24,30,45,48,44,40,48,36,40,41,39},
        .chroma_quantiser_matrix = {6,7,7,9,8,9,18,10,10,18,39,26,22,26,39,39,39,39,39,39,39,39,39,39,39,39,39,39,39,39,39,39,39,39,39,39,39,39,39,39,39,39,39,39,39,39,39,39,39,39,39,39,39,39,39,39,39,39,39,39,39,39,39,39}};
    struct vav4l2_buffer matrix_buf = {.type = VAQMatrixBufferType, .data = &matrix, .size = sizeof(matrix)};
    struct vav4l2_buffer *buffers[] = {&params, &preset_buf, &matrix_buf};
    if (mode == 10 || mode == 11) pic.quality = 0;
    if (mode == 11) preset.level.quality_level = UINT32_MAX;
    struct vav4l2_m2m_state *q = &c.jpeg_q;
    if (mode != 4 && mode != 6 && mode != 13 && mode != 18) {
        q->valid = 1; q->key[0] = q->key[1] = 4; q->key[2] = 50;
        q->out_planes = 1; q->out_len[0] = 72; q->cap_len = 64;
        q->out_map[0] = mapping(72); q->cap_map = mapping(64);
        q->ofmt.fmt.pix_mp.width = 4; q->ofmt.fmt.pix_mp.height = 6; q->ofmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
        q->ofmt.fmt.pix_mp.num_planes = 1; q->ofmt.fmt.pix_mp.plane_fmt[0].bytesperline = 8;
        memset(q->cap_map, 0x9b, 64);
    }
    vav4l2_set_ioctl_hook(m2m_ioctl);
    VAStatus result = vav4l2_jpeg_encode(&c, buffers, mode == 18 ? 3 : mode >= 10 ? 2 : 1);
    if (!mode || mode == 6 || mode == 10 || mode == 11 || mode == 18) {
        expect(result == VA_STATUS_SUCCESS && seg->size == 44 && ((uint8_t *)seg->buf)[43] == 0x9b,
               "jpeg-full-capacity-with-data-offset");
        if (result != VA_STATUS_SUCCESS) {
            vav4l2_m2m_teardown(&c, q);
            vav4l2_set_ioctl_hook(NULL);
            free(coded.data); close(c.v4l2_fd);
            return;
        }
        if (mode == 18) expect(jpeg_quality_value == 50 && q->key[2] == 50,
            "jpeg-ffmpeg-default-tables-preserve-native-picture-quality");
        if (mode == 6 || mode == 13 || mode == 18) {
            expect(jpeg_output_after_capture && jpeg_output_submitted == 1 &&
                   q->ofmt.fmt.pix_mp.pixelformat == V4L2_PIX_FMT_NV12M && q->out_planes == 2,
                   "jpeg-selects-raw-after-capture-reset");
            expect(((uint8_t *)q->out_map[0])[0] == 0x6a && ((uint8_t *)q->out_map[1])[0] == 0x6a,
                   "jpeg-negotiated-nv12m-plane-pixels");
            /* Identical second picture must reuse the negotiated queues. */
            expect(vav4l2_jpeg_encode(&c, buffers, mode == 18 ? 3 : 1) == VA_STATUS_SUCCESS && jpeg_output_submitted == 2,
                   "jpeg-negotiation-reused-next-picture");
        } else {
            expect(((uint8_t *)q->out_map[0])[0] == 0x6a && ((uint8_t *)q->out_map[0])[48] == 0x6a,
                   "jpeg-live-capture-and-negotiated-uv-offset");
        }
    } else {
        expect(result != VA_STATUS_SUCCESS && !seg->size && !q->valid && releases == 2, "jpeg-error-tears-down-without-partial-success");
        if (mode == 3) expect(result == VA_STATUS_ERROR_NOT_ENOUGH_BUFFER &&
            (seg->status & VA_CODED_BUF_STATUS_FRAME_SIZE_OVERFLOW), "jpeg-small-coded-buffer-reports-overflow");
    }
    vav4l2_m2m_teardown(&c, q);
    vav4l2_set_ioctl_hook(NULL);
    free(coded.data); close(c.v4l2_fd);
}
static void vpp_geometry(void)
{
    mode = 0; out_bytes = 0;
    struct vav4l2_driver_data dd = {0};
    uint8_t input[144], output[96]; memset(input, 0x11, sizeof(input)); memset(output, 0, sizeof(output));
    memset(input + 16 * 6, 0x7a, 32);
    struct vav4l2_surface src = {.width = 4, .height = 4, .format = VA_FOURCC_NV12,
        .has_pic = 1, .cap_fourcc = V4L2_PIX_FMT_NV12, .buf_index = 0,
        .cap_view = input, .stride = 16, .aligned_h = 6};
    struct vav4l2_surface dst = {.width = 4, .height = 4, .format = VA_FOURCC_NV12,
        .cpu_ptr = output, .cpu_stride = 16, .cpu_size = sizeof(output)};
    dd.surfaces[1] = &src;
    struct vav4l2_context c = {.driver_data = &dd, .codec = VAV4L2_CODEC_VPP, .current_surface = &dst};
    c.v4l2_fd = open("/dev/null", O_RDWR | O_CLOEXEC);
    struct vav4l2_m2m_state *q = &c.vpp_q;
    q->valid = 1; q->key[0] = q->key[1] = q->key[3] = q->key[4] = 4;
    q->key[2] = q->key[5] = VA_FOURCC_NV12;
    q->out_planes = 1;
    q->out_len[0] = q->cap_len = 48;
    q->out_map[0] = mapping(48); q->cap_map = mapping(48);
    q->ofmt.fmt.pix_mp.plane_fmt[0].bytesperline = q->cfmt.fmt.pix_mp.plane_fmt[0].bytesperline = 8;
    memset(q->cap_map, 0x22, 48); memset((uint8_t *)q->cap_map + 32, 0x5b, 16);
    VAProcPipelineParameterBuffer pipe = {.surface = 1};
    struct vav4l2_buffer params = {.type = VAProcPipelineParameterBufferType, .data = &pipe, .size = sizeof(pipe)};
    struct vav4l2_buffer *buffers[] = {&params};
    vav4l2_set_ioctl_hook(m2m_ioctl);
    /* VPP payload has no offset; report 48 bytes for this case. */
    mode = 5;
    VAStatus result = vav4l2_vpp_run(&c, buffers, 1);
    expect(result == VA_STATUS_SUCCESS && out_bytes == 48 && ((uint8_t *)q->out_map[0])[32] == 0x7a,
           "vpp-padded-source-chroma-and-output-bytesused");
    expect(output[64] == 0x5b && output[32] == 0x22, "vpp-destination-plane-offset");
    vav4l2_m2m_teardown(&c, q);
    vav4l2_set_ioctl_hook(NULL); close(c.v4l2_fd);
}
static void failed_teardown(void)
{
    for (mode = 14; mode <= 17; mode++) {
        struct vav4l2_context c = {.v4l2_fd = open("/dev/null", O_RDWR | O_CLOEXEC)};
        int fd = c.v4l2_fd;
        struct vav4l2_m2m_state *q = &c.jpeg_q;
        q->valid = 1; q->streaming = mode == 17 ? 0 : 3;
        q->out_map[0] = mapping(64); q->out_len[0] = 64;
        q->cap_map = mapping(64); q->cap_len = 64;
        stops = releases = 0;
        vav4l2_set_ioctl_hook(m2m_ioctl);
        vav4l2_m2m_teardown(&c, q);
        expect(c.v4l2_fd == -1 && fcntl(fd, F_GETFD) < 0 && !q->valid && !q->streaming &&
               !q->out_map[0] && !q->cap_map && stops == 2 && releases == (mode == 17 ? 2 : 0),
               "m2m-failed-active-stop-or-release-closes-device-before-reuse");
        vav4l2_set_ioctl_hook(NULL);
        if (c.v4l2_fd >= 0) close(c.v4l2_fd);
    }
}
int main(void)
{
    setenv("VAV4L2_EXPBUF_EXPORT", "1", 1); /* Geometry fixture supplies live capture storage. */
    for (int i = 0; i <= 4; i++) jpeg_case(i);
    jpeg_case(6);
    jpeg_case(13);
    for (int i = 7; i <= 11; i++) jpeg_case(i);
    vpp_geometry();
    failed_teardown();
    jpeg_case(18);
    return failures != 0;
}
