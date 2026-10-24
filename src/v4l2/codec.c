/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>
#include "probe.h"
extern const int vav4l2_has_h264, vav4l2_has_hevc, vav4l2_has_av1, vav4l2_has_vp8, vav4l2_has_mpeg2, vav4l2_has_jpeg, vav4l2_has_vpp;
struct codec_descriptor { enum vav4l2_codec codec; const char *name; uint32_t fourcc; const int *available; uint32_t controls[6]; };
static const struct codec_descriptor codecs[] = {
    {VAV4L2_CODEC_H264, "H.264", V4L2_PIX_FMT_H264_SLICE, &vav4l2_has_h264, {V4L2_CID_STATELESS_H264_SPS, V4L2_CID_STATELESS_H264_PPS, V4L2_CID_STATELESS_H264_SCALING_MATRIX, V4L2_CID_STATELESS_H264_DECODE_PARAMS}},
    {VAV4L2_CODEC_HEVC, "HEVC", V4L2_PIX_FMT_HEVC_SLICE, &vav4l2_has_hevc, {V4L2_CID_STATELESS_HEVC_SPS, V4L2_CID_STATELESS_HEVC_PPS, V4L2_CID_STATELESS_HEVC_SCALING_MATRIX, V4L2_CID_STATELESS_HEVC_DECODE_PARAMS}},
    {VAV4L2_CODEC_AV1, "AV1", V4L2_PIX_FMT_AV1_FRAME, &vav4l2_has_av1, {V4L2_CID_STATELESS_AV1_SEQUENCE, V4L2_CID_STATELESS_AV1_FRAME, V4L2_CID_STATELESS_AV1_TILE_GROUP_ENTRY}},
    {VAV4L2_CODEC_VP8, "VP8", V4L2_PIX_FMT_VP8_FRAME, &vav4l2_has_vp8, {V4L2_CID_STATELESS_VP8_FRAME}},
    {VAV4L2_CODEC_MPEG2, "MPEG-2", V4L2_PIX_FMT_MPEG2_SLICE, &vav4l2_has_mpeg2, {V4L2_CID_STATELESS_MPEG2_SEQUENCE, V4L2_CID_STATELESS_MPEG2_PICTURE, V4L2_CID_STATELESS_MPEG2_QUANTISATION}},
    {VAV4L2_CODEC_JPEG_ENC, "JPEG encode", 0, &vav4l2_has_jpeg, {V4L2_CID_JPEG_COMPRESSION_QUALITY}},
    {VAV4L2_CODEC_VPP, "VPP", 0, &vav4l2_has_vpp, {0}},
};
static const struct codec_descriptor *descriptor(enum vav4l2_codec codec)
{
    for (unsigned i = 0; i < sizeof(codecs) / sizeof(codecs[0]); i++) if (codecs[i].codec == codec) return &codecs[i];
    return NULL;
}
const char *vav4l2_codec_name(enum vav4l2_codec codec) { const struct codec_descriptor *d = descriptor(codec); return d ? d->name : "unknown"; }
int vav4l2_codec_enabled(enum vav4l2_codec codec) { const struct codec_descriptor *d = descriptor(codec); return d && *d->available; }
int vav4l2_codec_coded_fourcc(enum vav4l2_codec codec, uint32_t *fourcc)
{
    const struct codec_descriptor *d = descriptor(codec);
    if (!d || !d->fourcc || !fourcc) return -1;
    *fourcc = d->fourcc; return 0;
}
int vav4l2_codec_controls_usable(int fd, enum vav4l2_codec codec)
{
    const struct codec_descriptor *d = descriptor(codec);
    if (!d || !*d->available || fd < 0) return 0;
    for (unsigned i = 0; i < sizeof(d->controls) / sizeof(d->controls[0]) && d->controls[i]; i++) {
        struct v4l2_query_ext_ctrl q = {.id = d->controls[i]};
        if (vav4l2_xioctl(fd, VIDIOC_QUERY_EXT_CTRL, &q) < 0 || (q.flags & (V4L2_CTRL_FLAG_DISABLED | V4L2_CTRL_FLAG_READ_ONLY))) return 0;
    }
    if (codec == VAV4L2_CODEC_H264 || codec == VAV4L2_CODEC_HEVC) {
        const uint32_t ids[] = {codec == VAV4L2_CODEC_H264 ? V4L2_CID_STATELESS_H264_DECODE_MODE : V4L2_CID_STATELESS_HEVC_DECODE_MODE,
            codec == VAV4L2_CODEC_H264 ? V4L2_CID_STATELESS_H264_START_CODE : V4L2_CID_STATELESS_HEVC_START_CODE};
        const int values[] = {codec == VAV4L2_CODEC_H264 ? V4L2_STATELESS_H264_DECODE_MODE_FRAME_BASED : V4L2_STATELESS_HEVC_DECODE_MODE_FRAME_BASED,
            codec == VAV4L2_CODEC_H264 ? V4L2_STATELESS_H264_START_CODE_ANNEX_B : V4L2_STATELESS_HEVC_START_CODE_ANNEX_B};
        for (unsigned i = 0; i < 2; i++) {
            struct v4l2_queryctrl q = {.id = ids[i]}; struct v4l2_querymenu menu = {.id = ids[i], .index = values[i]};
            if (vav4l2_xioctl(fd, VIDIOC_QUERYCTRL, &q) < 0 || (q.flags & V4L2_CTRL_FLAG_DISABLED) || q.minimum > values[i] || q.maximum < values[i] || vav4l2_xioctl(fd, VIDIOC_QUERYMENU, &menu) < 0) return 0;
        }
    }
    return 1;
}
int vav4l2_node_is_m2m(const struct v4l2_capability *cap)
{
    if (!cap) return 0;
    uint32_t caps = cap->capabilities & V4L2_CAP_DEVICE_CAPS ? cap->device_caps : cap->capabilities;
    return (caps & (V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING)) == (V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING);
}

