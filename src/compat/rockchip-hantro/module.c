/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <string.h>
#include "compat/compat.h"
static int jpeg_match(const struct vav4l2_compat_identity *id)
{
    return !strcmp(id->driver, "hantro-vpu") && !strcmp(id->card, "rockchip,rk3588-vepu121-enc");
}
static int jpeg_score(const struct vav4l2_compat_identity *id, unsigned bytes, unsigned rows, int coherent)
{
    (void)id; (void)bytes; (void)rows; (void)coherent;
    return 40;
}
static int jpeg_allocate(const struct vav4l2_compat_identity *id, const char *node, unsigned bytes, unsigned rows, int coherent, const struct vav4l2_storage_ops *ops)
{
    (void)id; (void)coherent;
    struct v4l2_format fmt = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE};
    fmt.fmt.pix_mp.width = fmt.fmt.pix_mp.height = 128;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_JPEG;
    fmt.fmt.pix_mp.field = V4L2_FIELD_NONE; fmt.fmt.pix_mp.num_planes = 1;
    fmt.fmt.pix_mp.plane_fmt[0].sizeimage = (uint64_t)bytes * rows;
    return ops->export_capture(node, &fmt, bytes, rows, 1, "exported");
}
const struct vav4l2_compat_module vav4l2_compat_hantro_jpeg = {
    .api_version = VAV4L2_COMPAT_API_VERSION, .name = "rockchip-hantro-jpeg", .match = jpeg_match,
    .storage_score = jpeg_score, .allocate = jpeg_allocate
};
