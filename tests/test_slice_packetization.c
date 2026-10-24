#include <stdio.h>
#include <string.h>
#include <limits.h>
#include "va_v4l2.h"
#include "h264422_annexb.h"

static int failures, controls, submits;
static const uint8_t *expected;
static size_t expected_size;
static struct vav4l2_context *reconfig_context;
static uint8_t *replacement_output;
static void expect(int ok, const char *tag)
{
    if (!ok) { fprintf(stderr, "FAIL %s\n", tag); failures++; }
}
static int control_ioctl(int fd, unsigned long request, void *arg)
{
    (void)fd; (void)request; (void)arg; controls++;
    if (reconfig_context) {
        reconfig_context->output_buf_ptr[0] = replacement_output; reconfig_context->output_buf_size = 64; reconfig_context = NULL;
    }
    return 0;
}
int __wrap_vav4l2_ensure_capture(struct vav4l2_context *ctx, int width, int height, uint32_t fourcc)
{
    (void)ctx; (void)width; (void)height; (void)fourcc; return 0;
}
int __wrap_vav4l2_decode_submit(struct vav4l2_context *ctx, int index, uint32_t bytes, uint64_t timestamp)
{
    (void)timestamp; submits++;
    expect(bytes == expected_size && !memcmp(ctx->output_buf_ptr[index], expected, expected_size), "submitted-byte-ranges-and-fragments");
    vav4l2_out_pool_push(ctx, index);
    return 0;
}
union slice_param { VASliceParameterBufferH264 h264; VASliceParameterBufferHEVC hevc; VASliceParameterBufferVP8 vp8; VASliceParameterBufferMPEG2 mpeg2; };
static void set_param(union slice_param *p, unsigned offset, unsigned size, unsigned flag)
{
    VASliceParameterBufferBase base = {.slice_data_size = size, .slice_data_offset = offset, .slice_data_flag = flag};
    memset(p, 0, sizeof(*p)); memcpy(p, &base, sizeof(base));
}
static struct vav4l2_buffer params(union slice_param *p, unsigned count)
{
    return (struct vav4l2_buffer){.type = VASliceParameterBufferType, .size = sizeof(*p),
        .num_elements = count, .allocated_elements = count, .data = p};
}
static struct vav4l2_buffer data(void *p, unsigned size)
{
    return (struct vav4l2_buffer){.type = VASliceDataBufferType, .size = size,
        .num_elements = 1, .allocated_elements = 1, .data = p};
}
static void packet(enum vav4l2_codec codec, struct vav4l2_buffer **buffers, int n,
                   const uint8_t *want, size_t size, VAStatus status, size_t capacity)
{
    uint8_t out[1024], original[1024]; size_t bytes = 99, measured = 99;
    memset(out, 0xa5, sizeof(out)); memcpy(original, out, sizeof(out));
    expect(vav4l2_packetize_slices(codec, buffers, n, NULL, capacity, &measured) == status, "preflight-status");
    expect(vav4l2_packetize_slices(codec, buffers, n, out, capacity, &bytes) == status, "packet-status");
    if (status == VA_STATUS_SUCCESS) {
        expect(bytes == size && measured == size && !memcmp(out, want, size), "exact-packet-bytes");
        expect(!memcmp(out + size, original + size, sizeof(out) - size), "packet-output-tail-untouched");
    } else {
        expect(!bytes && !measured && !memcmp(out, original, sizeof(out)), "rejected-picture-does-not-write-output");
    }
}
static void ranges(enum vav4l2_codec codec)
{
    uint8_t payload[] = {0xee, 0x65, 0xa1, 0xee, 0, 0, 1, 0x41, 0xb2, 0xee};
    uint8_t h264[] = {0, 0, 1, 0x65, 0xa1, 0, 0, 1, 0x41, 0xb2};
    uint8_t hevc[] = {0, 0, 0, 1, 0x65, 0xa1, 0, 0, 1, 0x41, 0xb2};
    union slice_param p[2]; set_param(&p[0], 1, 2, VA_SLICE_DATA_FLAG_ALL); set_param(&p[1], 4, 5, VA_SLICE_DATA_FLAG_ALL);
    struct vav4l2_buffer pb = params(p, 2), db = data(payload, sizeof(payload));
    struct vav4l2_buffer *buffers[] = {&pb, &db};
    const uint8_t *want = codec == VAV4L2_CODEC_H264 ? h264 : hevc;
    size_t size = codec == VAV4L2_CODEC_H264 ? sizeof(h264) : sizeof(hevc);
    packet(codec, buffers, 2, want, size, VA_STATUS_SUCCESS, size);
    packet(codec, buffers, 2, NULL, 0, VA_STATUS_ERROR_OPERATION_FAILED, size - 1);
    set_param(&p[1], UINT32_MAX, 1, 0);
    packet(codec, buffers, 2, NULL, 0, VA_STATUS_ERROR_INVALID_PARAMETER, 1024);
    set_param(&p[1], sizeof(payload), 1, 0);
    packet(codec, buffers, 2, NULL, 0, VA_STATUS_ERROR_INVALID_PARAMETER, 1024);
    set_param(&p[1], 0, 0, 0);
    packet(codec, buffers, 2, NULL, 0, VA_STATUS_ERROR_INVALID_PARAMETER, 1024);
    set_param(&p[1], 0, 1, 8);
    packet(codec, buffers, 2, NULL, 0, VA_STATUS_ERROR_INVALID_PARAMETER, 1024);
    pb.num_elements = 0;
    packet(codec, buffers, 2, NULL, 0, VA_STATUS_ERROR_INVALID_PARAMETER, 1024);
    pb.num_elements = 3;
    packet(codec, buffers, 2, NULL, 0, VA_STATUS_ERROR_INVALID_PARAMETER, 1024);
    pb.num_elements = 1; pb.size = sizeof(VASliceParameterBufferBase);
    packet(codec, buffers, 2, NULL, 0, VA_STATUS_ERROR_INVALID_PARAMETER, 1024);
}
static void fragments(enum vav4l2_codec codec)
{
    uint8_t nal[] = {0, 0, 0, 1, 0x65, 0xaa, 0, 0, 1, 0xbb};
    union slice_param p[3]; struct vav4l2_buffer b[6]; struct vav4l2_buffer *buffers[6];
    for (int prefix = 0; prefix <= 4; prefix++) {
        if (prefix == 1 || prefix == 2) continue;
        uint8_t *src = nal + (prefix ? 4 - prefix : 4);
        size_t size = sizeof(nal) - (size_t)(src - nal);
        for (unsigned cut = 1; cut < size - 1; cut++) {
            unsigned offsets[] = {0, cut, cut + 1}, sizes[] = {cut, 1, size - cut - 1};
            unsigned flags[] = {VA_SLICE_DATA_FLAG_BEGIN, VA_SLICE_DATA_FLAG_MIDDLE, VA_SLICE_DATA_FLAG_END};
            for (int i = 0; i < 3; i++) {
                set_param(&p[i], 0, sizes[i], flags[i]); b[i * 2] = params(&p[i], 1);
                b[i * 2 + 1] = data(src + offsets[i], sizes[i]);
                buffers[i * 2] = &b[i * 2]; buffers[i * 2 + 1] = &b[i * 2 + 1];
            }
            uint8_t want[32]; size_t bytes = 0;
            if (!prefix) {
                unsigned len = codec == VAV4L2_CODEC_H264 ? 3 : 4;
                memcpy(want, nal + 4 - len, len); bytes = len;
            }
            memcpy(want + bytes, src, size); bytes += size;
            packet(codec, buffers, 6, want, bytes, VA_STATUS_SUCCESS, bytes);
            packet(codec, buffers, 4, NULL, 0, VA_STATUS_ERROR_INVALID_PARAMETER, 1024);
            packet(codec, buffers + 2, 4, NULL, 0, VA_STATUS_ERROR_INVALID_PARAMETER, 1024);
            set_param(&p[1], 0, 1, VA_SLICE_DATA_FLAG_ALL);
            packet(codec, buffers, 6, NULL, 0, VA_STATUS_ERROR_INVALID_PARAMETER, 1024);
        }
    }
}
static void association(enum vav4l2_codec codec)
{
    uint8_t a[] = {0xee, 0x65, 0xaa}, b[] = {0xbb, 0x41, 0xcc, 0xdd};
    union slice_param p[2]; set_param(&p[0], 1, 2, 0); set_param(&p[1], 1, 1, 0);
    struct vav4l2_buffer pb[] = {params(&p[0], 1), params(&p[1], 1)}, db[] = {data(a, sizeof(a)), data(b, sizeof(b))};
    struct vav4l2_buffer *buffers[] = {&pb[0], &db[0], &pb[1], &db[1]};
    uint8_t want[16]; unsigned prefix = codec == VAV4L2_CODEC_H264 ? 3 : 4; size_t bytes = 0;
    uint8_t start[] = {0, 0, 0, 1};
    memcpy(want, start + 4 - prefix, prefix); bytes += prefix; memcpy(want + bytes, a + 1, 2); bytes += 2;
    memcpy(want + bytes, start + 4 - prefix, prefix); bytes += prefix; want[bytes++] = b[1];
    packet(codec, buffers, 4, want, bytes, VA_STATUS_SUCCESS, bytes);
    packet(codec, buffers, 3, NULL, 0, VA_STATUS_ERROR_INVALID_PARAMETER, 1024);
    packet(codec, buffers + 1, 3, NULL, 0, VA_STATUS_ERROR_INVALID_PARAMETER, 1024);
    union slice_param many[40]; uint8_t input[40], output[200]; size_t size = 0;
    for (unsigned i = 0; i < 40; i++) {
        input[i] = 0x41 + i; set_param(&many[i], i, 1, 0);
        memcpy(output + size, start + 4 - prefix, prefix); size += prefix; output[size++] = input[i];
    }
    struct vav4l2_buffer mp = params(many, 40), md = data(input, sizeof(input));
    struct vav4l2_buffer *mb[] = {&mp, &md};
    packet(codec, mb, 2, output, size, VA_STATUS_SUCCESS, size);
}
static void translate(enum vav4l2_codec codec)
{
    struct vav4l2_driver_data dd = {0}; uint8_t out[64], payload[] = {0xee, 0x65, 0xb8, 0x40, 0x80, 0xff};
    VAPictureParameterBufferH264 h264 = {0}; VAPictureParameterBufferHEVC hevc = {0};
    uint8_t hevc_payload[] = {0xee, 0x26, 0x01, 0xa0, 0x80};
    hevc.slice_parsing_fields.bits.RapPicFlag = hevc.slice_parsing_fields.bits.IdrPicFlag = 1;
    for (int i = 0; i < 16; i++) h264.ReferenceFrames[i].flags = VA_PICTURE_H264_INVALID;
    for (int i = 0; i < 15; i++) hevc.ReferenceFrames[i].flags = VA_PICTURE_HEVC_INVALID;
    h264.seq_fields.bits.frame_mbs_only_flag = 1;
    h264.pic_fields.bits.reference_pic_flag = 1;
    struct vav4l2_buffer picture = {.type = VAPictureParameterBufferType,
        .data = codec == VAV4L2_CODEC_H264 ? (void *)&h264 : (void *)&hevc};
    union slice_param p; set_param(&p, 1, 4, 0);
    if (codec == VAV4L2_CODEC_H264) p.h264.slice_type = 2;
    struct vav4l2_buffer pb = params(&p, 1), db = data(codec == VAV4L2_CODEC_HEVC ? hevc_payload : payload, codec == VAV4L2_CODEC_HEVC ? sizeof(hevc_payload) : sizeof(payload));
    struct vav4l2_buffer *buffers[] = {&picture, &pb, &db};
    struct vav4l2_context ctx = {.codec = codec, .profile = VAProfileH264High, .driver_data = &dd,
        .v4l2_fd = 77, .request_fd = 78, .streamed = 1, .n_free_out = 1, .output_buf_size = sizeof(out)};
    ctx.output_buf_ptr[0] = out;
    uint8_t want[] = {0, 0, 0, 1, 0x65, 0xb8, 0x40, 0x80};
    uint8_t hevc_want[] = {0, 0, 0, 1, 0x26, 0x01, 0xa0, 0x80};
    expected = codec == VAV4L2_CODEC_H264 ? want + 1 : hevc_want; expected_size = codec == VAV4L2_CODEC_H264 ? 7 : 8;
    VAStatus (*run)(struct vav4l2_context *, struct vav4l2_buffer **, int) =
        codec == VAV4L2_CODEC_H264 ? vav4l2_h264_translate : vav4l2_hevc_translate;
    uint8_t resized[64]; memset(resized, 0xa5, sizeof(resized));
    if (codec == VAV4L2_CODEC_H264) { reconfig_context = &ctx; replacement_output = resized; ctx.output_buf_size = 1; }
    vav4l2_set_ioctl_hook(control_ioctl); controls = submits = 0;
    expect(run(&ctx, buffers, 3) == VA_STATUS_SUCCESS && submits == 1 && controls > 0, "translator-uses-packetizer");
    set_param(&p, UINT32_MAX, 2, 0); controls = submits = 0;
    expect(run(&ctx, buffers, 3) == VA_STATUS_ERROR_INVALID_PARAMETER && !submits && !controls && ctx.n_free_out == 1,
           "invalid-range-rejected-before-controls-or-pool-changes");
    set_param(&p, 1, 4, 0); ctx.output_buf_size = expected_size - 1; memset(out, 0xa5, sizeof(out));
    if (codec == VAV4L2_CODEC_H264) p.h264.slice_type = 2;
    expect(run(&ctx, buffers, 3) == VA_STATUS_ERROR_OPERATION_FAILED && !submits && ctx.n_free_out == 1 && out[0] == 0xa5,
           "output-overflow-returns-unsubmitted-slot");
    ctx.output_buf_size = sizeof(out); ctx.output_buf_ptr[0] = NULL;
    expect(run(&ctx, buffers, 3) == VA_STATUS_ERROR_OPERATION_FAILED && !submits && ctx.n_free_out == 1,
        "missing-output-mapping-cannot-be-misread-as-successful-size-only-preflight");
    vav4l2_set_ioctl_hook(NULL);
}
static void vp8_fragments(void)
{
    uint8_t payload[] = {0xee, 0, 0, 1, 0xaa, 0xbb, 0xff};
    union slice_param p[3]; struct vav4l2_buffer b[4];
    set_param(&p[0], 1, 5, VA_SLICE_DATA_FLAG_ALL);
    b[0] = params(p, 1); b[1] = data(payload, sizeof(payload));
    struct vav4l2_buffer *whole[] = {&b[0], &b[1]};
    packet(VAV4L2_CODEC_VP8, whole, 2, payload + 1, 5, VA_STATUS_SUCCESS, 5);
    set_param(&p[0], 1, 1, VA_SLICE_DATA_FLAG_BEGIN); set_param(&p[1], 2, 1, VA_SLICE_DATA_FLAG_MIDDLE);
    set_param(&p[2], 3, 3, VA_SLICE_DATA_FLAG_END); b[0] = params(p, 3);
    packet(VAV4L2_CODEC_VP8, whole, 2, payload + 1, 5, VA_STATUS_SUCCESS, 5);
    packet(VAV4L2_CODEC_VP8, whole, 2, NULL, 0, VA_STATUS_ERROR_OPERATION_FAILED, 4);
    b[0] = params(p, 1); b[2] = params(&p[1], 2); b[3] = b[1];
    struct vav4l2_buffer *split[] = {&b[0], &b[1], &b[2], &b[3]};
    packet(VAV4L2_CODEC_VP8, split, 4, payload + 1, 5, VA_STATUS_SUCCESS, 5);
    set_param(&p[0], 1, 1, VA_SLICE_DATA_FLAG_ALL); set_param(&p[1], 2, 1, VA_SLICE_DATA_FLAG_ALL);
    b[0] = params(p, 2);
    packet(VAV4L2_CODEC_VP8, whole, 2, NULL, 0, VA_STATUS_ERROR_INVALID_PARAMETER, 16);
}