uint32_t vav4l2_stateful_fourcc(enum vav4l2_codec codec)
{
    switch (codec) {
    case VAV4L2_CODEC_H264: return V4L2_PIX_FMT_H264;
    case VAV4L2_CODEC_HEVC: return V4L2_PIX_FMT_HEVC;
    default: return 0;
    }
}
int vav4l2_stateful_usable(int fd, enum vav4l2_codec codec)
{
    if (!vav4l2_stateful_fourcc(codec) || !vav4l2_codec_enabled(codec)) return 0;
    struct v4l2_format format = {.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE};
    if (vav4l2_xioctl(fd, VIDIOC_G_FMT, &format) < 0) return 0;
    format.fmt.pix_mp.pixelformat = vav4l2_stateful_fourcc(codec); format.fmt.pix_mp.num_planes = 1;
    if (vav4l2_xioctl(fd, VIDIOC_S_FMT, &format) < 0 || format.fmt.pix_mp.pixelformat != vav4l2_stateful_fourcc(codec) ||
        format.fmt.pix_mp.num_planes != 1) return 0;
    struct v4l2_queryctrl profile = {.id = codec == VAV4L2_CODEC_H264 ? V4L2_CID_MPEG_VIDEO_H264_PROFILE : V4L2_CID_MPEG_VIDEO_HEVC_PROFILE};
    if (vav4l2_xioctl(fd, VIDIOC_QUERYCTRL, &profile) < 0 || (profile.flags & V4L2_CTRL_FLAG_DISABLED) ||
        profile.minimum < 0 || profile.maximum >= 32 || profile.minimum > profile.maximum) return 0;
    unsigned mask = codec == VAV4L2_CODEC_H264 ? (1u << V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE) |
        (1u << V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_BASELINE) | (1u << V4L2_MPEG_VIDEO_H264_PROFILE_MAIN) |
        (1u << V4L2_MPEG_VIDEO_H264_PROFILE_HIGH) : 1u << V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN;
    int found = 0;
    for (int i = profile.minimum; i <= profile.maximum; i++) if (mask & (1u << i)) {
        struct v4l2_querymenu menu = {.id = profile.id, .index = i};
        if (vav4l2_xioctl(fd, VIDIOC_QUERYMENU, &menu) == 0) found = 1;
    }
    if (!found) return 0;
    struct v4l2_event_subscription subscription = {.type = V4L2_EVENT_SOURCE_CHANGE};
    struct v4l2_decoder_cmd command = {.cmd = V4L2_DEC_CMD_STOP};
    if (vav4l2_xioctl(fd, VIDIOC_SUBSCRIBE_EVENT, &subscription) < 0) return 0;
    int valid = vav4l2_xioctl(fd, VIDIOC_TRY_DECODER_CMD, &command) == 0;
    if (vav4l2_xioctl(fd, VIDIOC_UNSUBSCRIBE_EVENT, &subscription) < 0) return 0;
    return valid;
}
/* Select the implemented queue protocol from this opened node's formats. */
int vav4l2_decoder_select(int fd, enum vav4l2_codec codec, uint32_t *format)
{
    uint32_t formats[VAV4L2_MAX_FORMATS], request = 0, stream = vav4l2_stateful_fourcc(codec);
    int count = vav4l2_enum_output_fourccs(fd, formats, VAV4L2_MAX_FORMATS);
    if (!format || vav4l2_codec_coded_fourcc(codec, &request) < 0) return -1;
    for (int i = 0; i < count; i++) if (formats[i] == request && vav4l2_codec_controls_usable(fd, codec) && vav4l2_queue_has_requests(fd)) { *format = request; return 0; }
    for (int i = 0; i < count; i++) if (stream && formats[i] == stream && vav4l2_stateful_usable(fd, codec)) { *format = stream; return 1; }
    errno = ENOTSUP; return -1;
}
