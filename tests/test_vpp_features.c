/* Selection negotiation and composition on memfd-backed mocked M2M queues. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include "va_v4l2.h"
#include "probe.h"
static int failures, selections, formats, ioctls, clamp_selection, rotation, hflip, vflip, rga3_mode, control_calls, tries, submissions;
static int odd422_stride;
static struct vav4l2_context *current;
static struct v4l2_format raw[2];
static struct v4l2_rect crop, compose;
static void expect(int ok, const char *tag)
{
    if (!ok) { fprintf(stderr, "FAIL %s\n", tag); failures++; }
}
static int mock_ioctl(int fd, unsigned long request, void *arg)
{
    (void)fd; ioctls++;
    if (request == VIDIOC_QUERYCAP) { ((struct v4l2_capability *)arg)->capabilities = V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING; return 0; }
    if (request == VIDIOC_TRY_FMT) {
        struct v4l2_format *f = arg; tries++;
        if (rga3_mode && f->fmt.pix_mp.width < 128) f->fmt.pix_mp.width = 128;
        if (rga3_mode && f->fmt.pix_mp.height < 128) f->fmt.pix_mp.height = 128;
        return 0;
    }
    if (request == VIDIOC_S_FMT) {
        struct v4l2_format *f = arg;
        unsigned cap = f->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        unsigned bpp = (f->fmt.pix_mp.pixelformat == V4L2_PIX_FMT_XBGR32 || f->fmt.pix_mp.pixelformat == V4L2_PIX_FMT_XRGB32 ||
             f->fmt.pix_mp.pixelformat == V4L2_PIX_FMT_BGRA32 || f->fmt.pix_mp.pixelformat == V4L2_PIX_FMT_BGRX32 || f->fmt.pix_mp.pixelformat == V4L2_PIX_FMT_Y210) ? 4 :
            (f->fmt.pix_mp.pixelformat == V4L2_PIX_FMT_UYVY || f->fmt.pix_mp.pixelformat == V4L2_PIX_FMT_YUYV ||
             f->fmt.pix_mp.pixelformat == V4L2_PIX_FMT_VYUY || f->fmt.pix_mp.pixelformat == V4L2_PIX_FMT_YVYU || f->fmt.pix_mp.pixelformat == V4L2_PIX_FMT_P010) ? 2 : 1;
        f->fmt.pix_mp.plane_fmt[0].bytesperline = rga3_mode ? f->fmt.pix_mp.width * bpp : bpp == 4 ? 64 : f->fmt.pix_mp.pixelformat == V4L2_PIX_FMT_P010 ? 32 : 16;
        if (odd422_stride && (f->fmt.pix_mp.pixelformat == V4L2_PIX_FMT_NV16 || f->fmt.pix_mp.pixelformat == V4L2_PIX_FMT_NV61))
            f->fmt.pix_mp.plane_fmt[0].bytesperline = 17;
        if (cap) {
            f->fmt.pix_mp.colorspace = raw[0].fmt.pix_mp.colorspace;
            if (!f->fmt.pix_mp.ycbcr_enc) f->fmt.pix_mp.ycbcr_enc = raw[0].fmt.pix_mp.ycbcr_enc;
        }
        raw[cap] = *f; formats++;
        crop = compose = (struct v4l2_rect){.width = 8, .height = 8};
        return 0;
    }
    if (request == VIDIOC_S_SELECTION) {
        struct v4l2_selection *s = arg;
        expect(s->flags == (V4L2_SEL_FLAG_GE | V4L2_SEL_FLAG_LE), "exact-selection-constraints");
        if (s->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
            expect(s->target == V4L2_SEL_TGT_CROP, "output-is-crop"); crop = s->r;
        } else {
            expect(s->target == V4L2_SEL_TGT_COMPOSE, "capture-is-compose"); compose = s->r;
        }
        if (clamp_selection) s->r.left++;
        selections++; return 0;
    }
    if (request == VIDIOC_S_CTRL) {
        control_calls++;
        if (rga3_mode) { errno = EINVAL; return -1; }
        struct v4l2_control *c = arg;
        if (c->id == V4L2_CID_ROTATE) rotation = c->value;
        if (c->id == V4L2_CID_HFLIP) hflip = c->value;
        if (c->id == V4L2_CID_VFLIP) vflip = c->value;
        return 0;
    }
    if (request == VIDIOC_QUERYBUF) {
        struct v4l2_buffer *b = arg;
        if (rga3_mode) {
            int cap = b->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            b->m.planes[0].length = vav4l2_va_image_size(cap ? VA_FOURCC_NV12 : VA_FOURCC_BGRX,
                raw[cap].fmt.pix_mp.plane_fmt[0].bytesperline, 128);
            b->m.planes[0].m.mem_offset = cap ? 1048576 : 0;
            return 0;
        }
        b->m.planes[0].length = b->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE ? 192 :
            raw[1].fmt.pix_mp.pixelformat == V4L2_PIX_FMT_XBGR32 ? 512 : 192;
        unsigned cap = b->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        if (raw[cap].fmt.pix_mp.pixelformat == V4L2_PIX_FMT_NV16 || raw[cap].fmt.pix_mp.pixelformat == V4L2_PIX_FMT_NV61)
            b->m.planes[0].length = raw[cap].fmt.pix_mp.plane_fmt[0].bytesperline * 16;
        if (raw[cap].fmt.pix_mp.pixelformat == V4L2_PIX_FMT_P010 || raw[cap].fmt.pix_mp.pixelformat == V4L2_PIX_FMT_Y210 ||
            raw[cap].fmt.pix_mp.pixelformat == V4L2_PIX_FMT_XRGB32 || raw[cap].fmt.pix_mp.pixelformat == V4L2_PIX_FMT_BGRA32 || raw[cap].fmt.pix_mp.pixelformat == V4L2_PIX_FMT_BGRX32)
            b->m.planes[0].length = 512;
        b->m.planes[0].m.mem_offset = b->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE ? 0 : 4096;
        return 0;
    }
    if (request == VIDIOC_QBUF) {
        struct v4l2_buffer *b = arg;
        if (b->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
            submissions++;
            memset(current->vpp_q.cap_map, 0x33, current->vpp_q.cap_len);
            if (raw[1].fmt.pix_mp.pixelformat == V4L2_PIX_FMT_NV16 || raw[1].fmt.pix_mp.pixelformat == V4L2_PIX_FMT_NV61) {
                unsigned pitch = raw[1].fmt.pix_mp.plane_fmt[0].bytesperline;
                unsigned char *uv = (unsigned char *)current->vpp_q.cap_map + pitch * 8;
                for (unsigned y = 0; y < 8; y++) for (unsigned x = 0; x < 4; x++) {
                    uv[y * pitch + x * 2] = raw[1].fmt.pix_mp.pixelformat == V4L2_PIX_FMT_NV61 ? 0xd2 - y : 0x44 + y;
                    uv[y * pitch + x * 2 + 1] = raw[1].fmt.pix_mp.pixelformat == V4L2_PIX_FMT_NV61 ? 0x44 + y : 0xd2 - y;
                }
            }
            if (raw[1].fmt.pix_mp.pixelformat == V4L2_PIX_FMT_NV12)
                memset((unsigned char *)current->vpp_q.cap_map + (rga3_mode ? 16384 : 128), 0x44, rga3_mode ? 8192 : 64);
            if (raw[1].fmt.pix_mp.pixelformat == V4L2_PIX_FMT_NV21) {
                unsigned char *vu = (unsigned char *)current->vpp_q.cap_map + 128;
                for (unsigned i = 0; i < 64; i++) vu[i] = (i & 1) ? 0x44 : 0xd2;
            }
            if (raw[1].fmt.pix_mp.pixelformat == V4L2_PIX_FMT_YUV420) {
                memset((unsigned char *)current->vpp_q.cap_map + 128, 0x44, 32);
                memset((unsigned char *)current->vpp_q.cap_map + 160, 0xd2, 32);
            }
            if (raw[1].fmt.pix_mp.pixelformat == V4L2_PIX_FMT_UYVY)
                for (unsigned y = 0; y < 8; y++) for (unsigned x = 0; x < 4; x++) {
                    unsigned char *p = (unsigned char *)current->vpp_q.cap_map + y * 16 + x * 4;
                    p[0] = 0x44; p[1] = 0x33; p[2] = 0xd2; p[3] = 0x35;
                }
            if (raw[1].fmt.pix_mp.pixelformat == V4L2_PIX_FMT_VYUY ||
                raw[1].fmt.pix_mp.pixelformat == V4L2_PIX_FMT_YVYU || raw[1].fmt.pix_mp.pixelformat == V4L2_PIX_FMT_YUYV) {
                const unsigned char vyuy[] = {0xd2, 0x33, 0x44, 0x35};
                const unsigned char yvyu[] = {0x33, 0xd2, 0x35, 0x44};
                const unsigned char yuyv[] = {0x33, 0x44, 0x35, 0xd2};
                const unsigned char *pair = raw[1].fmt.pix_mp.pixelformat == V4L2_PIX_FMT_VYUY ? vyuy :
                    raw[1].fmt.pix_mp.pixelformat == V4L2_PIX_FMT_YVYU ? yvyu : yuyv;
                for (unsigned y = 0; y < 8; y++) for (unsigned x = 0; x < 4; x++)
                    memcpy((unsigned char *)current->vpp_q.cap_map + y * 16 + x * 4, pair, 4);
            }
        }
        return 0;
    }
    if (request == VIDIOC_DQBUF) {
        struct v4l2_buffer *b = arg;
        b->m.planes[0].bytesused = current->vpp_q.cap_len;
        return 0;
    }
    return 0;
}
static VAStatus run(struct vav4l2_context *c, VAProcPipelineParameterBuffer *pipe)
{
    struct vav4l2_buffer params = {.type = VAProcPipelineParameterBufferType, .size = sizeof(*pipe), .data = pipe};
    struct vav4l2_buffer *buffers[] = {&params};
    current = c;
    return vav4l2_vpp_run(c, buffers, 1);
}
int main(void)
{
    setenv("VAV4L2_EXPBUF_EXPORT", "1", 1);
    struct vav4l2_driver_data dd = {0};
    unsigned char input[512], output[512]; memset(input, 0x11, sizeof(input)); memset(output, 0xaa, sizeof(output));
    struct vav4l2_surface src = {.width = 8, .height = 8, .format = VA_FOURCC_NV12,
        .cpu_ptr = input, .cpu_stride = 16, .cpu_size = sizeof(input), .last_writer = VAV4L2_WRITER_CPU};
    struct vav4l2_surface dst = {.width = 8, .height = 8, .format = VA_FOURCC_NV12,
        .cpu_ptr = output, .cpu_stride = 16, .cpu_size = 192};
    dd.surfaces[1] = &src;
    struct vav4l2_context c = {.codec = VAV4L2_CODEC_VPP, .driver_data = &dd, .current_surface = &dst};
    c.v4l2_fd = memfd_create("vpp-feature-queues", MFD_CLOEXEC);
    if (c.v4l2_fd < 0 || ftruncate(c.v4l2_fd, 8192) < 0) return 1;
    VARectangle in = {2, 2, 4, 4}, out = {2, 2, 4, 4};
    VAProcPipelineParameterBuffer pipe = {.surface = 1, .surface_region = &in, .output_region = &out};
    vav4l2_set_ioctl_hook(mock_ioctl);
    expect(run(&c, &pipe) == VA_STATUS_SUCCESS && selections == 2 && formats == 2 &&
           crop.left == 2 && compose.top == 2, "cold-crop-compose");
    expect(output[0] == 0xaa && output[2 * 16 + 2] == 0x33 && output[7 * 16 + 7] == 0xaa &&
           output[128] == 0xaa && output[128 + 16 + 2] == 0x44, "transparent-background-keeps-outside-planes");
    in.x = in.y = 0; out.x = out.y = 4;
    expect(run(&c, &pipe) == VA_STATUS_SUCCESS && selections == 4 && formats == 2 && compose.left == 4,
           "warm-region-change-does-not-reallocate");
    pipe.surface_region = pipe.output_region = NULL;
    expect(run(&c, &pipe) == VA_STATUS_SUCCESS && selections == 6 && !c.vpp_q.selection_active &&
           crop.width == 8 && compose.height == 8, "full-frame-reuse-resets-selection");
    expect(run(&c, &pipe) == VA_STATUS_SUCCESS && selections == 6 && formats == 2,
           "ordinary-full-frame-does-not-require-selection-support");
    pipe.filter_flags = VA_FILTER_SCALING_HQ;
    pipe.input_color_properties.chroma_sample_location = pipe.output_color_properties.chroma_sample_location =
        VA_CHROMA_SITING_VERTICAL_CENTER | VA_CHROMA_SITING_HORIZONTAL_LEFT;
    expect(run(&c, &pipe) == VA_STATUS_SUCCESS, "ffmpeg-default-hq-and-unchanged-chroma-siting");
    pipe.output_color_properties.chroma_sample_location = VA_CHROMA_SITING_VERTICAL_CENTER | VA_CHROMA_SITING_HORIZONTAL_CENTER;
    expect(run(&c, &pipe) == VA_STATUS_ERROR_UNIMPLEMENTED, "phase-conversion-still-unsupported");
    pipe.input_color_properties.chroma_sample_location = pipe.output_color_properties.chroma_sample_location = 0;
    pipe.filter_flags = VA_FILTER_SCALING_NL_ANAMORPHIC;
    expect(run(&c, &pipe) == VA_STATUS_ERROR_UNIMPLEMENTED, "nonlinear-scaling-not-a-quality-preference");
    pipe.filter_flags = 0;
    out = (VARectangle){2, 2, 4, 4}; pipe.output_region = &out;
    pipe.output_background_color = 0xff000000;
    expect(run(&c, &pipe) == VA_STATUS_SUCCESS && output[0] == 16 && output[128] == 128 &&
           output[2 * 16 + 2] == 0x33, "opaque-black-background-is-limited-range-yuv");
    pipe.rotation_state = VA_ROTATION_90; pipe.mirror_state = VA_MIRROR_HORIZONTAL | VA_MIRROR_VERTICAL;
    expect(run(&c, &pipe) == VA_STATUS_SUCCESS && rotation == 270 && hflip == 0 && vflip == 0,
           "double-mirror-is-a-half-turn-after-rotation");
    dst.format = VA_FOURCC_BGRX; dst.cpu_stride = 64; dst.cpu_size = sizeof(output);
    pipe.surface_color_standard = VAProcColorStandardBT709; pipe.output_background_color = 0xff123456;
    expect(run(&c, &pipe) == VA_STATUS_SUCCESS && raw[0].fmt.pix_mp.colorspace == V4L2_COLORSPACE_REC709 &&
           output[0] == 0x56 && output[1] == 0x34 && output[2] == 0x12 && output[3] == 255,
           "bt709-negotiation-and-rgb-background-byte-order");
    unsigned before = ioctls;
    pipe.input_color_properties.color_range = VA_SOURCE_RANGE_FULL;
    expect(run(&c, &pipe) == VA_STATUS_SUCCESS && raw[0].fmt.pix_mp.quantization == V4L2_QUANTIZATION_FULL_RANGE,
           "generic-node-negotiates-full-yuv-range-without-rga-restriction");
    before = ioctls;
    pipe.input_color_properties.color_range = 0;
    pipe.num_additional_outputs = 1;
    expect(run(&c, &pipe) == VA_STATUS_ERROR_UNIMPLEMENTED && ioctls == (int)before, "additional-output-not-silently-ignored");
    pipe.num_additional_outputs = 0; out.x = -1;
    expect(run(&c, &pipe) == VA_STATUS_ERROR_INVALID_PARAMETER && ioctls == (int)before, "negative-compose-origin");
    out.x = 2; clamp_selection = 1;
    expect(run(&c, &pipe) == VA_STATUS_ERROR_OPERATION_FAILED && !c.vpp_q.valid, "clamped-selection-is-not-success");
    clamp_selection = 0; c.vpp_behaviors = VAV4L2_COMPAT_SWAP_MIRROR_AXES;
    dst.format = VA_FOURCC_NV12; dst.cpu_stride = 16; dst.cpu_size = 192;
    const unsigned added[] = {VA_FOURCC_P010, VA_FOURCC_Y210, VA_FOURCC_XRGB, VA_FOURCC_ABGR, VA_FOURCC_XBGR};
    for (unsigned i = 0; i < sizeof(added) / sizeof(added[0]); i++) {
        src.format = dst.format = added[i]; src.cpu_stride = dst.cpu_stride = added[i] == VA_FOURCC_P010 ? 32 : 64;
        src.cpu_size = dst.cpu_size = 512; memset(input, 0xff, sizeof(input));
        pipe = (VAProcPipelineParameterBuffer){.surface = 1, .output_region = &out, .output_background_color = 0xff000000};
        expect(run(&c, &pipe) == VA_STATUS_SUCCESS, "ten-bit-and-RGB32-staged-processing");
        unsigned offset = 2 * dst.cpu_stride + 2 * (added[i] == VA_FOURCC_P010 ? 2 : 4);
        expect(output[offset] == 0x33, "staged-compose-preserves-visible-output");
        if (i < 2) expect(output[0] == 0 && output[1] == 16 && output[i ? 3 : dst.cpu_stride * 8 + 1] == 128,
            "ten-bit-limited-background-stores-high-ten-bits");
        else expect(output[0] == 255 && !output[1] && !output[2] && !output[3], "leading-alpha-RGB32-background");
    }
    unsigned char packed10[160]; memset(packed10, 0xff, sizeof(packed10));
    for (unsigned i = 0; i < 2; i++) {
        src.format = dst.format = i ? VA_FOURCC_Y210 : VA_FOURCC_P010;
        src.cpu_stride = dst.cpu_stride = i ? 64 : 32; src.cpu_size = dst.cpu_size = 512;
        src.cap_view = packed10; src.cap_fourcc = i ? V4L2_PIX_FMT_NV20 : V4L2_PIX_FMT_NV15;
        src.stride = 10; src.aligned_h = 8; src.buf_index = 0; src.has_pic = 1; src.last_writer = VAV4L2_WRITER_NONE;
        pipe = (VAProcPipelineParameterBuffer){.surface = 1};
        expect(run(&c, &pipe) == VA_STATUS_SUCCESS && ((unsigned char *)c.vpp_q.out_map[0])[0] == 0xc0 &&
            ((unsigned char *)c.vpp_q.out_map[0])[1] == 255 && src.cap_view == packed10 && src.buf_index == 0,
            "packed-ten-bit-decode-stages-losslessly-without-detaching-DPB");
    }
    src.cap_view = NULL; src.cap_fourcc = src.stride = src.aligned_h = src.has_pic = 0;
    src.buf_index = -1; src.last_writer = VAV4L2_WRITER_CPU;
    src.format = dst.format = VA_FOURCC_NV12; src.cpu_stride = dst.cpu_stride = 16; src.cpu_size = dst.cpu_size = 192;
    struct vav4l2_context planar_capture = {.v4l2_fd = -1, .capture_bufs_allocd = 1};
    struct v4l2_pix_format_mplane native = {.width = 8, .height = 8, .field = V4L2_FIELD_NONE,
        .pixelformat = V4L2_PIX_FMT_NV12M, .num_planes = 2};
    native.plane_fmt[0].bytesperline = native.plane_fmt[1].bytesperline = 16;
    native.plane_fmt[0].sizeimage = 128; native.plane_fmt[1].sizeimage = 64;
    expect(!vav4l2_capture_layout_set(&planar_capture, &native), "native-planar-VPP-source-fixture");
    src.capture_ctx = &planar_capture; src.cap_view = input; src.cap_fourcc = V4L2_PIX_FMT_NV12;
    src.stride = 16; src.aligned_h = 8; src.buf_index = 0; src.has_pic = 1; src.last_writer = VAV4L2_WRITER_NONE;
    pipe = (VAProcPipelineParameterBuffer){.surface = 1}; setenv("VAV4L2_VPP_DMABUF", "auto", 1);
    expect(run(&c, &pipe) == VA_STATUS_SUCCESS && src.capture_ctx == &planar_capture,
        "native-planar-capture-stages-without-single-object-relabel");
    unsetenv("VAV4L2_VPP_DMABUF"); vav4l2_capture_layout_release(&planar_capture);
    src.capture_ctx = NULL; src.cap_view = NULL; src.cap_fourcc = src.stride = src.aligned_h = src.has_pic = 0;
    src.buf_index = -1; src.last_writer = VAV4L2_WRITER_CPU;
    const uint32_t extra_layouts[] = {VA_FOURCC_NV21, VA_FOURCC_YV12};
    for (unsigned i = 0; i < 2; i++) {
        src.format = dst.format = extra_layouts[i];
        memset(input + 128, 0x19, 64);
        if (i) memset(input + 160, 0x2e, 32);
        pipe = (VAProcPipelineParameterBuffer){.surface = 1, .output_region = &out,
            .output_background_color = 0xffff0000, .surface_color_standard = VAProcColorStandardBT601};
        expect(run(&c, &pipe) == VA_STATUS_SUCCESS && raw[0].fmt.pix_mp.pixelformat ==
               (i ? V4L2_PIX_FMT_YUV420 : V4L2_PIX_FMT_NV21) && raw[1].fmt.pix_mp.pixelformat == raw[0].fmt.pix_mp.pixelformat,
               "new-layout-negotiates-canonical-planar-kernel-transport");
        expect(((unsigned char *)c.vpp_q.out_map[0])[128] == (i ? 0x2e : 0x19) &&
               ((unsigned char *)c.vpp_q.out_map[0])[i ? 160 : 129] == 0x19,
               "new-layout-input-copy-reorders-distinct-yv12-components-to-i420");
        expect(output[0] == 82 && output[128] == 240 && output[i ? 160 : 129] == 90 &&
               output[i ? 137 : 146] == 0xd2 && output[i ? 169 : 147] == 0x44,
               "new-layout-compose-and-red-background-use-v-before-u");
        int before_invalid = ioctls;
        out.x = 1;
        expect(run(&c, &pipe) == VA_STATUS_ERROR_INVALID_PARAMETER && ioctls == before_invalid,
               "new-layout-chroma-rectangle-alignment-before-ioctl");
        out.x = 2;
    }
    const struct {
        unsigned va, raw; unsigned char uploaded[4], background[4], captured[4];
    } packed[] = {
        {VA_FOURCC_YUY2, V4L2_PIX_FMT_YUYV, {0x2e,0x19,0x41,0x63}, {82,90,82,240}, {0x33,0x44,0x35,0xd2}},
        {VA_FOURCC_UYVY, V4L2_PIX_FMT_UYVY, {0x19,0x2e,0x63,0x41}, {90,82,240,82}, {0x44,0x33,0xd2,0x35}},
        {VA_FOURCC_VYUY, V4L2_PIX_FMT_VYUY, {0x63,0x2e,0x19,0x41}, {240,82,90,82}, {0xd2,0x33,0x44,0x35}},
        {VA_FOURCC_YVYU, V4L2_PIX_FMT_YVYU, {0x2e,0x63,0x41,0x19}, {82,240,82,90}, {0x33,0xd2,0x35,0x44}},
    };
    for (unsigned i = 0; i < 4; i++) {
        src.format = dst.format = packed[i].va;
        for (unsigned j = 0; j < 128; j++) input[j] = packed[i].uploaded[j & 3];
        pipe = (VAProcPipelineParameterBuffer){.surface = 1, .output_region = &out,
            .output_background_color = 0xffff0000, .surface_color_standard = VAProcColorStandardBT601};
        expect(run(&c, &pipe) == VA_STATUS_SUCCESS && raw[0].fmt.pix_mp.pixelformat == packed[i].raw &&
               raw[1].fmt.pix_mp.pixelformat == packed[i].raw, "packed422-exact-kernel-format");
        expect(!memcmp(c.vpp_q.out_map[0], packed[i].uploaded, 4) &&
               !memcmp(output, packed[i].background, 4) && !memcmp(output + 36, packed[i].captured, 4),
               "packed422-input-compose-and-background-independent-byte-order");
        int before_invalid = ioctls; out.x = 1;
        expect(run(&c, &pipe) == VA_STATUS_ERROR_INVALID_PARAMETER && ioctls == before_invalid,
               "packed422-invalid-chroma-origin-before-ioctl");
        out.x = 2;
    }
    struct vav4l2_surface saved_src = src;
    unsigned char nv16[256] = {0};
    for (unsigned y = 0; y < 8; y++) for (unsigned x = 0; x < 8; x++) {
        nv16[y * 16 + x] = 17 + x + y * 11;
        nv16[128 + y * 16 + x] = (x & 1) ? 171 + y : 31 + x / 2;
    }
    src = (struct vav4l2_surface){.width = 8, .height = 8, .format = VA_FOURCC_YUY2,
        .has_pic = 1, .cap_fourcc = V4L2_PIX_FMT_NV16, .cap_view = nv16, .buf_index = 0,
        .stride = 16, .aligned_h = 8, .last_writer = VAV4L2_WRITER_MEMFD, .memfd_fd = -1};
    expect(vav4l2_surface_grow_memfd(&src, sizeof(nv16)) == 0, "nv16-memfd-fixture");
    void *capture_copy = vav4l2_surface_map_memfd(&src, sizeof(nv16));
    if (!capture_copy) return 1;
    memcpy(capture_copy, nv16, sizeof(nv16));
    for (unsigned fallback = 0; fallback < 2; fallback++) {
        setenv("VAV4L2_EXPBUF_EXPORT", fallback ? "0" : "1", 1);
        pipe = (VAProcPipelineParameterBuffer){.surface = 1};
        expect(run(&c, &pipe) == VA_STATUS_SUCCESS && raw[0].fmt.pix_mp.pixelformat == V4L2_PIX_FMT_YUYV,
               "decoded-nv16-enters-existing-packed-vpp-transport");
        for (unsigned y = 0; y < 8; y++) for (unsigned x = 0; x < 4; x++) {
            unsigned char expected[] = {17 + x * 2 + y * 11, 31 + x, 18 + x * 2 + y * 11, 171 + y};
            expect(!memcmp((unsigned char *)c.vpp_q.out_map[0] + y * 16 + x * 4, expected, 4),
                   "nv16-full-height-chroma-and-luma-preserved-in-yuy2-staging");
        }
        expect(src.last_writer == VAV4L2_WRITER_MEMFD && src.cap_view == nv16 && src.buf_index == 0,
               "vpp-read-does-not-replace-decoder-reference-storage");
        int before_bad_capture = ioctls; src.stride = 7;
        expect(run(&c, &pipe) == VA_STATUS_ERROR_INVALID_PARAMETER && ioctls == before_bad_capture,
               "nv16-short-capture-row-rejected-before-queue-ioctl");
        src.stride = 16; src.cap_fourcc = V4L2_PIX_FMT_NV20;
        expect(run(&c, &pipe) == VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT && ioctls == before_bad_capture,
               "ten-bit-capture-is-not-silently-truncated-by-vpp");
        src.cap_fourcc = V4L2_PIX_FMT_NV16;
    }
    src.cpu_ptr = input; src.cpu_stride = 16; src.cpu_size = sizeof(input); src.last_writer = VAV4L2_WRITER_CPU;
    expect(run(&c, &pipe) == VA_STATUS_SUCCESS && !memcmp(c.vpp_q.out_map[0], input, 16),
           "cpu-replacement-is-packed-even-with-stale-nv16-capture-metadata");
    if (src.memfd_map) munmap(src.memfd_map, src.memfd_map_size);
    if (src.memfd_retired) munmap(src.memfd_retired, src.memfd_retired_size);
    close(src.memfd_fd); src = saved_src; src.format = dst.format = VA_FOURCC_NV12;
    struct vav4l2_surface before_planar_src = src, before_planar_dst = dst;
    unsigned char planar_input[256], planar_output[256];
    const unsigned planar_formats[] = {VA_FOURCC_422H, VA_FOURCC_YV16};
    for (unsigned layout = 0; layout < 2; layout++) for (unsigned odd = 0; odd < 2; odd++) {
        vav4l2_m2m_teardown(&c, &c.vpp_q); odd422_stride = odd;
        memset(planar_input, 0xa5, sizeof(planar_input)); memset(planar_output, 0xa5, sizeof(planar_output));
        for (unsigned y = 0; y < 8; y++) {
            for (unsigned x = 0; x < 8; x++) planar_input[y * 16 + x] = 17 + y * 7 + x;
            for (unsigned x = 0; x < 4; x++) {
                planar_input[128 + y * 8 + x] = layout ? 173 + y + x : 31 + y * 2 + x;
                planar_input[192 + y * 8 + x] = layout ? 31 + y * 2 + x : 173 + y + x;
            }
        }
        src.format = dst.format = planar_formats[layout]; src.cpu_ptr = planar_input; src.cpu_size = sizeof(planar_input);
        dst.cpu_ptr = planar_output; dst.cpu_size = sizeof(planar_output);
        VARectangle region = {2, 1, 4, 3};
        pipe = (VAProcPipelineParameterBuffer){.surface = 1, .output_region = &region,
            .output_background_color = 0xffff0000, .surface_color_standard = VAProcColorStandardBT601};
        expect(run(&c, &pipe) == VA_STATUS_SUCCESS && raw[0].fmt.pix_mp.pixelformat ==
               (layout ? V4L2_PIX_FMT_NV61 : V4L2_PIX_FMT_NV16) && raw[1].fmt.pix_mp.pixelformat == raw[0].fmt.pix_mp.pixelformat,
               "planar422-uses-native-nv16-nv61-queue-directions");
        unsigned pitch = odd ? 17 : 16; unsigned char *queued = c.vpp_q.out_map[0];
        if (!queued) { failures++; continue; }
        for (unsigned y = 0; y < 8; y++) for (unsigned x = 0; x < 4; x++)
            expect(queued[pitch * 8 + y * pitch + x * 2] == planar_input[128 + y * 8 + x] &&
                   queued[pitch * 8 + y * pitch + x * 2 + 1] == planar_input[192 + y * 8 + x],
                   "planar422-upload-has-full-height-chroma-and-independent-queue-stride");
        expect(planar_output[0] == 82 && planar_output[128] == (layout ? 240 : 90) &&
               planar_output[192] == (layout ? 90 : 240), "planar422-background-uses-physical-plane-order");
        for (unsigned y = 1; y < 4; y++) for (unsigned x = 1; x < 3; x++)
            expect(planar_output[128 + y * 8 + x] == (layout ? 0xd2 - y : 0x44 + y) &&
                   planar_output[192 + y * 8 + x] == (layout ? 0x44 + y : 0xd2 - y),
                   "planar422-compose-keeps-odd-vertical-origin-and-every-chroma-row");
        int before_invalid = ioctls; region.x = 1;
        expect(run(&c, &pipe) == VA_STATUS_ERROR_INVALID_PARAMETER && ioctls == before_invalid,
               "planar422-invalid-horizontal-origin-rejected-before-ioctl");
    }
    odd422_stride = 0; src = before_planar_src; dst = before_planar_dst;
    pipe = (VAProcPipelineParameterBuffer){.surface = 1};
    uint32_t transform_keys[16];
    for (unsigned turn = 0; turn < 4; turn++) for (unsigned mirror = 0; mirror < 4; mirror++) {
        pipe.rotation_state = turn; pipe.mirror_state = mirror;
        int before_controls = control_calls;
        expect(run(&c, &pipe) == VA_STATUS_SUCCESS && control_calls == before_controls + 3,
               "changed-transform-reprograms-all-controls");
        int wanted_rotation = turn * 90, wanted_hflip = mirror & 1, wanted_vflip = !!(mirror & 2);
        vav4l2_vpp_transform_controls(VAV4L2_COMPAT_SWAP_MIRROR_AXES, &wanted_rotation, &wanted_hflip, &wanted_vflip);
        expect(rotation == wanted_rotation && hflip == wanted_hflip && vflip == wanted_vflip,
               "warm-sequence-applies-requested-transform-including-270-mirrors");
        unsigned at = turn * 4 + mirror;
        transform_keys[at] = c.vpp_q.key[6];
        for (unsigned previous = 0; previous < at; previous++)
            expect(transform_keys[at] != transform_keys[previous], "all-sixteen-transform-cache-keys-are-distinct");
    }
    int same_controls = control_calls, same_formats = formats;
    expect(run(&c, &pipe) == VA_STATUS_SUCCESS && control_calls == same_controls && formats == same_formats,
           "identical-transform-still-reuses-warm-queue");
    VARectangle source_region = {0, 0, 4, 4}, output_regions[] = {{0, 0, 4, 4}, {4, 4, 4, 4}};
    VAProcPipelineParameterBuffer pipelines[] = {{.surface = 1, .surface_region = &source_region,
        .output_region = &output_regions[0], .output_background_color = 0xff000000},
        {.surface = 1, .surface_region = &source_region, .output_region = &output_regions[1]}};
    struct vav4l2_buffer batch = {.type = VAProcPipelineParameterBufferType, .size = sizeof(pipelines[0]),
        .num_elements = 2, .allocated_elements = 2, .data = pipelines};
    struct vav4l2_buffer *batch_list[] = {&batch};
    int before_submissions = submissions; current = &c;
    expect(vav4l2_vpp_run(&c, batch_list, 1) == VA_STATUS_SUCCESS && submissions == before_submissions + 2 &&
           output[0] == 0x33 && output[4 * 16 + 4] == 0x33 && output[4] == 16 && output[4 * 16] == 16 &&
           output[128] == 0x44 && output[128 + 2 * 16 + 4] == 0x44, "compose-every-pipeline-array-element-in-order");
    struct vav4l2_buffer first = batch, second = batch;
    first.num_elements = first.allocated_elements = second.num_elements = second.allocated_elements = 1;
    second.data = &pipelines[1];
    struct vav4l2_buffer *pair[] = {&first, &second};
    before_submissions = submissions; memset(output, 0xaa, sizeof(output));
    expect(vav4l2_vpp_run(&c, pair, 2) == VA_STATUS_SUCCESS && submissions == before_submissions + 2 &&
           output[0] == 0x33 && output[4 * 16 + 4] == 0x33 && output[4] == 16,
           "compose-every-submitted-pipeline-buffer-in-order");
    second.type = VASliceDataBufferType; before_submissions = submissions;
    expect(vav4l2_vpp_run(&c, pair, 2) == VA_STATUS_ERROR_INVALID_PARAMETER && submissions == before_submissions,
           "wrong-vpp-buffer-type-is-not-silently-ignored-or-partially-submitted");
    src.status = VASurfaceRendering;
    expect(vav4l2_vpp_run(&c, batch_list, 1) == VA_STATUS_ERROR_SURFACE_BUSY && submissions == before_submissions,
           "pending-input-writer-is-not-read-as-a-completed-frame");
    src.status = VASurfaceReady;
    struct vav4l2_vpp_device rga2 = {.identity = {.driver = "rockchip-rga", .card = "rga2"}, .input = {V4L2_PIX_FMT_NV12, V4L2_PIX_FMT_YUV420, V4L2_PIX_FMT_XBGR32},
        .output = {V4L2_PIX_FMT_NV12, V4L2_PIX_FMT_YUV420, V4L2_PIX_FMT_XBGR32}, .num_input = 3, .num_output = 3, .controls = 7,
        .min_width = {34, 34}, .min_height = {34, 34}, .max_width = {8192, 8192}, .max_height = {8192, 8192}};
    struct vav4l2_vpp_device rga3 = {.identity = {.driver = "rockchip-rga", .card = "rga3"}, .input = {V4L2_PIX_FMT_NV12, V4L2_PIX_FMT_XBGR32, V4L2_PIX_FMT_YUYV},
        .output = {V4L2_PIX_FMT_NV12, V4L2_PIX_FMT_XBGR32, V4L2_PIX_FMT_YUYV}, .num_input = 3, .num_output = 3,
        .min_width = {128, 128}, .min_height = {128, 128}, .max_width = {8176, 8128}, .max_height = {8176, 8128}};
    struct vav4l2_vpp_device generic = rga2;
    strcpy(generic.identity.driver, "other");
    expect(vav4l2_vpp_device_score(&generic, V4L2_PIX_FMT_XBGR32, V4L2_PIX_FMT_NV12, 0, 0, 0, 0, 0) > 0,
        "unrelated-driver-does-not-inherit-rga-quirks-from-card-name");
    expect(vav4l2_vpp_formats_usable(rga3.input, 3, rga3.output, 3), "rga3-raw-formats-do-not-require-rotate-control");
    const uint32_t coded[] = {V4L2_PIX_FMT_VP9_FRAME};
    expect(!vav4l2_vpp_formats_usable(coded, 1, rga3.output, 3), "vp9-decoder-not-a-raw-vpp-node");
    expect(vav4l2_vpp_device_score(&rga3, V4L2_PIX_FMT_XBGR32, V4L2_PIX_FMT_NV12, 0, 0, 0, 0, 0) > 0 &&
           vav4l2_vpp_device_score(&rga2, V4L2_PIX_FMT_XBGR32, V4L2_PIX_FMT_NV12, 0, 0, 0, 0, 0) < 0,
           "rgb-to-yuv-selects-capable-rga3-not-unreliable-rga2");
    expect(vav4l2_vpp_device_score(&rga3, V4L2_PIX_FMT_NV12, V4L2_PIX_FMT_NV12, 90, 0, 0, 0, 0) < 0 &&
           vav4l2_vpp_device_score(&rga3, V4L2_PIX_FMT_NV12, V4L2_PIX_FMT_NV12, 0, 0, 0, 1, 0) < 0 &&
           vav4l2_vpp_device_score(&rga3, V4L2_PIX_FMT_NV12, V4L2_PIX_FMT_NV12, 0, 0, 0, 0, 1) < 0,
           "rga3-unimplemented-transform-and-offset-selection-not-used");
    expect(vav4l2_vpp_device_score(&rga2, V4L2_PIX_FMT_NV12, V4L2_PIX_FMT_YUYV, 0, 0, 0, 0, 0) < 0,
           "format-must-be-enumerated-in-both-queue-directions");
    for (int turn = 0; turn < 4; turn++) for (int mirror = 0; mirror < 4; mirror++) {
        int r = turn * 90, h = mirror & 1, v = !!(mirror & 2);
        vav4l2_vpp_transform_controls(VAV4L2_COMPAT_SWAP_MIRROR_AXES, &r, &h, &v);
        for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
            int vx = turn == 1 ? 3 - y : turn == 2 ? 3 - x : turn == 3 ? y : x;
            int vy = turn == 1 ? x : turn == 2 ? 3 - y : turn == 3 ? 3 - x : y;
            if (mirror & 1) vx = 3 - vx;
            if (mirror & 2) vy = 3 - vy;
            int hx = r == 90 ? 3 - y : r == 180 ? 3 - x : r == 270 ? y : x;
            int hy = r == 90 ? x : r == 180 ? 3 - y : r == 270 ? 3 - x : y;
            /* rga2's register/corner mapping: VFLIP changes X, HFLIP changes Y. */
            if (v) hx = 3 - hx;
            if (h) hy = 3 - hy;
            expect(vx == hx && vy == hy, "all-va-rotation-mirror-mappings-match-rga2-axes");
        }
    }
    dd.vpp_devices_scanned = 1; dd.num_vpp_devices = 1; dd.vpp_devices[0] = rga3;
    VAProcPipelineCaps caps;
    expect(vav4l2_vpp_query_device_caps(&dd, &caps) == VA_STATUS_SUCCESS && caps.rotation_flags == 1 && !caps.mirror_flags &&
           caps.min_input_width == 128 && caps.max_output_width == 8128 && caps.num_output_pixel_formats == 3,
           "rga3-only-caps-do-not-advertise-rga2-limits-or-controls");
    dd.num_vpp_devices = 2; dd.vpp_devices[1] = rga2; dd.vpp_caps_ready = 0;
    expect(vav4l2_vpp_query_device_caps(&dd, &caps) == VA_STATUS_SUCCESS && (caps.rotation_flags & (1u << VA_ROTATION_90)) &&
           caps.mirror_flags == 3 && caps.min_input_width == 34 && caps.max_output_width == 8192,
           "combined-rga2-rga3-caps-union");
    const uint32_t all_formats[] = {V4L2_PIX_FMT_NV12, V4L2_PIX_FMT_NV21, V4L2_PIX_FMT_YUV420,
        V4L2_PIX_FMT_YVU420, V4L2_PIX_FMT_YUYV, V4L2_PIX_FMT_ARGB32, V4L2_PIX_FMT_ABGR32, V4L2_PIX_FMT_XBGR32, V4L2_PIX_FMT_UYVY, V4L2_PIX_FMT_VYUY, V4L2_PIX_FMT_YVYU, V4L2_PIX_FMT_NV16, V4L2_PIX_FMT_NV61, V4L2_PIX_FMT_RGB565, V4L2_PIX_FMT_RGBA32, V4L2_PIX_FMT_RGBX32,
        V4L2_PIX_FMT_XRGB32, V4L2_PIX_FMT_BGRA32, V4L2_PIX_FMT_BGRX32, V4L2_PIX_FMT_P010, V4L2_PIX_FMT_Y210};
    memcpy(dd.vpp_devices[0].input, all_formats, sizeof(all_formats));
    memcpy(dd.vpp_devices[0].output, all_formats, sizeof(all_formats));
    dd.vpp_devices[0].num_input = dd.vpp_devices[0].num_output = sizeof(all_formats) / sizeof(all_formats[0]);
    dd.num_vpp_devices = 1; dd.vpp_caps_ready = 0;
    expect(vav4l2_vpp_query_device_caps(&dd, &caps) == VA_STATUS_SUCCESS &&
           caps.num_input_pixel_formats == 21 && caps.num_output_pixel_formats == 21 &&
           caps.input_pixel_format[8] == VA_FOURCC_UYVY && caps.output_pixel_format[8] == VA_FOURCC_UYVY &&
           caps.input_pixel_format[9] == VA_FOURCC_VYUY && caps.output_pixel_format[10] == VA_FOURCC_YVYU &&
           caps.input_pixel_format[11] == VA_FOURCC_422H && caps.output_pixel_format[12] == VA_FOURCC_YV16,
           "all-twenty-one-vpp-layouts-fit-per-driver-caps-storage");
    dd.vpp_devices[0].input[0] = V4L2_PIX_FMT_UYVY; dd.vpp_devices[0].output[0] = V4L2_PIX_FMT_NV12;
    dd.vpp_devices[0].num_input = dd.vpp_devices[0].num_output = 1; dd.vpp_caps_ready = 0;
    expect(vav4l2_vpp_query_device_caps(&dd, &caps) == VA_STATUS_SUCCESS &&
           caps.num_input_pixel_formats == 1 && caps.input_pixel_format[0] == VA_FOURCC_UYVY &&
           caps.num_output_pixel_formats == 1 && caps.output_pixel_format[0] == VA_FOURCC_NV12 &&
           vav4l2_vpp_device_score(&dd.vpp_devices[0], V4L2_PIX_FMT_NV12, V4L2_PIX_FMT_UYVY, 0, 0, 0, 0, 0) < 0,
           "input-only-uyvy-is-not-advertised-or-selected-as-an-output");
    dd.vpp_devices[0].input[0] = dd.vpp_devices[0].output[0] = V4L2_PIX_FMT_YVU420;
    dd.vpp_devices[0].num_input = dd.vpp_devices[0].num_output = 1; dd.vpp_caps_ready = 0;
    expect(vav4l2_vpp_query_device_caps(&dd, &caps) == VA_STATUS_SUCCESS &&
           !caps.num_input_pixel_formats && !caps.num_output_pixel_formats &&
           !vav4l2_vpp_formats_usable(dd.vpp_devices[0].input, 1, dd.vpp_devices[0].output, 1),
           "native-yvu420-only-node-does-not-advertise-canonical-yv12-transport");
    vav4l2_m2m_teardown(&c, &c.vpp_q); close(c.v4l2_fd);
    char node[] = "test-vpp-node-XXXXXX";
    int node_fd = mkstemp(node);
    if (node_fd < 0 || ftruncate(node_fd, 2097152) < 0) return 1;
    close(node_fd);
    rga3_mode = 1; clamp_selection = 0;
    unsigned char *rgb_data = calloc(1, 65536), *nv12_data = calloc(1, 24576);
    if (!rgb_data || !nv12_data) return 1;
    src = (struct vav4l2_surface){.width = 128, .height = 128, .format = VA_FOURCC_BGRX, .cpu_ptr = rgb_data,
        .cpu_stride = 512, .cpu_size = 65536, .last_writer = VAV4L2_WRITER_CPU};
    dst = (struct vav4l2_surface){.width = 128, .height = 128, .format = VA_FOURCC_NV12, .cpu_ptr = nv12_data,
        .cpu_stride = 128, .cpu_size = 24576};
    dd.num_vpp_devices = 1; dd.vpp_devices[0] = rga3;
    snprintf(dd.vpp_devices[0].path, sizeof(dd.vpp_devices[0].path), "%s", node);
    c.v4l2_fd = memfd_create("vpp-before-switch", MFD_CLOEXEC); c.device_path = strdup("old-rga-node");
    pipe = (VAProcPipelineParameterBuffer){.surface = 1, .output_color_standard = VAProcColorStandardBT601,
        .input_color_properties.color_range = VA_SOURCE_RANGE_FULL, .output_color_properties.color_range = VA_SOURCE_RANGE_REDUCED};
    src.width = 64; dst.width = 64;
    expect(run(&c, &pipe) == VA_STATUS_ERROR_UNIMPLEMENTED && !c.vpp_q.valid,
           "rga3-try-fmt-clamping-does-not-count-as-size-support");
    src.width = dst.width = 128;
    int old_controls = control_calls;
    expect(run(&c, &pipe) == VA_STATUS_SUCCESS && !strcmp(c.device_path, node) && !c.vpp_behaviors &&
           !c.vpp_controls && control_calls == old_controls && nv12_data[0] == 0x33 && nv12_data[16384] == 0x44,
           "cold-rga3-switch-maps-real-queues-without-unsupported-zero-controls");
    int old_formats = formats, old_tries = tries;
    expect(run(&c, &pipe) == VA_STATUS_SUCCESS && formats == old_formats && tries == old_tries &&
           control_calls == old_controls, "warm-rga3-frame-reuses-negotiated-device-and-queues");
    pipe.rotation_state = VA_ROTATION_90;
    expect(run(&c, &pipe) == VA_STATUS_ERROR_UNIMPLEMENTED && !c.vpp_q.valid && control_calls == old_controls,
           "rga3-only-rotation-fails-before-starting-hardware");
    vav4l2_m2m_teardown(&c, &c.vpp_q); vav4l2_set_ioctl_hook(NULL); close(c.v4l2_fd);
    free(c.device_path); free(rgb_data); free(nv12_data); unlink(node);
    struct vav4l2_driver_data depth = {.vpp_devices_scanned = 1, .num_vpp_devices = 1};
    depth.vpp_devices[0].input[0] = V4L2_PIX_FMT_P010; depth.vpp_devices[0].num_input = 1;
    depth.vpp_devices[0].output[0] = V4L2_PIX_FMT_Y210; depth.vpp_devices[0].num_output = 1;
    expect(vav4l2_vpp_extra_rt_formats(&depth) == VA_RT_FORMAT_YUV422_10, "ten-bit-RT-advertisement-follows-output-direction");
    depth.vpp_caps_ready = 0; depth.vpp_devices[0].output[0] = V4L2_PIX_FMT_P010;
    expect(vav4l2_vpp_extra_rt_formats(&depth) == VA_RT_FORMAT_YUV420_10, "P010-only-raw-node-advertises-only-P010-depth");
    return failures != 0;
}