static void mpeg2_ranges(void)
{
    uint8_t payload[] = {0xee, 0, 0, 1, 1, 0x33, 0xaa, 0xff, 0, 0, 1, 2, 0x44, 0xbb, 0xee};
    uint8_t wanted[] = {0, 0, 1, 1, 0x33, 0xaa, 0, 0, 1, 2, 0x44, 0xbb};
    union slice_param p[3]; set_param(&p[0], 1, 6, VA_SLICE_DATA_FLAG_ALL); set_param(&p[1], 8, 6, VA_SLICE_DATA_FLAG_ALL);
    struct vav4l2_buffer pb = params(p, 2), db = data(payload, sizeof(payload));
    struct vav4l2_buffer *buffers[] = {&pb, &db};
    packet(VAV4L2_CODEC_MPEG2, buffers, 2, wanted, sizeof(wanted), VA_STATUS_SUCCESS, sizeof(wanted));
    packet(VAV4L2_CODEC_MPEG2, buffers, 2, NULL, 0, VA_STATUS_ERROR_OPERATION_FAILED, sizeof(wanted) - 1);
    set_param(&p[0], 1, 1, VA_SLICE_DATA_FLAG_BEGIN); set_param(&p[1], 2, 1, VA_SLICE_DATA_FLAG_MIDDLE);
    set_param(&p[2], 3, 4, VA_SLICE_DATA_FLAG_END); pb = params(p, 3);
    packet(VAV4L2_CODEC_MPEG2, buffers, 2, payload + 1, 6, VA_STATUS_SUCCESS, 6);
    pb.num_elements = 2;
    packet(VAV4L2_CODEC_MPEG2, buffers, 2, NULL, 0, VA_STATUS_ERROR_INVALID_PARAMETER, 64);
    set_param(&p[0], UINT32_MAX, 6, 0); pb.num_elements = 1;
    packet(VAV4L2_CODEC_MPEG2, buffers, 2, NULL, 0, VA_STATUS_ERROR_INVALID_PARAMETER, 64);
    set_param(&p[0], 1, 6, 0); pb = params(p, 1);
    struct vav4l2_driver_data dd = {0}; uint8_t output[64];
    VAPictureParameterBufferMPEG2 pic = {.horizontal_size = 16, .vertical_size = 16, .picture_coding_type = 1};
    struct vav4l2_buffer picture = {.type = VAPictureParameterBufferType, .data = &pic};
    struct vav4l2_buffer *submission[] = {&picture, &pb, &db};
    struct vav4l2_context ctx = {.driver_data = &dd, .codec = VAV4L2_CODEC_MPEG2, .v4l2_fd = 77, .request_fd = 78,
        .streamed = 1, .n_free_out = 1, .output_buf_size = sizeof(output)};
    ctx.output_buf_ptr[0] = output; expected = payload + 1; expected_size = 6;
    controls = submits = 0; vav4l2_set_ioctl_hook(control_ioctl);
    expect(vav4l2_mpeg2_translate(&ctx, submission, 3) == VA_STATUS_SUCCESS && submits == 1,
        "mpeg2-translator-submits-only-the-declared-slice-range");
    set_param(&p[0], sizeof(payload), 1, 0); controls = submits = 0;
    expect(vav4l2_mpeg2_translate(&ctx, submission, 3) == VA_STATUS_ERROR_INVALID_PARAMETER && !controls && !submits && ctx.n_free_out == 1,
        "mpeg2-invalid-range-never-touches-controls-or-queues");
    vav4l2_set_ioctl_hook(NULL);
}

