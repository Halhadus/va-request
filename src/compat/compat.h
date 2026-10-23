/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef VAREQ_COMPAT_H
#define VAREQ_COMPAT_H
#include <stddef.h>
#include <stdint.h>
#include <linux/videodev2.h>

#define VAREQ_COMPAT_API_VERSION 1u
#define VAREQ_COMPAT_MAX_MATCHES 8u
#define VAREQ_COMPAT_SWAP_MIRROR_AXES 1u
#define VAREQ_COMPAT_STREAM_BEFORE_QUEUE 2u
#define VAREQ_COMPAT_EXACT_IMPORT_SPAN 4u
#define VAREQ_COMPAT_KEEP_DMA_STREAMS 8u

struct vareq_compat_identity { char driver[17], card[33], bus_info[33]; uint32_t version; };
struct vareq_compat_job { uint32_t src, dst, src_quantization, dst_quantization; int src_rgb, dst_rgb, rotate, hflip, vflip, crop_offset, compose; };
struct vareq_storage_ops {
    int (*export_capture)(const char *node, struct v4l2_format *format, unsigned bytes, unsigned rows, int exact, const char *tag);
};
struct vareq_compat_module {
    unsigned api_version;
    const char *name;
    int (*match)(const struct vareq_compat_identity *identity);
    unsigned behaviors;
    /* Negative rejects only this job/provider; nonnegative ranks an eligible path. */
    int (*vpp_score)(const struct vareq_compat_identity *identity, const struct vareq_compat_job *job);
    int (*storage_score)(const struct vareq_compat_identity *identity, unsigned bytes, unsigned rows, int coherent);
    /* Return an owned CLOEXEC FD, or -1 without submitting a processing job. */
    int (*allocate)(const struct vareq_compat_identity *identity, const char *node, unsigned bytes, unsigned rows, int coherent, const struct vareq_storage_ops *ops);
};
struct vareq_compat {
    struct vareq_compat_identity identity;
    const struct vareq_compat_module *modules[VAREQ_COMPAT_MAX_MATCHES];
    unsigned count, behaviors;
};
void vareq_compat_identity(const struct v4l2_capability *cap, struct vareq_compat_identity *identity);
const struct vareq_compat_module *const *vareq_compat_modules(size_t *count);
int vareq_compat_resolve(const struct vareq_compat_identity *identity, struct vareq_compat *result);
int vareq_compat_resolve_modules(const struct vareq_compat_identity *identity, const struct vareq_compat_module *const *modules, size_t count, struct vareq_compat *result);
int vareq_compat_vpp_score(const struct vareq_compat *compat, const struct vareq_compat_job *job);
#endif
