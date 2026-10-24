/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <limits.h>
#include <string.h>
#include <unistd.h>
#include "compat/compat.h"

static int rga2_match(const struct vav4l2_compat_identity *id)
{
    return !strcmp(id->driver, "rockchip-rga") && (!strcmp(id->card, "rga2") || !strcmp(id->card, "rockchip-rga"));
}
static int rga3_match(const struct vav4l2_compat_identity *id) { return !strcmp(id->driver, "rockchip-rga") && !strcmp(id->card, "rga3"); }
static int supported_range(const struct vav4l2_compat_job *job)
{
    return (!job->src_quantization || job->src_quantization == (job->src_rgb ? V4L2_QUANTIZATION_FULL_RANGE : V4L2_QUANTIZATION_LIM_RANGE)) &&
        (!job->dst_quantization || job->dst_quantization == (job->dst_rgb ? V4L2_QUANTIZATION_FULL_RANGE : V4L2_QUANTIZATION_LIM_RANGE));
}
static int rga2_vpp(const struct vav4l2_compat_identity *id, const struct vav4l2_compat_job *job)
{
    (void)id;
    /* Measured limited-range RGB-to-YUV pixels do not match requested CSC. */
    return !supported_range(job) || (job->src_rgb && !job->dst_rgb) ? -1 : 10;
}
static int rga3_vpp(const struct vav4l2_compat_identity *id, const struct vav4l2_compat_job *job)
{
    (void)id;
    /* Current command builder implements origin cropping and full output only. */
    if (!supported_range(job) || job->crop_offset || job->compose) return -1;
    return job->src_rgb != job->dst_rgb ? 90 : 0;
}
static int rga2_storage_score(const struct vav4l2_compat_identity *id, unsigned bytes, unsigned rows, int coherent)
{
    (void)id; (void)bytes; (void)rows;
    return coherent ? -1 : 80;
}

/* RGA2's own dma-sg EXPBUF storage passed the differential transport matrix;
 * imported coherent storage did not. Use CAPTURE allocation so CPU readers
 * retain the exporter's device-write invalidation path. SYNC START/END still
 * brackets every CPU access; this is allocation only, never a queued job.
 * Allocation geometry describes storage, not the public image/plane layout. */
static int rga2_allocate(const struct vav4l2_compat_identity *identity, const char *node, unsigned bytes, unsigned rows, int coherent, const struct vav4l2_storage_ops *ops)
{
    (void)identity; (void)coherent;
    static const uint32_t formats[] = {V4L2_PIX_FMT_NV12, V4L2_PIX_FMT_XBGR32, V4L2_PIX_FMT_YUYV};
    uint64_t need = (uint64_t)bytes * rows;
    long page = sysconf(_SC_PAGESIZE); if (page <= 0) return -1;
    uint64_t limit = ((need + page - 1) / page) * page;
    for (unsigned i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
        unsigned bpp = i == 1 ? 4 : i == 2 ? 2 : 1;
        uint64_t width = ((uint64_t)bytes + bpp - 1) / bpp;
        if (width < 34) width = 34;
        if (width > 8192) width = 8192;
        width = (width + 3) & ~3ull;
        uint64_t preferred = width;
        for (unsigned attempt = 0; attempt < 2; attempt++) {
            if (attempt) {
                /* Storage width need not equal the consumer's image pitch. The
                 * minimum RGB width can otherwise turn128x256 bytes into36x228
                 * RGB pixels (32832 bytes), missing an exact64x128 allocation. */
                uint64_t best = UINT64_MAX, distance = UINT64_MAX; unsigned selected = 0;
                for (unsigned candidate = 36; candidate <= 8192; candidate += 4) {
                    if (candidate == preferred) continue;
                    uint64_t line = i ? candidate * bpp : candidate * 3 / 2;
                    uint64_t height = (need + line - 1) / line;
                    if (height < 34) height = 34;
                    height = (height + 1) & ~1ull;
                    uint64_t size = line * height, delta = candidate > preferred ? candidate - preferred : preferred - candidate;
                    if (height <= 8192 && size <= limit && (size < best || (size == best && delta < distance))) {
                        best = size; distance = delta; selected = candidate;
                    }
                }
                if (!selected) break;
                width = selected;
            }
            uint64_t row = i ? width * bpp : width * 3 / 2;
            uint64_t height = (need + row - 1) / row;
            if (height < 34) height = 34;
            height = (height + 1) & ~1ull;
            if (height > 8192) continue;
            struct v4l2_format fmt = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE};
            fmt.fmt.pix_mp.width = width; fmt.fmt.pix_mp.height = height;
            fmt.fmt.pix_mp.pixelformat = formats[i]; fmt.fmt.pix_mp.num_planes = 1;
            fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
            int fd = ops->export_capture(node, &fmt, bytes, rows, 1, "exported-rga2-capture");
            if (fd >= 0) return fd;
        }
    }
    return -1;
}

static void rga3_format(unsigned bytes, unsigned rows, struct v4l2_format *fmt)
{
    uint64_t need = (uint64_t)bytes * rows;
    unsigned width = bytes < 128 ? 128 : bytes > 4096 ? 4096 : bytes;
    width = (width + 15) & ~15u;
    uint64_t height = (need + width + width / 2 - 1) / (width + width / 2);
    if (height < 128) height = 128;
    height = (height + 15) & ~15ull;
    *fmt = (struct v4l2_format){.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE};
    fmt->fmt.pix_mp.width = width; fmt->fmt.pix_mp.height = height; fmt->fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    fmt->fmt.pix_mp.field = V4L2_FIELD_NONE; fmt->fmt.pix_mp.num_planes = 1;
}
static int rga3_storage_score(const struct vav4l2_compat_identity *id, unsigned bytes, unsigned rows, int coherent)
{
    (void)id; (void)coherent;
    long page = sysconf(_SC_PAGESIZE); if (page <= 0) return -1;
    struct v4l2_format fmt; rga3_format(bytes, rows, &fmt);
    uint64_t need = (uint64_t)bytes * rows;
    return (uint64_t)fmt.fmt.pix_mp.width * fmt.fmt.pix_mp.height * 3 / 2 <= ((need + page - 1) / page) * page ? 60 : 20;
}
static int rga3_allocate(const struct vav4l2_compat_identity *id, const char *node, unsigned bytes, unsigned rows, int coherent, const struct vav4l2_storage_ops *ops)
{
    (void)id; (void)coherent;
    struct v4l2_format fmt; rga3_format(bytes, rows, &fmt);
    return ops->export_capture(node, &fmt, bytes, rows, 0, "exported");
}
const struct vav4l2_compat_module vav4l2_compat_rga2 = {
    .api_version = VAV4L2_COMPAT_API_VERSION, .name = "rockchip-rga2", .match = rga2_match,
    .behaviors = VAV4L2_COMPAT_SWAP_MIRROR_AXES | VAV4L2_COMPAT_STREAM_BEFORE_QUEUE | VAV4L2_COMPAT_EXACT_IMPORT_SPAN | VAV4L2_COMPAT_KEEP_DMA_STREAMS,
    .vpp_score = rga2_vpp, .storage_score = rga2_storage_score, .allocate = rga2_allocate
};
const struct vav4l2_compat_module vav4l2_compat_rga3 = {
    .api_version = VAV4L2_COMPAT_API_VERSION, .name = "rockchip-rga3", .match = rga3_match,
    .vpp_score = rga3_vpp, .storage_score = rga3_storage_score, .allocate = rga3_allocate
};