int main(void)
{
    for (unsigned prefix = 3; prefix <= 4; prefix++) for (unsigned tail = 1; tail <= 7; tail++) {
        unsigned char stream[12] = {0}, *slices[2] = {0}; int sizes[2] = {0};
        stream[prefix - 1] = 1; stream[prefix] = 0x65;
        for (unsigned i = 1; i < tail; i++) stream[prefix + i] = 0x80 + i;
        expect(h264422_slices(stream, prefix + tail, slices, sizes, 2) == 1 &&
               slices[0] == stream + prefix && sizes[0] == (int)tail &&
               slices[0][tail - 1] == stream[prefix + tail - 1], "client-annexb-keeps-entire-final-nal-through-eof");
    }
    unsigned char stream[] = {0,0,0,1,0x67,0x42, 0,0,1,0x65,0x81,0x82, 0,0,0,1,0x41,0x91,0x92,0x93};
    unsigned char *nals[2]; int sizes[2];
    expect(h264422_slices(stream, sizeof(stream), nals, sizes, 2) == 2 &&
           nals[0] == stream + 9 && sizes[0] == 3 && nals[1] == stream + 16 && sizes[1] == 4,
           "client-annexb-mixed-prefixes-exclude-start-code-and-nonslice-nal");
    expect(h264422_slices(stream, 3, nals, sizes, 2) == 0 &&
           h264422_slices(stream, sizeof(stream), nals, sizes, 0) == 0,
           "client-annexb-short-prefix-and-zero-capacity");
    for (int codec = VAV4L2_CODEC_H264; codec <= VAV4L2_CODEC_HEVC; codec++) {
        ranges(codec); fragments(codec); association(codec); translate(codec);
    }
    size_t bytes = 1;
    expect(vav4l2_packetize_slices(VAV4L2_CODEC_AV1, NULL, 0, NULL, 0, &bytes) == VA_STATUS_ERROR_INVALID_PARAMETER && !bytes,
           "packetizer-does-not-extend-av1-or-vp9-scope");
    vp8_fragments();
    mpeg2_ranges();
    return failures != 0;
}
