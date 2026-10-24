/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef VAV4L2_COMPAT_H
#define VAV4L2_COMPAT_H
#include <stddef.h>
#include <stdint.h>
#include <linux/videodev2.h>

#define VAV4L2_COMPAT_API_VERSION 2u
#define VAV4L2_COMPAT_MAX_MATCHES 8u
#define VAV4L2_COMPAT_SWAP_MIRROR_AXES 1u
#define VAV4L2_COMPAT_STREAM_BEFORE_QUEUE 2u
#define VAV4L2_COMPAT_EXACT_IMPORT_SPAN 4u
#define VAV4L2_COMPAT_KEEP_DMA_STREAMS 8u

struct vav4l2_compat_identity { char driver[17], card[33], bus_info[33]; uint32_t version; };
struct vav4l2_compat_job { uint32_t src, dst, src_quantization, dst_quantization; int src_rgb, dst_rgb, rotate, hflip, vflip, crop_offset, compose; };
struct vav4l2_storage_ops {
    int (*export_capture)(const char *node, struct v4l2_format *format, unsigned bytes, unsigned rows, int exact, const char *tag);
};
enum vav4l2_compat_codec_direction { VAV4L2_COMPAT_DECODE, VAV4L2_COMPAT_ENCODE };
struct vav4l2_compat_codec { enum vav4l2_compat_codec_direction direction; uint32_t coded_format; int va_profile; unsigned profile_known; };
struct vav4l2_compat_module {
    unsigned api_version;
    const char *name;
    int (*match)(const struct vav4l2_compat_identity *identity);
    unsigned behaviors;
    int (*codec_score)(const struct vav4l2_compat_identity *identity, const struct vav4l2_compat_codec *codec);
    /* Setup/finish own only module state; the caller retains device/queue ownership. */
    int (*codec_setup)(const struct vav4l2_compat_identity *identity, const struct vav4l2_compat_codec *codec, int fd, void **state);
    void (*codec_finish)(const struct vav4l2_compat_identity *identity, const struct vav4l2_compat_codec *codec, int fd, void *state);
    int (*codec_format)(const struct vav4l2_compat_identity *identity, const struct vav4l2_compat_codec *codec, void *state, struct v4l2_format *format);
    int (*codec_controls)(const struct vav4l2_compat_identity *identity, const struct vav4l2_compat_codec *codec, void *state, struct v4l2_ext_controls *controls);
    /* Negative rejects only this job/provider; nonnegative ranks an eligible path. */
    int (*vpp_score)(const struct vav4l2_compat_identity *identity, const struct vav4l2_compat_job *job);
    int (*storage_score)(const struct vav4l2_compat_identity *identity, unsigned bytes, unsigned rows, int coherent);
    /* Return an owned CLOEXEC FD, or -1 without submitting a processing job. */
    int (*allocate)(const struct vav4l2_compat_identity *identity, const char *node, unsigned bytes, unsigned rows, int coherent, const struct vav4l2_storage_ops *ops);
};
struct vav4l2_compat {
    struct vav4l2_compat_identity identity;
    const struct vav4l2_compat_module *modules[VAV4L2_COMPAT_MAX_MATCHES];
    unsigned count, behaviors;
};
struct vav4l2_compat_backend {
    struct vav4l2_compat policy;
    struct vav4l2_compat_codec codec;
    void *state[VAV4L2_COMPAT_MAX_MATCHES];
    unsigned initialized, opened;
};
int vav4l2_compat_codec_score(const struct vav4l2_compat_identity *identity, const struct vav4l2_compat_codec *codec);
int vav4l2_compat_backend_needed(void);
int vav4l2_compat_backend_init(struct vav4l2_compat_backend *backend, const struct vav4l2_compat_identity *identity, const struct vav4l2_compat_codec *codec, int fd);
void vav4l2_compat_backend_finish(struct vav4l2_compat_backend *backend, int fd);
int vav4l2_compat_prepare_format(struct vav4l2_compat_backend *backend, struct v4l2_format *format);
int vav4l2_compat_prepare_controls(struct vav4l2_compat_backend *backend, struct v4l2_ext_controls *controls);
void vav4l2_compat_identity(const struct v4l2_capability *cap, struct vav4l2_compat_identity *identity);
const struct vav4l2_compat_module *const *vav4l2_compat_modules(size_t *count);
int vav4l2_compat_resolve(const struct vav4l2_compat_identity *identity, struct vav4l2_compat *result);
int vav4l2_compat_resolve_modules(const struct vav4l2_compat_identity *identity, const struct vav4l2_compat_module *const *modules, size_t count, struct vav4l2_compat *result);
int vav4l2_compat_vpp_score(const struct vav4l2_compat *compat, const struct vav4l2_compat_job *job);
#endif
