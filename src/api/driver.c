/*
 * v4l2 — VA-API to Linux V4L2 bridge driver
 *
 * Copyright 2026 — MIT License
 *
 * This file implements the libva driver VTable. It translates VA-API calls
 * into capability-checked V4L2 Request API and memory-to-memory operations.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <poll.h>
#include <time.h>

#include <va/va.h>
#include <va/va_backend.h>
#include <va/va_backend_vpp.h>
#include <va/va_drmcommon.h>
#include <va/va_enc_jpeg.h>
#include <drm_fourcc.h>

#include "va_v4l2.h"
#include "memory/gpu_copy.h"
#include "probe.h"
#include "internal.h"

/* libva does not serialize client threads. Callbacks hold this lock while
 * accessing driver state, including synchronous jobs and stateful waits.
 * Timed sync includes lock acquisition in its timeout budget. */
pthread_mutex_t vav4l2_api_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned g_driver_instances;

/* Read once at init: every hot-path debug gate tests this flag instead of
 * rescanning environ per frame. */
int vav4l2_debug;

/* Supported profiles */
static const VAProfile vav4l2_profiles[] = {
    VAProfileH264ConstrainedBaseline,
    VAProfileH264Main,
    VAProfileH264High,
    VAProfileH264High10,
    VAProfileH264High422,
    VAProfileHEVCMain,
    VAProfileHEVCMain10,
    VAProfileVP9Profile0,
    VAProfileVP9Profile2,
    /* Experimental AV1 is opt-in; normal profile queries omit it. */
    VAProfileAV1Profile0,
    VAProfileVP8Version0_3,
    VAProfileMPEG2Simple,
    VAProfileMPEG2Main,
    VAProfileJPEGBaseline,
    VAProfileNone,
};

#define NUM_PROFILES (sizeof(vav4l2_profiles) / sizeof(vav4l2_profiles[0]))

/* RT formats per profile (bitmask) */
static const struct {
    VAProfile profile;
    unsigned int rt_format;
} profile_rt_formats[] = {
    { VAProfileH264ConstrainedBaseline, VA_RT_FORMAT_YUV420 },
    { VAProfileH264Main,       VA_RT_FORMAT_YUV420 },
    { VAProfileH264High,       VA_RT_FORMAT_YUV420 },
    { VAProfileH264High10,     VA_RT_FORMAT_YUV420_10 },
    { VAProfileH264High422,    VA_RT_FORMAT_YUV422 | VA_RT_FORMAT_YUV422_10 },
    { VAProfileHEVCMain,       VA_RT_FORMAT_YUV420 },
    { VAProfileHEVCMain10,     VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_YUV420_10 },
    { VAProfileVP9Profile0,    VA_RT_FORMAT_YUV420 },
    { VAProfileVP9Profile2,    VA_RT_FORMAT_YUV420_10 },
    { VAProfileAV1Profile0,    VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_YUV420_10 },
    { VAProfileVP8Version0_3,  VA_RT_FORMAT_YUV420 },
    { VAProfileMPEG2Simple,    VA_RT_FORMAT_YUV420 },
    { VAProfileMPEG2Main,      VA_RT_FORMAT_YUV420 },
    { VAProfileJPEGBaseline,   VA_RT_FORMAT_YUV420 },
    { VAProfileNone,           VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_YUV422 | VA_RT_FORMAT_RGB32 | VA_RT_FORMAT_RGB16 },
};

/* Map VA profile to codec. Device path is resolved by OUTPUT fourcc. */
static const struct {
    VAProfile profile;
    enum vav4l2_codec codec;
} profile_codec_map[] = {
    { VAProfileH264ConstrainedBaseline, VAV4L2_CODEC_H264 },
    { VAProfileH264Main,    VAV4L2_CODEC_H264 },
    { VAProfileH264High,    VAV4L2_CODEC_H264 },
    { VAProfileH264High10,  VAV4L2_CODEC_H264 },
    { VAProfileH264High422, VAV4L2_CODEC_H264 },
    { VAProfileHEVCMain,    VAV4L2_CODEC_HEVC },
    { VAProfileHEVCMain10,  VAV4L2_CODEC_HEVC },
    { VAProfileVP9Profile0, VAV4L2_CODEC_VP9 },
    { VAProfileVP9Profile2, VAV4L2_CODEC_VP9 },
    { VAProfileAV1Profile0, VAV4L2_CODEC_AV1 },
    { VAProfileVP8Version0_3, VAV4L2_CODEC_VP8 },
    { VAProfileMPEG2Simple, VAV4L2_CODEC_MPEG2 },
    { VAProfileMPEG2Main,   VAV4L2_CODEC_MPEG2 },
    { VAProfileJPEGBaseline, VAV4L2_CODEC_JPEG_ENC },
    { VAProfileNone,        VAV4L2_CODEC_VPP },
};

static int codec_for_profile(VAProfile profile, enum vav4l2_codec *codec)
{
    unsigned i;

    for (i = 0; i < sizeof(profile_codec_map) / sizeof(profile_codec_map[0]); i++) {
        if (profile_codec_map[i].profile == profile) {
            *codec = profile_codec_map[i].codec;
            return 0;
        }
    }
    return -1;
}

static const char *cached_device(const struct vav4l2_driver_data *dd, enum vav4l2_codec codec)
{
    const char *p = NULL;

    switch (codec) {
    case VAV4L2_CODEC_H264:  p = dd->dev_h264;  break;
    case VAV4L2_CODEC_HEVC:  p = dd->dev_hevc;  break;
    case VAV4L2_CODEC_VP9:   p = dd->dev_vp9;   break;
    case VAV4L2_CODEC_AV1:   p = dd->dev_av1;   break;
    case VAV4L2_CODEC_VP8:      p = dd->dev_vp8;      break;
    case VAV4L2_CODEC_MPEG2:    p = dd->dev_mpeg2;    break;
    case VAV4L2_CODEC_JPEG_ENC: p = dd->dev_jpeg_enc; break;
    case VAV4L2_CODEC_VPP:      p = dd->dev_vpp;      break;
    default: break;
    }
    return (p && p[0]) ? p : NULL;
}

static int kernel_profile_available(const struct vav4l2_driver_data *dd, VAProfile profile)
{
    if (profile == VAProfileVP9Profile0 || profile == VAProfileVP9Profile2) {
        unsigned bit = profile == VAProfileVP9Profile0 ? V4L2_MPEG_VIDEO_VP9_PROFILE_0 : V4L2_MPEG_VIDEO_VP9_PROFILE_2;
        return dd->vp9_profiles_known && !!(dd->vp9_profiles & (1u << bit));
    }
    if (profile == VAProfileHEVCMain || profile == VAProfileHEVCMain10) {
        unsigned bit = profile == VAProfileHEVCMain ? V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN : V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN_10;
        return !dd->hevc_profiles_known || !!(dd->hevc_profiles & (1u << bit));
    }
    if (!dd->h264_profiles_known) return 1;
    unsigned bit;
    switch (profile) {
    case VAProfileH264ConstrainedBaseline:
        return !!(dd->h264_profiles & ((1u << V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE) |
                                      (1u << V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_BASELINE)));
    case VAProfileH264Main: bit = V4L2_MPEG_VIDEO_H264_PROFILE_MAIN; break;
    case VAProfileH264High: bit = V4L2_MPEG_VIDEO_H264_PROFILE_HIGH; break;
    case VAProfileH264High10: bit = V4L2_MPEG_VIDEO_H264_PROFILE_HIGH_10; break;
    case VAProfileH264High422: bit = V4L2_MPEG_VIDEO_H264_PROFILE_HIGH_422; break;
    default: return 1;
    }
    return !!(dd->h264_profiles & (1u << bit));
}

static int profile_available(const struct vav4l2_driver_data *dd, VAProfile profile)
{
    if (!kernel_profile_available(dd, profile)) return 0;
    if (!vav4l2_compat_backend_needed()) return 1;
    enum vav4l2_codec kind;
    if (codec_for_profile(profile, &kind) < 0 || kind == VAV4L2_CODEC_VPP) return 1;
    const char *path = cached_device(dd, kind);
    if (!path) return 0;
    int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return 0;
    struct v4l2_capability cap = {0};
    int result = vav4l2_xioctl(fd, VIDIOC_QUERYCAP, &cap);
    uint32_t coded = V4L2_PIX_FMT_JPEG;
    if (result == 0 && kind != VAV4L2_CODEC_JPEG_ENC && vav4l2_decoder_select(fd, kind, &coded) < 0) result = -1;
    close(fd);
    if (result < 0) return 0;
    struct vav4l2_compat_identity identity; vav4l2_compat_identity(&cap, &identity);
    struct vav4l2_compat_codec codec = {.direction = kind == VAV4L2_CODEC_JPEG_ENC ? VAV4L2_COMPAT_ENCODE : VAV4L2_COMPAT_DECODE,
        .coded_format = coded, .va_profile = profile, .profile_known = 1};
    return vav4l2_compat_codec_score(&identity, &codec) >= 0;
}

/*
 * VA-API driver VTable implementations
 */

/* Existing decoder/JPEG dimension policy. Raw VPP limits come from its
 * discovered devices rather than inheriting this codec limit. */
#define VAV4L2_MAX_PICTURE_DIM 4096



static void destroy_surface_locked(struct vav4l2_driver_data *dd,
                                   struct vav4l2_surface *s, VASurfaceID id);

/* Caller holds vav4l2_api_lock. */
void vav4l2_free_buffer_locked(struct vav4l2_buffer *buf)
{
    if (buf->mmapped == 1)
        munmap(buf->data, buf->size);
    else if (buf->mmapped == 0)
        free(buf->data);
    else if (buf->mmapped == 2 && buf->borrow_surf) {
        struct vav4l2_surface *s = buf->borrow_surf;
        if (s->memfd_borrows)
            s->memfd_borrows--;
        if (!s->memfd_borrows && s->memfd_retired) {
            munmap(s->memfd_retired, s->memfd_retired_size);
            s->memfd_retired = NULL;
            s->memfd_retired_size = 0;
        }
    }
    free(buf);
}

static VAStatus
vav4l2_terminate(VADriverContextP ctx)
{
    struct vav4l2_driver_data *driver_data = ctx->pDriverData;
    int i;

    if (!driver_data)
        return VA_STATUS_SUCCESS;

    pthread_mutex_lock(&vav4l2_api_lock);
    while (driver_data->contexts) {
        struct vav4l2_context *context = driver_data->contexts;
        struct vav4l2_buffer *buf = context->buffers;

        driver_data->contexts = context->next;
        while (buf) {
            struct vav4l2_buffer *next = buf->next;

            vav4l2_free_buffer_locked(buf);
            buf = next;
        }
        vav4l2_release_context_device(context);
        free(context->device_path);
        free(context->render_targets);
        free(context);
    }
    while (driver_data->orphan_buffers) {
        struct vav4l2_buffer *buf = driver_data->orphan_buffers;
        driver_data->orphan_buffers = buf->next;
        vav4l2_free_buffer_locked(buf);
    }
    for (i = 0; i < VAV4L2_MAX_SURFACES; i++) {
        struct vav4l2_surface *s = driver_data->surfaces[i];
        if (s)
            destroy_surface_locked(driver_data, s, (VASurfaceID)i);
    }
    while (driver_data->configs) {
        struct vav4l2_config *config = driver_data->configs;

        driver_data->configs = config->next;
        free(config);
    }
    vav4l2_gpu_copy_finish(driver_data);
    vav4l2_gbm_driver_cleanup(driver_data);
    if (g_driver_instances && !--g_driver_instances) {
        vav4l2_gbm_cleanup();
        vav4l2_format_cleanup();
    }
    pthread_mutex_unlock(&vav4l2_api_lock);

    free(driver_data);
    ctx->pDriverData = NULL;

    return VA_STATUS_SUCCESS;
}

static VAStatus
vav4l2_query_config_profiles(VADriverContextP ctx,
                             VAProfile *profiles,
                             int *num_profiles)
{
    struct vav4l2_driver_data *driver_data = ctx->pDriverData;
    int count = 0;
    int cap = profiles ? (int)NUM_PROFILES : 0;

    for (unsigned i = 0; i < NUM_PROFILES; i++) {
        enum vav4l2_codec codec;

        if (!profile_available(driver_data, vav4l2_profiles[i])) continue;

        if (vav4l2_profiles[i] == VAProfileAV1Profile0 &&
            (!getenv("VAV4L2_ADVERTISE_AV1") ||
             strcmp(getenv("VAV4L2_ADVERTISE_AV1"), "1") != 0)) {
            /* Experimental translator; opt in until broader streams are qualified. */
            continue;
        }
        if (codec_for_profile(vav4l2_profiles[i], &codec) < 0)
            continue;
        if (!cached_device(driver_data, codec))
            continue;
        if (count < cap)
            profiles[count] = vav4l2_profiles[i];
        count++;
    }

    if (num_profiles)
        *num_profiles = count;
    return VA_STATUS_SUCCESS;
}

static VAStatus
vav4l2_query_config_entrypoints(VADriverContextP ctx,
                                VAProfile profile,
                                VAEntrypoint *entrypoints,
                                int *num_entrypoints)
{
    struct vav4l2_driver_data *driver_data = ctx->pDriverData;
    enum vav4l2_codec codec;
    int supported = 0;

    if (!num_entrypoints) return VA_STATUS_ERROR_INVALID_PARAMETER;

    for (unsigned i = 0; i < NUM_PROFILES; i++) {
        if (vav4l2_profiles[i] == profile) {
            supported = 1;
            break;
        }
    }
    if (supported && codec_for_profile(profile, &codec) == 0 &&
        !cached_device(driver_data, codec))
        supported = 0;
    if (!profile_available(driver_data, profile)) supported = 0;

    if (!supported) {
        *num_entrypoints = 0;
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }

    VAEntrypoint ep = VAEntrypointVLD;
    if (profile == VAProfileJPEGBaseline)
        ep = VAEntrypointEncPicture;
    else if (profile == VAProfileNone)
        ep = VAEntrypointVideoProc;

    if (entrypoints)
        entrypoints[0] = ep;
    if (num_entrypoints)
        *num_entrypoints = 1;
    return VA_STATUS_SUCCESS;
}

static VAStatus profile_entrypoint(VAProfile profile, VAEntrypoint entrypoint)
{
    enum vav4l2_codec codec;
    if (codec_for_profile(profile, &codec) < 0)
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    VAEntrypoint expected = profile == VAProfileNone ? VAEntrypointVideoProc :
        profile == VAProfileJPEGBaseline ? VAEntrypointEncPicture : VAEntrypointVLD;
    return entrypoint == expected ? VA_STATUS_SUCCESS : VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
}

/* Surface limits enclose both input and output roles. Exact format/size
 * combinations remain subject to per-job TRY_FMT, not a Cartesian promise. */
static VAStatus vpp_surface_limits(const VAProcPipelineCaps *caps, uint32_t limits[4])
{
    const uint32_t min_in[] = {caps->min_input_width, caps->min_input_height};
    const uint32_t min_out[] = {caps->min_output_width, caps->min_output_height};
    const uint32_t max_in[] = {caps->max_input_width, caps->max_input_height};
    const uint32_t max_out[] = {caps->max_output_width, caps->max_output_height};
    for (unsigned axis = 0; axis < 2; axis++) {
        if (!min_in[axis] || !min_out[axis] || min_in[axis] > max_in[axis] || min_out[axis] > max_out[axis])
            return VA_STATUS_ERROR_OPERATION_FAILED;
        limits[axis] = min_in[axis] < min_out[axis] ? min_in[axis] : min_out[axis];
        limits[axis + 2] = max_in[axis] > max_out[axis] ? max_in[axis] : max_out[axis];
        if (limits[axis + 2] > UINT16_MAX) limits[axis + 2] = UINT16_MAX;
        if (limits[axis] > limits[axis + 2]) return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    return VA_STATUS_SUCCESS;
}

static unsigned int config_attribute(struct vav4l2_driver_data *dd, VAProfile profile, VAEntrypoint entrypoint,
                                      VAConfigAttribType type)
{
    switch (type) {
    case VAConfigAttribRTFormat:
        for (unsigned i = 0; i < sizeof(profile_rt_formats) / sizeof(profile_rt_formats[0]); i++)
            if (profile_rt_formats[i].profile == profile)
                return profile_rt_formats[i].rt_format | (profile == VAProfileNone ? vav4l2_vpp_extra_rt_formats(dd) : 0);
        return VA_ATTRIB_NOT_SUPPORTED;
    case VAConfigAttribDecSliceMode:
        return entrypoint == VAEntrypointVLD ? VA_DEC_SLICE_MODE_NORMAL : VA_ATTRIB_NOT_SUPPORTED;
    case VAConfigAttribEncPackedHeaders:
        return entrypoint == VAEntrypointEncPicture ? 0 : VA_ATTRIB_NOT_SUPPORTED;
    case VAConfigAttribEncQualityRange:
        /* One encode preset. JPEG's picture quality (1..100) controls
         * quantization, not VAEncMiscParameterTypeQualityLevel presets. */
        return entrypoint == VAEntrypointEncPicture ? 1 : VA_ATTRIB_NOT_SUPPORTED;
    case VAConfigAttribMaxPictureWidth:
    case VAConfigAttribMaxPictureHeight:
        if (entrypoint == VAEntrypointVideoProc) {
            VAProcPipelineCaps caps;
            uint32_t limits[4];
            pthread_mutex_lock(&vav4l2_api_lock);
            VAStatus status = vav4l2_vpp_query_device_caps(dd, &caps);
            pthread_mutex_unlock(&vav4l2_api_lock);
            if (status != VA_STATUS_SUCCESS || vpp_surface_limits(&caps, limits) != VA_STATUS_SUCCESS)
                return VA_ATTRIB_NOT_SUPPORTED;
            return limits[type == VAConfigAttribMaxPictureWidth ? 2 : 3];
        }
        return VAV4L2_MAX_PICTURE_DIM;
    default:
        return VA_ATTRIB_NOT_SUPPORTED;
    }
}

static VAStatus
vav4l2_get_config_attributes(VADriverContextP ctx,
                             VAProfile profile,
                             VAEntrypoint entrypoint,
                             VAConfigAttrib *attrib_list,
                             int num_attribs)
{
    if (!profile_available(ctx->pDriverData, profile)) return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    VAStatus st = profile_entrypoint(profile, entrypoint);
    if (st != VA_STATUS_SUCCESS)
        return st;
    if (num_attribs < 0 || (num_attribs && !attrib_list))
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    for (int i = 0; i < num_attribs; i++)
        attrib_list[i].value = config_attribute(ctx->pDriverData, profile, entrypoint, attrib_list[i].type);
    return VA_STATUS_SUCCESS;
}

static VAStatus
vav4l2_create_config(VADriverContextP ctx,
                     VAProfile profile,
                     VAEntrypoint entrypoint,
                     VAConfigAttrib *attrib_list,
                     int num_attribs,
                     VAConfigID *config_id)
{
    struct vav4l2_driver_data *driver_data = ctx->pDriverData;
    struct vav4l2_config *config;

    const char *device_path;
    enum vav4l2_codec codec;
    uint32_t fourcc;
    char fcc[5];

    VAStatus st = profile_entrypoint(profile, entrypoint);
    if (st != VA_STATUS_SUCCESS)
        return st;
    if (!config_id || num_attribs < 0 || (num_attribs && !attrib_list))
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (!profile_available(driver_data, profile)) return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    if (codec_for_profile(profile, &codec) < 0)
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    unsigned int supported_rt = 0, rt;
    for (unsigned i = 0; i < sizeof(profile_rt_formats) / sizeof(profile_rt_formats[0]); i++)
        if (profile_rt_formats[i].profile == profile)
            supported_rt = profile_rt_formats[i].rt_format;
    if (profile == VAProfileNone) supported_rt |= vav4l2_vpp_extra_rt_formats(driver_data);
    rt = profile == VAProfileHEVCMain10 ? VA_RT_FORMAT_YUV420_10 : supported_rt & (~supported_rt + 1u);
    for (int i = 0; i < num_attribs; i++) {
        if (attrib_list[i].type == VAConfigAttribRTFormat) {
            rt = attrib_list[i].value;
        } else {
            unsigned int supported = config_attribute(driver_data, profile, entrypoint, attrib_list[i].type);
            if (supported == VA_ATTRIB_NOT_SUPPORTED || attrib_list[i].value != supported)
                return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
        }
    }
    if (!rt || (rt & ~supported_rt))
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;

    device_path = cached_device(driver_data, codec);
    if (!device_path) {
        if (vav4l2_codec_coded_fourcc(codec, &fourcc) == 0)
            vav4l2_fourcc_to_str(fourcc, fcc);
        else
            memcpy(fcc, "????", 5);
        fprintf(stderr,
                "v4l2: no V4L2 decoder advertises %s "
                "(OUTPUT fourcc %s)\n",
                vav4l2_codec_name(codec), fcc);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    config = calloc(1, sizeof(*config));
    if (!config)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;

    config->profile = profile;
    config->entrypoint = entrypoint;
    config->codec = codec;
    config->device_path = device_path;

    config->rt_format = rt;

    fprintf(stderr, "v4l2: %s config uses %s (VA profile %d rt=0x%x)\n",
            vav4l2_codec_name(codec), device_path, (int)profile, config->rt_format);

    /* Simple ID allocation */
    pthread_mutex_lock(&vav4l2_api_lock);
    config->config_id = ++driver_data->next_config_id;
    config->next = driver_data->configs;
    driver_data->configs = config;
    pthread_mutex_unlock(&vav4l2_api_lock);

    *config_id = config->config_id;
    return VA_STATUS_SUCCESS;
}

static VAStatus
vav4l2_destroy_config(VADriverContextP ctx, VAConfigID config_id)
{
    struct vav4l2_driver_data *driver_data = ctx->pDriverData;
    struct vav4l2_config **pp;

    pthread_mutex_lock(&vav4l2_api_lock);
    pp = &driver_data->configs;
    while (*pp) {
        if ((*pp)->config_id == config_id) {
            struct vav4l2_config *config = *pp;
            *pp = config->next;
            free(config);
            pthread_mutex_unlock(&vav4l2_api_lock);
            return VA_STATUS_SUCCESS;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&vav4l2_api_lock);

    return VA_STATUS_ERROR_INVALID_CONFIG;
}

static VAStatus
vav4l2_query_config_attributes(VADriverContextP ctx,
                               VAConfigID config_id,
                               VAProfile *profile,
                               VAEntrypoint *entrypoint,
                               VAConfigAttrib *attrib_list,
                               int *num_attribs)
{
    struct vav4l2_driver_data *driver_data = ctx->pDriverData;
    struct vav4l2_config *config;
    VAProfile prof;
    VAEntrypoint entry;
    unsigned int rt;
    VAConfigAttrib attribs[8];
    int count = 0;

    pthread_mutex_lock(&vav4l2_api_lock);
    config = driver_data->configs;
    while (config && config->config_id != config_id)
        config = config->next;
    if (!config) {
        pthread_mutex_unlock(&vav4l2_api_lock);
        return VA_STATUS_ERROR_INVALID_CONFIG;
    }
    /* Snapshot the config fields under the lock (vaDestroyConfig frees
     * nodes holding it), then build the reply unlocked. */
    prof = config->profile;
    entry = config->entrypoint;
    rt = config->rt_format;
    pthread_mutex_unlock(&vav4l2_api_lock);

    if (profile)
        *profile = prof;
    if (entrypoint)
        *entrypoint = entry;

    /*
     * Chrome's FillProfileInfo_Locked allocates vaMaxNumConfigAttributes()
     * slots and passes an uninitialized *num_attribs used only as an out
     * parameter. Do not treat *num_attribs as a buffer capacity.
     */
    attribs[count].type = VAConfigAttribRTFormat;
    attribs[count].value = rt;
    count++;
    static const VAConfigAttribType types[] = {
        VAConfigAttribDecSliceMode, VAConfigAttribMaxPictureWidth,
        VAConfigAttribMaxPictureHeight, VAConfigAttribEncPackedHeaders,
        VAConfigAttribEncQualityRange,
    };
    for (unsigned i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        unsigned int value = config_attribute(driver_data, prof, entry, types[i]);
        if (value == VA_ATTRIB_NOT_SUPPORTED)
            continue;
        attribs[count].type = types[i];
        attribs[count].value = value;
        count++;
    }

    if (attrib_list)
        memcpy(attrib_list, attribs, (size_t)count * sizeof(attribs[0]));
    if (num_attribs)
        *num_attribs = count;
    return VA_STATUS_SUCCESS;
}

/* Surface FOURCCs and their VA render-target class. Keep allocation and
 * per-config enumeration in agreement; a FOURCC is not an RT bitmask. */
static const struct {
    uint32_t fourcc;
    unsigned int rt_format;
} surface_formats[] = {
    { VA_FOURCC_NV12, VA_RT_FORMAT_YUV420 },
    { VA_FOURCC_I420, VA_RT_FORMAT_YUV420 },
    { VA_FOURCC_P010, VA_RT_FORMAT_YUV420_10 },
    { VA_FOURCC_YUY2, VA_RT_FORMAT_YUV422 },
    { VA_FOURCC_Y210, VA_RT_FORMAT_YUV422_10 },
    { VA_FOURCC_BGRX, VA_RT_FORMAT_RGB32 },
    { VA_FOURCC_BGRA, VA_RT_FORMAT_RGB32 },
    { VA_FOURCC_ARGB, VA_RT_FORMAT_RGB32 },
    { VA_FOURCC_NV21, VA_RT_FORMAT_YUV420 },
    { VA_FOURCC_YV12, VA_RT_FORMAT_YUV420 },
    { VA_FOURCC_UYVY, VA_RT_FORMAT_YUV422 },
    { VA_FOURCC_VYUY, VA_RT_FORMAT_YUV422 },
    { VA_FOURCC_YVYU, VA_RT_FORMAT_YUV422 },
    { VA_FOURCC_422H, VA_RT_FORMAT_YUV422 },
    { VA_FOURCC_YV16, VA_RT_FORMAT_YUV422 },
    { VA_FOURCC_RGB565, VA_RT_FORMAT_RGB16 },
    { VA_FOURCC_RGBA, VA_RT_FORMAT_RGB32 },
    { VA_FOURCC_RGBX, VA_RT_FORMAT_RGB32 },
    { VA_FOURCC_XRGB, VA_RT_FORMAT_RGB32 },
    { VA_FOURCC_ABGR, VA_RT_FORMAT_RGB32 },
    { VA_FOURCC_XBGR, VA_RT_FORMAT_RGB32 },
    { VA_FOURCC_BGR565, VA_RT_FORMAT_RGB16 },
};

static uint32_t default_surface_fourcc(unsigned int rt)
{
    for (unsigned i = 0; i < sizeof(surface_formats) / sizeof(surface_formats[0]); i++)
        if (surface_formats[i].rt_format == rt)
            return surface_formats[i].fourcc;
    return 0;
}

/* Enumerate only formats belonging to the supplied configuration. */
static VAStatus
vav4l2_query_surface_attributes(VADriverContextP ctx,
                                VAConfigID config_id,
                                VASurfaceAttrib *attrib_list,
                                unsigned int *num_attribs)
{
    struct vav4l2_driver_data *driver_data = ctx->pDriverData;
    struct vav4l2_config *config;
    unsigned int rt;
    VAEntrypoint entrypoint;

    if (!num_attribs)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    pthread_mutex_lock(&vav4l2_api_lock);
    config = driver_data->configs;
    while (config && config->config_id != config_id)
        config = config->next;
    if (!config) {
        pthread_mutex_unlock(&vav4l2_api_lock);
        return VA_STATUS_ERROR_INVALID_CONFIG;
    }
    rt = config->rt_format;
    entrypoint = config->entrypoint;
    pthread_mutex_unlock(&vav4l2_api_lock);

    VAProcPipelineCaps vpp_caps;
    uint32_t limits[] = {16, 16, VAV4L2_MAX_PICTURE_DIM, VAV4L2_MAX_PICTURE_DIM};
    if (entrypoint == VAEntrypointVideoProc) {
        pthread_mutex_lock(&vav4l2_api_lock);
        VAStatus status = vav4l2_vpp_query_device_caps(driver_data, &vpp_caps);
        pthread_mutex_unlock(&vav4l2_api_lock);
        if (status != VA_STATUS_SUCCESS) return status;
        status = vpp_surface_limits(&vpp_caps, limits);
        if (status != VA_STATUS_SUCCESS) return status;
    }
    VASurfaceAttrib attribs[sizeof(surface_formats) / sizeof(surface_formats[0]) + 5];
    unsigned int count = 0;
    int import_available = 0;
    for (unsigned p = 0; p < sizeof(surface_formats) / sizeof(surface_formats[0]); p++) {
        if (!(rt & surface_formats[p].rt_format))
            continue;
        uint32_t fourcc = surface_formats[p].fourcc;
        if (entrypoint == VAEntrypointVideoProc) {
            unsigned supported = 0;
            for (unsigned i = 0; i < vpp_caps.num_input_pixel_formats; i++)
                supported |= vpp_caps.input_pixel_format[i] == fourcc;
            for (unsigned i = 0; i < vpp_caps.num_output_pixel_formats; i++)
                supported |= vpp_caps.output_pixel_format[i] == fourcc;
            if (!supported) continue;
        } else if ((entrypoint != VAEntrypointEncPicture &&
                    (fourcc == VA_FOURCC_I420 || fourcc == VA_FOURCC_NV21 || fourcc == VA_FOURCC_YV12)) || fourcc == VA_FOURCC_UYVY ||
                   fourcc == VA_FOURCC_VYUY || fourcc == VA_FOURCC_YVYU ||
                   fourcc == VA_FOURCC_422H || fourcc == VA_FOURCC_YV16) {
            /* JPEG accepts the implemented 420 inputs; decode retains
             * its existing layouts. Raw 422 additions remain VPP-only. */
            continue;
        }
        attribs[count].type          = VASurfaceAttribPixelFormat;
        attribs[count].flags         = VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE;
        attribs[count].value.type    = VAGenericValueTypeInteger;
        attribs[count].value.value.i = (int)surface_formats[p].fourcc;
        import_available = 1;
        count++;
    }

    attribs[count].type          = VASurfaceAttribMinWidth;
    attribs[count].flags         = VA_SURFACE_ATTRIB_GETTABLE;
    attribs[count].value.type    = VAGenericValueTypeInteger;
    attribs[count].value.value.i = limits[0];
    count++;

    attribs[count].type          = VASurfaceAttribMinHeight;
    attribs[count].flags         = VA_SURFACE_ATTRIB_GETTABLE;
    attribs[count].value.type    = VAGenericValueTypeInteger;
    attribs[count].value.value.i = limits[1];
    count++;

    attribs[count].type          = VASurfaceAttribMaxWidth;
    attribs[count].flags         = VA_SURFACE_ATTRIB_GETTABLE;
    attribs[count].value.type    = VAGenericValueTypeInteger;
    attribs[count].value.value.i = limits[2];
    count++;

    attribs[count].type          = VASurfaceAttribMaxHeight;
    attribs[count].flags         = VA_SURFACE_ATTRIB_GETTABLE;
    attribs[count].value.type    = VAGenericValueTypeInteger;
    attribs[count].value.value.i = limits[3];
    count++;

    attribs[count].type          = VASurfaceAttribMemoryType;
    attribs[count].flags         = VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE;
    attribs[count].value.type    = VAGenericValueTypeInteger;
    attribs[count].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_VA |
        (import_available ? VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME | VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2 | VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_3 : 0);
    count++;

    if (!attrib_list) {
        *num_attribs = count;
        return VA_STATUS_SUCCESS;
    }

    if (*num_attribs < count) {
        *num_attribs = count;
        return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
    }

    memcpy(attrib_list, attribs, count * sizeof(attribs[0]));
    *num_attribs = count;
    return VA_STATUS_SUCCESS;
}

/*
 * Surfaces — backed by V4L2 buffers (DMA-BUF)
 */

static VAStatus
vav4l2_alloc_surfaces(VADriverContextP ctx,
                       int width, int height,
                       int format,
                       int num_surfaces,
                       VASurfaceID *surfaces, int allocate_backing)
{
    struct vav4l2_driver_data *driver_data = ctx->pDriverData;

    if (width <= 0 || height <= 0 || width > UINT16_MAX || height > UINT16_MAX ||
        num_surfaces <= 0 || !surfaces)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    uint32_t stride = vav4l2_default_image_stride(format, width);
    if (!stride || !vav4l2_va_image_size(format, stride, height))
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    pthread_mutex_lock(&vav4l2_api_lock);
    int created = 0;
    for (int i = 0; i < num_surfaces; i++) {
        struct vav4l2_surface *surface = calloc(1, sizeof(*surface));
        VASurfaceID id;

        if (!surface)
            goto fail;

        /* Recycle an ID or take the next free one; never index past the
         * fixed table. */
        if (driver_data->n_free_surface_ids > 0)
            id = driver_data->free_surface_ids[--driver_data->n_free_surface_ids];
        else if (driver_data->next_surface_id + 1 < VAV4L2_MAX_SURFACES)
            id = ++driver_data->next_surface_id;
        else {
            free(surface);
            goto fail;
        }
        if (driver_data->surfaces[id]) {
            free(surface);
            goto fail;
        }

        surface->driver_data = driver_data;
        surface->surface_id = id;
        surface->width = width;
        surface->height = height;
        surface->format = format;
        surface->status = VASurfaceReady;
        surface->buf_index = -1;
        surface->cap_view = NULL;
        surface->memfd_fd = -1;
        surface->cpu_stride = vav4l2_default_image_stride(format, width);
        surface->cpu_size = vav4l2_va_image_size(format, surface->cpu_stride, height);
        /* cpu_ptr is lazy (ensure_cpu): decode-only surfaces never touch
         * it — an eager calloc cost ~3MB/surface at 1080p for nothing. */
        /* dma-buf at create so Chrome/Firefox DRM-PRIME export works
         * before the first picture. V4L2 EXPBUF replaces this after REQBUFS. */
        if (allocate_backing && vav4l2_surface_alloc_export_fd(surface) < 0)
            fprintf(stderr, "v4l2: surface memfd backing failed\n");

        surfaces[i] = id;
        driver_data->surfaces[id] = surface;
        created++;
    }

    pthread_mutex_unlock(&vav4l2_api_lock);
    return VA_STATUS_SUCCESS;

fail:
    /* Cleanup stays under the lock and only touches surfaces this call
     * actually created — surfaces[] entries past `created` are the
     * caller's uninitialized memory, never scan them. */
    for (int j = 0; j < created; j++) {
        struct vav4l2_surface *s = vav4l2_surface_by_id(driver_data, surfaces[j]);

        if (s) {
            if (s->memfd_map)
                munmap(s->memfd_map, s->memfd_map_size);
            if (s->memfd_retired)
                munmap(s->memfd_retired, s->memfd_retired_size);
            if (s->memfd_fd >= 0)
                close(s->memfd_fd);
            free(s->cpu_ptr);
            free(s);
            driver_data->surfaces[surfaces[j]] = NULL;
            vav4l2_surface_id_push(driver_data, surfaces[j]);
            surfaces[j] = VA_INVALID_ID;
        }
    }
    pthread_mutex_unlock(&vav4l2_api_lock);
    return VA_STATUS_ERROR_ALLOCATION_FAILED;
}

static VAStatus
vav4l2_import_surfaces(VADriverContextP ctx, unsigned width, unsigned height,
                       VASurfaceID *surfaces, unsigned count, unsigned fourcc, const VADRMPRIMESurfaceDescriptor *descriptors)
{
    struct vav4l2_surface *imports = calloc(count, sizeof(*imports));
    VASurfaceID *ids = calloc(count, sizeof(*ids));
    if (!imports || !ids) { free(imports); free(ids); return VA_STATUS_ERROR_ALLOCATION_FAILED; }
    VAStatus status = VA_STATUS_SUCCESS;
    pthread_mutex_lock(&vav4l2_api_lock);
    for (unsigned i = 0; i < count; i++) {
        imports[i].driver_data = ctx->pDriverData;
        imports[i].width = width; imports[i].height = height; imports[i].format = fourcc;
        const VADRMPRIMESurfaceDescriptor *d = &descriptors[i];
        int canonical = fourcc == VA_FOURCC_NV12 && d->fourcc == fourcc && d->num_objects == 1 && d->num_layers == 1 &&
            d->layers[0].num_planes == 2 && d->layers[0].offset[0] == 0 &&
            d->layers[0].pitch[0] == d->layers[0].pitch[1] && d->layers[0].offset[1] == (uint64_t)d->layers[0].pitch[0] * height;
        status = canonical ? vav4l2_gbm_surface_import(&imports[i], d) : vav4l2_prime_import(&imports[i], d);
        if (canonical && status != VA_STATUS_SUCCESS && status != VA_STATUS_ERROR_INVALID_PARAMETER &&
            status != VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT)
            status = vav4l2_prime_import(&imports[i], d);
        if (status != VA_STATUS_SUCCESS) break;
    }
    pthread_mutex_unlock(&vav4l2_api_lock);
    if (status == VA_STATUS_SUCCESS) {
        status = vav4l2_alloc_surfaces(ctx, width, height, fourcc, count, ids, 0);
        if (status == VA_STATUS_SUCCESS) {
            struct vav4l2_driver_data *dd = ctx->pDriverData;
            pthread_mutex_lock(&vav4l2_api_lock);
            for (unsigned i = 0; i < count; i++) {
                struct vav4l2_surface *s = dd->surfaces[ids[i]];
                if (status != VA_STATUS_SUCCESS) { destroy_surface_locked(dd, s, ids[i]); continue; }
                s->gbm_bo = imports[i].gbm_bo; s->gbm_stride = imports[i].gbm_stride;
                s->gbm_imported = imports[i].gbm_imported; s->import_fd = imports[i].import_fd;
                s->prime = imports[i].prime; imports[i].prime = NULL;
                s->stride = imports[i].stride; s->aligned_h = height; s->has_pic = 1; s->last_writer = imports[i].last_writer;
                s->cap_fourcc = imports[i].cap_fourcc;
                imports[i].gbm_bo = NULL; imports[i].gbm_imported = 0;
            }
            if (status == VA_STATUS_SUCCESS) memcpy(surfaces, ids, count * sizeof(*ids));
            pthread_mutex_unlock(&vav4l2_api_lock);
        }
    }
    pthread_mutex_lock(&vav4l2_api_lock);
    for (unsigned i = 0; i < count; i++) vav4l2_gbm_surface_destroy(&imports[i]);
    pthread_mutex_unlock(&vav4l2_api_lock);
    free(imports); free(ids);
    return status;
}

static VAStatus
vav4l2_create_surfaces(VADriverContextP ctx,
                       int width, int height,
                       int format,
                       int num_surfaces,
                       VASurfaceID *surfaces)
{
    uint32_t fourcc = default_surface_fourcc((unsigned int)format);
    if (!fourcc)
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    return vav4l2_alloc_surfaces(ctx, width, height, fourcc, num_surfaces, surfaces, 1);
}

static VAStatus
vav4l2_create_surfaces2(VADriverContextP ctx,
                        unsigned int format,
                        unsigned int width,
                        unsigned int height,
                        VASurfaceID *surfaces,
                        unsigned int num_surfaces,
                        VASurfaceAttrib *attrib_list,
                        unsigned int num_attribs)
{
    unsigned int fourcc = default_surface_fourcc(format);
    unsigned memory = VA_SURFACE_ATTRIB_MEM_TYPE_VA;
    int pixel_format_set = 0;
    const void *external = NULL;
    const VADRMFormatModifierList *modifiers = NULL;

    if (!width || !height || !surfaces || !num_surfaces || width > UINT16_MAX || height > UINT16_MAX || num_surfaces > VAV4L2_MAX_SURFACES ||
        (num_attribs && !attrib_list))
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (!fourcc)
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;

    for (unsigned int i = 0; i < num_attribs; i++) {
        if (attrib_list[i].type == VASurfaceAttribMemoryType) {
            if (attrib_list[i].value.type != VAGenericValueTypeInteger) return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
            memory = attrib_list[i].value.value.i;
        }
        if (attrib_list[i].type == VASurfaceAttribExternalBufferDescriptor) {
            if (attrib_list[i].value.type != VAGenericValueTypePointer || !attrib_list[i].value.value.p)
                return VA_STATUS_ERROR_INVALID_PARAMETER;
            external = attrib_list[i].value.value.p;
        }
        if (attrib_list[i].type == VASurfaceAttribDRMFormatModifiers) {
            if (attrib_list[i].value.type != VAGenericValueTypePointer || !attrib_list[i].value.value.p)
                return VA_STATUS_ERROR_INVALID_PARAMETER;
            modifiers = attrib_list[i].value.value.p;
        }
        if (attrib_list[i].type == VASurfaceAttribPixelFormat) {
            if (attrib_list[i].value.type != VAGenericValueTypeInteger)
                return VA_STATUS_ERROR_INVALID_PARAMETER;
            fourcc = attrib_list[i].value.value.i; pixel_format_set = 1;
        }
    }

    if (memory == VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME || memory == VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2 || memory == VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_3) {
        if (!external || modifiers) return VA_STATUS_ERROR_INVALID_PARAMETER;
        VADRMPRIMESurfaceDescriptor *normalized = NULL;
        const VADRMPRIMESurfaceDescriptor *descriptors = external;
        if (memory == VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME) {
            VAStatus status = vav4l2_prime_legacy(external, width, height, num_surfaces, &normalized);
            if (status != VA_STATUS_SUCCESS) return status;
            descriptors = normalized;
        } else if (memory == VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_3) {
            const VADRMPRIME3SurfaceDescriptor *v3 = external;
            for (unsigned i = 0; i < num_surfaces; i++) {
                if (v3[i].flags) return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
                for (unsigned j = 0; j < sizeof(v3[i].reserved) / sizeof(v3[i].reserved[0]); j++)
                    if (v3[i].reserved[j]) return VA_STATUS_ERROR_INVALID_PARAMETER;
            }
            normalized = calloc(num_surfaces, sizeof(*normalized));
            if (!normalized) return VA_STATUS_ERROR_ALLOCATION_FAILED;
            _Static_assert(offsetof(VADRMPRIME3SurfaceDescriptor, flags) == offsetof(VADRMPRIMESurfaceDescriptor, layers) + sizeof(normalized[0].layers), "PRIME descriptor prefix");
            for (unsigned i = 0; i < num_surfaces; i++) memcpy(&normalized[i], &v3[i], offsetof(VADRMPRIME3SurfaceDescriptor, flags));
            descriptors = normalized;
        }
        if (!pixel_format_set) fourcc = descriptors[0].fourcc;
        unsigned matched = 0;
        for (unsigned i = 0; i < sizeof(surface_formats) / sizeof(surface_formats[0]); i++)
            matched |= surface_formats[i].fourcc == fourcc && surface_formats[i].rt_format == format;
        VAStatus status = matched ? vav4l2_import_surfaces(ctx, width, height, surfaces, num_surfaces, fourcc, descriptors) : VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
        free(normalized); return status;
    }
    if (memory != VA_SURFACE_ATTRIB_MEM_TYPE_VA || external) return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
    /* va_drmcommon.h specifies one modifier list per requested surface.
     * Validate the whole batch before allocating any surface IDs. */
    if (modifiers) {
        for (unsigned s = 0; s < num_surfaces; s++) {
            if (!modifiers[s].num_modifiers || !modifiers[s].modifiers) return VA_STATUS_ERROR_INVALID_PARAMETER;
            int linear = 0;
            for (unsigned i = 0; i < modifiers[s].num_modifiers; i++) linear |= modifiers[s].modifiers[i] == DRM_FORMAT_MOD_LINEAR;
            if (!linear) return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
        }
    }

    for (unsigned i = 0; i < sizeof(surface_formats) / sizeof(surface_formats[0]); i++) {
        if (surface_formats[i].fourcc == fourcc && surface_formats[i].rt_format == format)
            return vav4l2_alloc_surfaces(ctx, width, height, fourcc, num_surfaces, surfaces, 1);
    }
    return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
}

void vav4l2_collect_decode_buffers(struct vav4l2_buffer **buffers, int n,
                                   struct vav4l2_collected *cb)
{
    int i;

    memset(cb, 0, sizeof(*cb));
    for (i = 0; i < n; i++) {
        struct vav4l2_buffer *buf = buffers[i];

        if (!buf || !buf->data)
            continue;
        switch (buf->type) {
        case VAPictureParameterBufferType:
            cb->pic = buf->data;
            break;
        case VASliceParameterBufferType: {
            unsigned count = buf->allocated_elements ? buf->num_elements :
                             (buf->num_elements ? buf->num_elements : 1);
            unsigned room = 32 - cb->n_slice_params;
            unsigned take = count < room ? count : room;

            cb->n_slice_params_seen = count > (unsigned)(INT_MAX - cb->n_slice_params_seen)
                ? INT_MAX : cb->n_slice_params_seen + (int)count;
            for (unsigned j = 0; j < take; j++)
                cb->slice_params[cb->n_slice_params++] = (uint8_t *)buf->data + (size_t)j * buf->size;
            break;
        }
        case VAIQMatrixBufferType:
            cb->iq = buf->data;
            break;
        case VASliceDataBufferType: {
            unsigned count = buf->allocated_elements ? buf->num_elements :
                             (buf->num_elements ? buf->num_elements : 1);
            if (!count || buf->size > UINT_MAX / count)
                break;
            uint32_t bytes = buf->size * count;
            if (cb->n_slice_datas < VAV4L2_MAX_SLICE_DATAS) {
                cb->slice_datas[cb->n_slice_datas] = buf->data;
                cb->slice_sizes[cb->n_slice_datas] = bytes;
                cb->n_slice_datas++;
            }
            if (bytes > cb->largest_size) {
                cb->largest = buf->data;
                cb->largest_size = bytes;
            }
            break;
        }
        case VAProbabilityBufferType:
            cb->prob = buf->data;
            break;
        default:
            break;
        }
    }
}

/* Free one surface and recycle its ID. Caller holds vav4l2_api_lock and
 * has already detached the surface from any context (C1 does that for
 * the explicit destroy path; terminate frees contexts first). */
static void
destroy_surface_locked(struct vav4l2_driver_data *dd,
                       struct vav4l2_surface *s, VASurfaceID id)
{
    if (s->memfd_map)
        munmap(s->memfd_map, s->memfd_map_size);
    if (s->memfd_retired)
        munmap(s->memfd_retired, s->memfd_retired_size);
    if (s->memfd_fd >= 0)
        close(s->memfd_fd);
    vav4l2_gbm_surface_destroy(s);
    free(s->cpu_ptr);
    free(s);
    dd->surfaces[id] = NULL;
    vav4l2_surface_id_push(dd, id);
}

static VAStatus
vav4l2_destroy_surfaces(VADriverContextP ctx,
                        VASurfaceID *surfaces,
                        int num_surfaces)
{
    struct vav4l2_driver_data *driver_data = ctx->pDriverData;

    if (num_surfaces < 0 || (num_surfaces && !surfaces)) return VA_STATUS_ERROR_INVALID_PARAMETER;

    pthread_mutex_lock(&vav4l2_api_lock);
    if (vav4l2_debug && num_surfaces > 0) {
        fprintf(stderr, "v4l2: destroy_surfaces n=%d first=%#x\n",
                num_surfaces, surfaces[0]);
    }
    for (int i = 0; i < num_surfaces; i++) {
        struct vav4l2_surface *surface = vav4l2_surface_by_id(driver_data, surfaces[i]);
        if (!surface) {
            pthread_mutex_unlock(&vav4l2_api_lock);
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }
        for (int j = 0; j < i; j++) if (surfaces[j] == surfaces[i]) {
            pthread_mutex_unlock(&vav4l2_api_lock);
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }
        for (struct vav4l2_context *c = driver_data->contexts; c; c = c->next) if (vav4l2_decode_pending(c, surfaces[i])) {
            pthread_mutex_unlock(&vav4l2_api_lock); return VA_STATUS_ERROR_SURFACE_BUSY;
        }
        if (surface && surface->memfd_borrows) {
            pthread_mutex_unlock(&vav4l2_api_lock);
            return VA_STATUS_ERROR_SURFACE_BUSY;
        }
    }
    for (int i = 0; i < num_surfaces; i++) {
        struct vav4l2_surface *surface = vav4l2_surface_by_id(driver_data,
                                                              surfaces[i]);
        if (!surface)
            continue;
        /* Detach from every context referencing the surface before freeing:
         * hand a still-held capture slot back to its pool (destroying
         * surfaced frames mid-session must not drain the 24-slot pool),
         * drop render-target registrations so a recycled ID is never
         * misidentified, and clear any in-flight current_surface. */
        for (struct vav4l2_context *c = driver_data->contexts; c; c = c->next) {
            int owns_capture = vav4l2_surface_capture_matches(c, surface);
            if (vav4l2_h264_field_pending(c, surfaces[i])) vav4l2_h264_discard_field(c, 1);

            for (int k = 0; k < c->num_render_targets; k++) {
                if (c->render_targets[k] == surfaces[i]) {
                    c->render_targets[k] = VA_INVALID_ID;
                }
            }
            if (owns_capture) {
                if (c->codec == VAV4L2_CODEC_AV1 && c->av1.model_active) {
                    /* Model-owned release: forget the owner so a recycled
                     * surface ID is never mistaken for it; the slot update
                     * pushes the buffer when the kernel stops referencing
                     * it. */
                    if (surface->buf_index < VAV4L2_MAX_CAPTURE_BUFS)
                        c->av1.buf_owner[surface->buf_index] =
                            VA_INVALID_SURFACE;
                } else {
                    vav4l2_cap_pool_push(c, surface->buf_index);
                }
            }
            if (c->current_surface == surface) {
                c->current_surface = NULL;
                c->current_surface_id = VA_INVALID_ID;
            }
        }
        surface->buf_index = -1;
        surface->cap_view = NULL;
        destroy_surface_locked(driver_data, surface, surfaces[i]);
    }
    pthread_mutex_unlock(&vav4l2_api_lock);

    return VA_STATUS_SUCCESS;
}

struct vav4l2_context *
vav4l2_context_for_surface(struct vav4l2_driver_data *dd, VASurfaceID surface)
{
    for (struct vav4l2_context *c = dd->contexts; c; c = c->next)
        for (int i = 0; i < c->num_render_targets; i++)
            if (c->render_targets[i] == surface)
                return c;
    return NULL;
}

/* ffmpeg (and some Chrome pools) decode onto surfaces that were never
 * listed as context render_targets. GetImage/Export still need the
 * owning V4L2 context — walk by the capture mmap pointer we stored. */
static struct vav4l2_context *
context_owning_capture(struct vav4l2_driver_data *dd, struct vav4l2_surface *surf)
{
    struct vav4l2_context *c;

    if (!dd || !surf)
        return NULL;
    if (surf->capture_ctx && surf->buf_index >= 0)
        return surf->capture_ctx;
    c = vav4l2_context_for_surface(dd, surf->surface_id);
    if (c)
        return c;
    if (surf->buf_index >= 0) {
        for (c = dd->contexts; c; c = c->next) {
            int i = surf->buf_index;

            if (i >= 0 && i < VAV4L2_MAX_CAPTURE_BUFS &&
                c->capture_buf_ptr[i] &&
                c->capture_buf_ptr[i] != MAP_FAILED &&
                (!surf->cap_view || c->capture_buf_ptr[i] == surf->cap_view))
                return c;
        }
    }
    for (c = dd->contexts; c; c = c->next)
        if (c->entrypoint == VAEntrypointVLD && c->v4l2_fd >= 0)
            return c;
    return NULL;
}


static VAStatus
vav4l2_create_context(VADriverContextP ctx,
                      VAConfigID config_id,
                      int picture_width,
                      int picture_height,
                      int flag,
                      VASurfaceID *render_targets,
                      int num_render_targets,
                      VAContextID *context_id)
{
    (void)flag;
    struct vav4l2_driver_data *driver_data = ctx->pDriverData;

    if (!context_id || picture_width <= 0 || picture_height <= 0 ||
        picture_width > UINT16_MAX || picture_height > UINT16_MAX || num_render_targets < 0 ||
        num_render_targets > VAV4L2_MAX_SURFACES || (num_render_targets && !render_targets))
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    struct vav4l2_context *context = calloc(1, sizeof(*context));
    if (!context)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;

    pthread_mutex_lock(&vav4l2_api_lock);

    context->config_id = config_id;
    context->driver_data = driver_data;
    context->width = picture_width;
    context->height = picture_height;
    context->num_render_targets = num_render_targets;
    context->request_fd = -1;
    context->render_targets = num_render_targets ? calloc(num_render_targets, sizeof(VASurfaceID)) : NULL;
    if (num_render_targets && !context->render_targets) {
        free(context);
        pthread_mutex_unlock(&vav4l2_api_lock);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    if (num_render_targets)
        memcpy(context->render_targets, render_targets, (size_t)num_render_targets * sizeof(VASurfaceID));

    /* Find the config to get codec info */
    struct vav4l2_config *config = driver_data->configs;
    while (config && config->config_id != config_id)
        config = config->next;

    if (!config) {
        pthread_mutex_unlock(&vav4l2_api_lock);
        free(context->render_targets);
        free(context);
        return VA_STATUS_ERROR_INVALID_CONFIG;
    }
    context->codec = config->codec;
    context->profile = config->profile;
    context->entrypoint = config->entrypoint;
    context->rt_format = config->rt_format;
    /* Own copy: vaDestroyConfig frees the config node while live contexts
     * keep using the path (device reopen on recapture). */
    context->device_path = strdup(config->device_path);
    context->v4l2_fd = -1;
    context->media_fd = -1;
    context->request_fd = -1;

    if (!context->device_path)
        goto fail;
    /* Open V4L2 video device */
    context->v4l2_fd = vav4l2_open_device(context->device_path);
    if (context->v4l2_fd < 0) {
        fprintf(stderr, "v4l2: failed to open %s\n", context->device_path);
        goto fail;
    }

    int stateful = 0;
    if (context->codec <= VAV4L2_CODEC_MPEG2) {
        stateful = vav4l2_decoder_select(context->v4l2_fd, context->codec, &context->coded_format);
        if (stateful < 0) goto fail;
    }
    if (vav4l2_codec_backend_init(context) < 0) goto fail;
    if (context->codec <= VAV4L2_CODEC_MPEG2) {
        int result = stateful ? vav4l2_stateful_init(context) : vav4l2_stateless_init(context);
        if (result < 0) goto fail;
    }
    *context_id = ++driver_data->next_context_id;
    context->context_id = *context_id;
    context->next = driver_data->contexts;
    driver_data->contexts = context;

    pthread_mutex_unlock(&vav4l2_api_lock);
    return VA_STATUS_SUCCESS;

fail:
    vav4l2_release_context_device(context);
    free(context->device_path);
    free(context->render_targets);
    free(context);
    pthread_mutex_unlock(&vav4l2_api_lock);
    return VA_STATUS_ERROR_OPERATION_FAILED;
}

static VAStatus
vav4l2_destroy_context(VADriverContextP ctx, VAContextID context_id)
{
    struct vav4l2_driver_data *driver_data = ctx->pDriverData;
    struct vav4l2_context **pp;

    pthread_mutex_lock(&vav4l2_api_lock);
    pp = &driver_data->contexts;
    while (*pp) {
        if ((*pp)->context_id == context_id) {
            struct vav4l2_context *context = *pp;
            *pp = context->next;

            vav4l2_release_context_device(context);
            while (context->buffers) {
                struct vav4l2_buffer *buf = context->buffers;
                context->buffers = buf->next;
                vav4l2_free_buffer_locked(buf);
            }
            free(context->device_path);
            free(context->render_targets);
            free(context);
            pthread_mutex_unlock(&vav4l2_api_lock);
            return VA_STATUS_SUCCESS;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&vav4l2_api_lock);

    return VA_STATUS_ERROR_INVALID_CONTEXT;
}

/*
 * Buffers — parameter buffers for decode
 */

static VAStatus
vav4l2_create_buffer(VADriverContextP ctx,
                     VAContextID context_id,
                     VABufferType type,
                     unsigned int size,
                     unsigned int num_elements,
                     void *data,
                     VABufferID *buf_id)
{
    struct vav4l2_driver_data *driver_data = ctx->pDriverData;

    /* All submitted byte counts and coded-buffer capacities are uint32_t. */
    if (!buf_id || !size || !num_elements || size > UINT_MAX / num_elements)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    size_t bytes = (size_t)size * num_elements;
    if (type == VAEncCodedBufferType &&
        (num_elements != 1 || bytes > SIZE_MAX - sizeof(VACodedBufferSegment)))
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    struct vav4l2_buffer *buf = calloc(1, sizeof(*buf));
    if (!buf)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    buf->type = type;
    buf->size = size;
    buf->num_elements = num_elements;
    buf->allocated_elements = num_elements;
    if (type == VAEncCodedBufferType) {
        /* Layout: VACodedBufferSegment header followed by payload. */
        buf->data = calloc(1, sizeof(VACodedBufferSegment) + bytes);
    } else {
        buf->data = malloc(bytes);
    }
    if (!buf->data) {
        free(buf);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    if (type == VAEncCodedBufferType) {
        VACodedBufferSegment *seg = buf->data;
        seg->buf = (uint8_t *)buf->data + sizeof(*seg);
    } else if (data) {
        memcpy(buf->data, data, bytes);
    }

    pthread_mutex_lock(&vav4l2_api_lock);
    *buf_id = ++driver_data->next_buffer_id;
    buf->buffer_id = *buf_id;

    /* Attach to context */
    struct vav4l2_context *context = driver_data->contexts;
    while (context && context->context_id != context_id)
        context = context->next;

    if (context) {
        buf->next = context->buffers;
        context->buffers = buf;
    } else {
        /* Orphan buffer — store globally */
        buf->next = driver_data->orphan_buffers;
        driver_data->orphan_buffers = buf;
    }
    pthread_mutex_unlock(&vav4l2_api_lock);

    return VA_STATUS_SUCCESS;
}

static VAStatus
vav4l2_destroy_buffer(VADriverContextP ctx, VABufferID buf_id)
{
    struct vav4l2_driver_data *driver_data = ctx->pDriverData;

    pthread_mutex_lock(&vav4l2_api_lock);

    /* Search in all contexts */
    struct vav4l2_context *context = driver_data->contexts;
    while (context) {
        struct vav4l2_buffer **pp = &context->buffers;
        while (*pp) {
            if ((*pp)->buffer_id == buf_id) {
                struct vav4l2_buffer *buf = *pp;
                *pp = buf->next;
                for (int i = 0; i < context->num_pending_buffers; i++)
                    if (context->pending_buffers[i] == buf)
                        context->pending_buffers[i] = NULL;
                vav4l2_free_buffer_locked(buf);
                pthread_mutex_unlock(&vav4l2_api_lock);
                return VA_STATUS_SUCCESS;
            }
            pp = &(*pp)->next;
        }
        context = context->next;
    }

    /* Search orphan buffers */
    struct vav4l2_buffer **pp = &driver_data->orphan_buffers;
    while (*pp) {
        if ((*pp)->buffer_id == buf_id) {
            struct vav4l2_buffer *buf = *pp;
            *pp = buf->next;
            vav4l2_free_buffer_locked(buf);
            pthread_mutex_unlock(&vav4l2_api_lock);
            return VA_STATUS_SUCCESS;
        }
        pp = &(*pp)->next;
    }

    pthread_mutex_unlock(&vav4l2_api_lock);
    return VA_STATUS_ERROR_INVALID_BUFFER;
}

static VAStatus
vav4l2_map_buffer(VADriverContextP ctx, VABufferID buf_id, void **pbuff)
{
    if (!pbuff)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    struct vav4l2_driver_data *driver_data = ctx->pDriverData;

    /* Search all contexts + orphans */
    pthread_mutex_lock(&vav4l2_api_lock);
    struct vav4l2_context *context = driver_data->contexts;
    while (context) {
        struct vav4l2_buffer *buf = context->buffers;
        while (buf) {
            if (buf->buffer_id == buf_id) {
                    *pbuff = buf->data;
                pthread_mutex_unlock(&vav4l2_api_lock);
                return VA_STATUS_SUCCESS;
            }
            buf = buf->next;
        }
        context = context->next;
    }

    struct vav4l2_buffer *buf = driver_data->orphan_buffers;
    while (buf) {
        if (buf->buffer_id == buf_id) {
            *pbuff = buf->data;
            pthread_mutex_unlock(&vav4l2_api_lock);
            return VA_STATUS_SUCCESS;
        }
        buf = buf->next;
    }
    pthread_mutex_unlock(&vav4l2_api_lock);

    return VA_STATUS_ERROR_INVALID_BUFFER;
}

static struct vav4l2_buffer *vav4l2_find_buffer(struct vav4l2_driver_data *dd, VABufferID id);

static VAStatus
vav4l2_unmap_buffer(VADriverContextP ctx, VABufferID buf_id)
{
    struct vav4l2_driver_data *dd = ctx->pDriverData;
    pthread_mutex_lock(&vav4l2_api_lock);
    struct vav4l2_buffer *buf = vav4l2_find_buffer(dd, buf_id);
    VAStatus status = buf ? VA_STATUS_SUCCESS : VA_STATUS_ERROR_INVALID_BUFFER;
    if (buf && buf->mmapped == 2 && buf->borrow_surf) {
        struct vav4l2_surface *surf = buf->borrow_surf;
        surf->last_writer = buf->data == surf->cpu_ptr ? VAV4L2_WRITER_CPU : VAV4L2_WRITER_MEMFD;
        /* A GBM export may have been created after DeriveImage. Flush CPU
         * writes when unmapping so its display backing is not left stale. */
        if (surf->gbm_bo && vav4l2_gbm_surface_sync(surf) < 0)
            status = VA_STATUS_ERROR_OPERATION_FAILED;
    }
    pthread_mutex_unlock(&vav4l2_api_lock);
    return status;
}

/* Caller holds vav4l2_api_lock. */
static struct vav4l2_buffer *vav4l2_find_buffer(struct vav4l2_driver_data *dd, VABufferID id)
{
    for (struct vav4l2_context *c = dd->contexts; c; c = c->next)
        for (struct vav4l2_buffer *b = c->buffers; b; b = b->next)
            if (b->buffer_id == id)
                return b;
    for (struct vav4l2_buffer *b = dd->orphan_buffers; b; b = b->next)
        if (b->buffer_id == id)
            return b;
    return NULL;
}

static VAStatus
vav4l2_buffer_info(VADriverContextP ctx, VABufferID buf_id, VABufferType *type,
                   unsigned int *size, unsigned int *num_elements)
{
    if (!type || !size || !num_elements)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    struct vav4l2_driver_data *dd = ctx->pDriverData;
    pthread_mutex_lock(&vav4l2_api_lock);
    struct vav4l2_buffer *buf = vav4l2_find_buffer(dd, buf_id);
    VAStatus status = buf ? VA_STATUS_SUCCESS : VA_STATUS_ERROR_INVALID_BUFFER;
    if (buf) {
        *type = buf->type;
        *size = buf->size;
        *num_elements = buf->num_elements;
    }
    pthread_mutex_unlock(&vav4l2_api_lock);
    return status;
}

static VAStatus
vav4l2_buffer_set_num_elements(VADriverContextP ctx,
                               VABufferID buf_id,
                               unsigned int num_elements)
{
    struct vav4l2_driver_data *driver_data = ctx->pDriverData;
    VAStatus st = VA_STATUS_ERROR_INVALID_BUFFER;

    pthread_mutex_lock(&vav4l2_api_lock);
    struct vav4l2_buffer *buf = vav4l2_find_buffer(driver_data, buf_id);
    if (buf) {
        if (buf->mmapped || buf->type == VAImageBufferType ||
            (buf->type == VAEncCodedBufferType && num_elements != 1) ||
            (num_elements && buf->size > UINT_MAX / num_elements)) {
            st = VA_STATUS_ERROR_INVALID_PARAMETER;
            goto out;
        }
        if (num_elements > buf->allocated_elements) {
            void *new_data = realloc(buf->data, (size_t)buf->size * num_elements);
            if (!new_data) {
                st = VA_STATUS_ERROR_ALLOCATION_FAILED;
                goto out;
            }
            buf->data = new_data;
            buf->allocated_elements = num_elements;
        }
        buf->num_elements = num_elements;
        st = VA_STATUS_SUCCESS;
    }

out:
    pthread_mutex_unlock(&vav4l2_api_lock);
    return st;
}

/*
 * Decode pipeline — Begin/Render/End
 */

static VAStatus
vav4l2_begin_picture(VADriverContextP ctx,
                     VAContextID context_id,
                     VASurfaceID render_target)
{
    struct vav4l2_driver_data *driver_data = ctx->pDriverData;
    struct vav4l2_surface *surface;
    struct vav4l2_context *context;
    VAStatus st;

    pthread_mutex_lock(&vav4l2_api_lock);
    context = driver_data->contexts;
    while (context && context->context_id != context_id)
        context = context->next;

    if (!context) {
        st = VA_STATUS_ERROR_INVALID_CONTEXT;
        goto out;
    }

    if (context->current_surface) {
        st = VA_STATUS_ERROR_OPERATION_FAILED;
        goto out;
    }

    surface = vav4l2_surface_by_id(driver_data, render_target);
    if (!surface) {
        st = VA_STATUS_ERROR_INVALID_SURFACE;
        goto out;
    }

    /* Queued readers may outlive the application's previous SyncSurface.
     * Retargeting waits for them before releasing native reference storage. */
    for (struct vav4l2_context *c = driver_data->contexts; c; c = c->next) if (vav4l2_decode_pending(c, render_target)) {
        if (surface->status == VASurfaceRendering) { st = VA_STATUS_ERROR_SURFACE_BUSY; goto out; }
        st = vav4l2_decode_sync(c, surface, UINT64_C(5000000000));
        if (st != VA_STATUS_SUCCESS) goto out;
    }
    int field_continue = vav4l2_h264_field_pending(context, render_target);

    if ((surface->prime || surface->gbm_imported) && context->codec != VAV4L2_CODEC_VPP && context->codec != VAV4L2_CODEC_JPEG_ENC &&
        ((surface->format != VA_FOURCC_NV12 && surface->format != VA_FOURCC_P010 && surface->format != VA_FOURCC_YUY2 && surface->format != VA_FOURCC_Y210) ||
         !(context->rt_format & (surface->format == VA_FOURCC_P010 ? VA_RT_FORMAT_YUV420_10 :
           surface->format == VA_FOURCC_Y210 ? VA_RT_FORMAT_YUV422_10 :
           surface->format == VA_FOURCC_YUY2 ? VA_RT_FORMAT_YUV422 : VA_RT_FORMAT_YUV420)))) {
        st = VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT; goto out;
    }

    for (struct vav4l2_context *other = driver_data->contexts; other; other = other->next) {
        if (other != context && (other->current_surface == surface || vav4l2_h264_field_pending(other, render_target)) &&
            (context->codec != VAV4L2_CODEC_JPEG_ENC || other->codec != VAV4L2_CODEC_JPEG_ENC)) {
            st = VA_STATUS_ERROR_SURFACE_BUSY;
            goto out;
        }
    }

    if (context->codec != VAV4L2_CODEC_JPEG_ENC && surface->memfd_borrows) {
        st = VA_STATUS_ERROR_SURFACE_BUSY;
        goto out;
    }
    if (context->codec == VAV4L2_CODEC_VPP && !surface->prime && !surface->gbm_bo &&
        surface->has_pic && surface->last_writer != VAV4L2_WRITER_CPU) {
        if (vav4l2_surface_ensure_cpu(surface) < 0) {
            st = VA_STATUS_ERROR_ALLOCATION_FAILED;
            goto out;
        }
        struct vav4l2_buffer backing = {.data = surface->cpu_ptr, .fourcc = surface->format,
            .pitch = surface->cpu_stride, .size = surface->cpu_size,
            .image_width = surface->width, .image_height = surface->height};
        st = vav4l2_surface_read_image_locked(driver_data, surface, &backing, 0, 0, surface->width, surface->height);
        if (st != VA_STATUS_SUCCESS) goto out;
        surface->last_writer = VAV4L2_WRITER_CPU;
    }

    /* JPEG reads this surface. It must retain the decoder's capture slot,
     * reference timestamp and content status, even if encoding fails. */
    if (!field_continue && context->codec != VAV4L2_CODEC_JPEG_ENC && surface->buf_index >= 0) {
        struct vav4l2_context *owner = driver_data->contexts;
        while (owner && !vav4l2_surface_capture_matches(owner, surface))
            owner = owner->next;
        if (!owner) {
            st = VA_STATUS_ERROR_INVALID_SURFACE;
            goto out;
        }
        /* AV1 model slots cannot move to another context until their DPB
         * references are released. Keep that ownership intact. */
        int av1_model = owner->codec == VAV4L2_CODEC_AV1 && owner->av1.model_active;
        if (av1_model && owner != context) {
            st = VA_STATUS_ERROR_SURFACE_BUSY;
            goto out;
        }
        if (owner->stateful || owner != context || !vav4l2_expbuf_export_wanted()) {
            if (!av1_model)
                vav4l2_cap_pool_push(owner, surface->buf_index);
            surface->buf_index = -1;
            surface->cap_view = NULL;
            surface->capture_ctx = NULL;
        }
    }

    if (context->h264_field && !field_continue) vav4l2_h264_discard_field(context, 1);
    context->current_surface = surface;
    context->current_surface_id = render_target;
    context->num_pending_buffers = 0;
    if (context->codec != VAV4L2_CODEC_JPEG_ENC)
        surface->status = VASurfaceRendering;

    /* Only decoded pictures need a request and a new DPB timestamp. */
    if (context->codec != VAV4L2_CODEC_JPEG_ENC && context->codec != VAV4L2_CODEC_VPP) {
        if (vav4l2_request_begin(context) < 0) { context->current_surface = NULL; surface->status = VASurfaceSkipped; st = VA_STATUS_ERROR_DECODING_ERROR; goto out; }
        if (context->media_fd >= 0 && context->request_fd < 0)
            context->request_fd = vav4l2_request_alloc(context->media_fd);
        /* Nanoseconds in 1us steps, matching vb2's OUTPUT timeval. */
        if (!field_continue) surface->timestamp = (uint64_t)context->frame_count++ * 1000;
    }

    st = VA_STATUS_SUCCESS;

out:
    vav4l2_decode_trace(context, "begin-picture", (int)st, render_target);
    pthread_mutex_unlock(&vav4l2_api_lock);
    return st;
}

static VAStatus
vav4l2_render_picture(VADriverContextP ctx,
                      VAContextID context_id,
                      VABufferID *buffers,
                      int num_buffers)
{
    struct vav4l2_driver_data *driver_data = ctx->pDriverData;
    struct vav4l2_context *context;
    VAStatus st;

    pthread_mutex_lock(&vav4l2_api_lock);
    context = driver_data->contexts;
    while (context && context->context_id != context_id)
        context = context->next;

    if (!context) {
        pthread_mutex_unlock(&vav4l2_api_lock);
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }

    if (num_buffers < 0 || (num_buffers && !buffers) || !context->current_surface) {
        pthread_mutex_unlock(&vav4l2_api_lock);
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    if (num_buffers > 256 - context->num_pending_buffers) {
        pthread_mutex_unlock(&vav4l2_api_lock);
        return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
    }
    int first = context->num_pending_buffers;
    for (int i = 0; i < num_buffers; i++) {
        struct vav4l2_buffer *b = context->buffers;
        while (b && b->buffer_id != buffers[i])
            b = b->next;
        if (!b) {
            pthread_mutex_unlock(&vav4l2_api_lock);
            return VA_STATUS_ERROR_INVALID_BUFFER;
        }
        context->pending_buffers[first + i] = b;
    }
    context->num_pending_buffers += num_buffers;

    st = VA_STATUS_SUCCESS;
    pthread_mutex_unlock(&vav4l2_api_lock);
    return st;
}

/* Validate VA decode buffers before dispatching to either queue backend. */
static int valid_parameter_buffer(enum vav4l2_codec codec, const struct vav4l2_buffer *b)
{
    size_t need = 0;
    if (!b || !b->data || !b->num_elements)
        return 0;
    if (b->allocated_elements && b->num_elements > b->allocated_elements)
        return 0;
    if (codec == VAV4L2_CODEC_VPP && b->type != VAProcPipelineParameterBufferType)
        return 0;
    if (b->type == VAPictureParameterBufferType) {
        switch (codec) {
        case VAV4L2_CODEC_H264: need = sizeof(VAPictureParameterBufferH264); break;
        case VAV4L2_CODEC_HEVC: need = sizeof(VAPictureParameterBufferHEVC); break;
        case VAV4L2_CODEC_VP8: need = sizeof(VAPictureParameterBufferVP8); break;
        case VAV4L2_CODEC_VP9: need = sizeof(VADecPictureParameterBufferVP9); break;
        case VAV4L2_CODEC_MPEG2: need = sizeof(VAPictureParameterBufferMPEG2); break;
        case VAV4L2_CODEC_AV1: need = sizeof(VADecPictureParameterBufferAV1); break;
        default: return 0;
        }
    } else if (b->type == VASliceParameterBufferType) {
        switch (codec) {
        case VAV4L2_CODEC_H264: need = sizeof(VASliceParameterBufferH264); break;
        case VAV4L2_CODEC_HEVC: need = sizeof(VASliceParameterBufferHEVC); break;
        case VAV4L2_CODEC_VP8: need = sizeof(VASliceParameterBufferVP8); break;
        case VAV4L2_CODEC_VP9: need = sizeof(VASliceParameterBufferVP9); break;
        case VAV4L2_CODEC_MPEG2: need = sizeof(VASliceParameterBufferMPEG2); break;
        case VAV4L2_CODEC_AV1: need = sizeof(VASliceParameterBufferAV1); break;
        default: return 0;
        }
    } else if (b->type == VAIQMatrixBufferType) {
        switch (codec) {
        case VAV4L2_CODEC_H264: need = sizeof(VAIQMatrixBufferH264); break;
        case VAV4L2_CODEC_HEVC: need = sizeof(VAQMatrixBufferHEVC); break;
        case VAV4L2_CODEC_VP8: need = sizeof(VAIQMatrixBufferVP8); break;
        case VAV4L2_CODEC_MPEG2: need = sizeof(VAIQMatrixBufferMPEG2); break;
        default: return 0;
        }
    } else if (b->type == VAProbabilityBufferType) {
        if (codec != VAV4L2_CODEC_VP8) return 0;
        need = sizeof(VAProbabilityDataBufferVP8);
    } else if (b->type == VAEncPictureParameterBufferType) {
        if (codec != VAV4L2_CODEC_JPEG_ENC) return 0;
        need = sizeof(VAEncPictureParameterBufferJPEG);
    } else if (b->type == VAEncSliceParameterBufferType) {
        if (codec != VAV4L2_CODEC_JPEG_ENC || b->num_elements != 1) return 0;
        need = sizeof(VAEncSliceParameterBufferJPEG);
    } else if (b->type == VAQMatrixBufferType || b->type == VAHuffmanTableBufferType) {
        if (codec != VAV4L2_CODEC_JPEG_ENC || b->num_elements != 1) return 0;
        need = b->type == VAQMatrixBufferType ? sizeof(VAQMatrixBufferJPEG) : sizeof(VAHuffmanTableBufferJPEGBaseline);
    } else if (b->type == VAEncMiscParameterBufferType) {
        need = sizeof(VAEncMiscParameterBuffer);
        if (b->size < need) return 0;
        const VAEncMiscParameterBuffer *m = b->data;
        if (m->type == VAEncMiscParameterTypeQualityLevel)
            need += sizeof(VAEncMiscParameterBufferQualityLevel);
    } else if (b->type == VAProcPipelineParameterBufferType) {
        if (codec != VAV4L2_CODEC_VPP) return 0;
        need = sizeof(VAProcPipelineParameterBuffer);
    }
    return b->size >= need;
}

static VAStatus
vav4l2_end_picture(VADriverContextP ctx,
                   VAContextID context_id)
{
    struct vav4l2_driver_data *driver_data = ctx->pDriverData;
    struct vav4l2_context *context;
    VAStatus va_status;

    pthread_mutex_lock(&vav4l2_api_lock);
    context = driver_data->contexts;
    while (context && context->context_id != context_id)
        context = context->next;

    if (!context) {
        pthread_mutex_unlock(&vav4l2_api_lock);
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }

    if (!context->current_surface) {
        pthread_mutex_unlock(&vav4l2_api_lock);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    for (int i = 0; i < context->num_pending_buffers; i++) {
        if (!context->pending_buffers[i]) {
            vav4l2_h264_discard_field(context, 1);
            if (context->codec != VAV4L2_CODEC_JPEG_ENC)
                context->current_surface->status = VASurfaceSkipped;
            context->current_surface = NULL;
            context->num_pending_buffers = 0;
            pthread_mutex_unlock(&vav4l2_api_lock);
            return VA_STATUS_ERROR_INVALID_BUFFER;
        }
        if (!valid_parameter_buffer(context->codec, context->pending_buffers[i])) {
            vav4l2_h264_discard_field(context, 1);
            if (context->codec != VAV4L2_CODEC_JPEG_ENC)
                context->current_surface->status = VASurfaceSkipped;
            context->current_surface = NULL;
            context->num_pending_buffers = 0;
            pthread_mutex_unlock(&vav4l2_api_lock);
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        }
    }

    if (context->codec == VAV4L2_CODEC_JPEG_ENC) {
        va_status = vav4l2_jpeg_encode(context, context->pending_buffers,
                                       context->num_pending_buffers);
        for (int i = 0; i < context->num_pending_buffers; i++) {
            struct vav4l2_buffer *p = context->pending_buffers[i];
            if (p->type == VAEncPictureParameterBufferType) {
                const VAEncPictureParameterBufferJPEG *pic = p->data;
                for (struct vav4l2_buffer *b = context->buffers; b; b = b->next)
                    if (b->buffer_id == pic->coded_buf && b->type == VAEncCodedBufferType) {
                        b->completion_status = va_status;
                        if (va_status != VA_STATUS_SUCCESS && b->data) {
                            VACodedBufferSegment *segment = b->data;
                            segment->size = 0; segment->status |= VA_CODED_BUF_STATUS_BAD_BITSTREAM;
                        }
                    }
            }
        }
        context->current_surface = NULL;
        context->num_pending_buffers = 0;
        pthread_mutex_unlock(&vav4l2_api_lock);
        return va_status;
    }
    if (context->codec == VAV4L2_CODEC_VPP) {
        va_status = vav4l2_vpp_run(context, context->pending_buffers,
                                   context->num_pending_buffers);
        context->current_surface->status =
            (va_status == VA_STATUS_SUCCESS) ? VASurfaceReady : VASurfaceSkipped;
        context->current_surface = NULL;
        context->num_pending_buffers = 0;
        pthread_mutex_unlock(&vav4l2_api_lock);
        return va_status;
    }

    if (context->v4l2_fd < 0 || (!context->stateful && context->request_fd < 0)) {
        vav4l2_h264_discard_field(context, 1);
        fprintf(stderr, "v4l2: decode device or request unavailable\n");
        context->current_surface->status = VASurfaceSkipped;
        context->current_surface = NULL;
        context->num_pending_buffers = 0;
        if (context->request_fd >= 0) {
            close(context->request_fd);
            context->request_fd = -1;
        }
        pthread_mutex_unlock(&vav4l2_api_lock);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    /* Bytestream and Request translators share VA buffer validation. */
    if (context->stateful) va_status = vav4l2_stateful_translate(context, context->pending_buffers, context->num_pending_buffers);
    else va_status = vav4l2_stateless_translate(context, context->pending_buffers, context->num_pending_buffers);

    if (va_status == VA_STATUS_SUCCESS && context->current_surface->status == VASurfaceSkipped)
        va_status = VA_STATUS_ERROR_DECODING_ERROR;
    vav4l2_decode_trace(context, "end-picture", (int)va_status, context->num_pending_buffers);
    if (va_status != VA_STATUS_SUCCESS) {
        vav4l2_h264_discard_field(context, 1);
        fprintf(stderr, "v4l2: decode translate failed: %d\n", va_status);
        /* Clean up and return */
        context->current_surface->status = VASurfaceSkipped;
        context->current_surface = NULL;
        context->num_pending_buffers = 0;
        if (context->request_fd >= 0) {
            close(context->request_fd);
            context->request_fd = -1;
        }
        pthread_mutex_unlock(&vav4l2_api_lock);
        return va_status;
    }

    /* Queued pictures retain Rendering until both queues and their request complete. */
    if (!context->stateful) context->current_surface->status = context->h264_field || vav4l2_request_pending(context, context->current_surface_id) ? VASurfaceRendering : VASurfaceReady;
    context->current_surface = NULL;
    context->num_pending_buffers = 0;

    pthread_mutex_unlock(&vav4l2_api_lock);
    /* request_fd lifecycle is owned by decode_submit / begin_picture */
    return VA_STATUS_SUCCESS;
}

/*
 * Surface sync/status — wait for V4L2 decode to complete
 */
static VAStatus
vav4l2_sync_surface(VADriverContextP ctx, VASurfaceID render_target)
{
    struct vav4l2_driver_data *driver_data = ctx->pDriverData;
    struct vav4l2_surface *surface;

    pthread_mutex_lock(&vav4l2_api_lock);
    surface = vav4l2_surface_by_id(driver_data, render_target);
    if (!surface) {
        if (vav4l2_debug)
            fprintf(stderr, "v4l2: sync lookup fail %#x\n",
                    render_target);
        pthread_mutex_unlock(&vav4l2_api_lock);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    for (struct vav4l2_context *c = driver_data->contexts; c; c = c->next) if (vav4l2_decode_pending(c, render_target)) {
        VAStatus result = vav4l2_decode_sync(c, surface, UINT64_C(5000000000));
        pthread_mutex_unlock(&vav4l2_api_lock); return result;
    }
    /* Both decoder protocols resolve pending work through the wait above. */
    if (surface->status == VASurfaceSkipped) {
        pthread_mutex_unlock(&vav4l2_api_lock);
        return VA_STATUS_ERROR_DECODING_ERROR;
    }
    if (surface->status == VASurfaceRendering) {
        pthread_mutex_unlock(&vav4l2_api_lock);
        return VA_STATUS_ERROR_SURFACE_BUSY;
    }
    surface->status = VASurfaceReady;

    pthread_mutex_unlock(&vav4l2_api_lock);
    return VA_STATUS_SUCCESS;
}

/* Include time spent waiting for the global lock in the timeout budget. */
static VAStatus vav4l2_sync_object(VADriverContextP ctx, unsigned int id, uint64_t timeout, int buffer)
{
    struct timespec start, now;
    if (clock_gettime(CLOCK_MONOTONIC, &start)) return VA_STATUS_ERROR_OPERATION_FAILED;
    for (;;) {
        int lock_status = pthread_mutex_trylock(&vav4l2_api_lock);
        if (!lock_status) {
            struct vav4l2_driver_data *dd = ctx->pDriverData;
            VAStatus status = VA_STATUS_SUCCESS;
            if (buffer) {
                struct vav4l2_buffer *b = vav4l2_find_buffer(dd, id);
                if (!b) status = VA_STATUS_ERROR_INVALID_BUFFER;
                else {
                    status = b->completion_status;
                    for (struct vav4l2_context *c = dd->contexts; c; c = c->next) {
                        if (!c->current_surface) continue;
                        for (int i = 0; i < c->num_pending_buffers; i++) {
                            struct vav4l2_buffer *p = c->pending_buffers[i];
                            if (p == b) status = VA_STATUS_ERROR_TIMEDOUT;
                            if (p && p->data && c->codec == VAV4L2_CODEC_JPEG_ENC && p->type == VAEncPictureParameterBufferType &&
                                p->size >= sizeof(VAEncPictureParameterBufferJPEG) &&
                                ((VAEncPictureParameterBufferJPEG *)p->data)->coded_buf == id) status = VA_STATUS_ERROR_TIMEDOUT;
                        }
                    }
                }
            } else {
                struct vav4l2_surface *s = vav4l2_surface_by_id(dd, id);
                if (!s) status = VA_STATUS_ERROR_INVALID_SURFACE;
                else if (s->status == VASurfaceRendering) {
                    status = VA_STATUS_ERROR_TIMEDOUT;
                    for (struct vav4l2_context *c = dd->contexts; c; c = c->next) if (vav4l2_decode_pending(c, id)) {
                        if (clock_gettime(CLOCK_MONOTONIC, &now)) { status = VA_STATUS_ERROR_OPERATION_FAILED; break; }
                        uint64_t spent = (uint64_t)(now.tv_sec - start.tv_sec) * UINT64_C(1000000000) + now.tv_nsec - start.tv_nsec;
                        uint64_t left = timeout == VA_TIMEOUT_INFINITE ? timeout : spent >= timeout ? 0 : timeout - spent;
                        status = vav4l2_decode_sync(c, s, left); break;
                    }
                }
                else if (s->status == VASurfaceSkipped) status = VA_STATUS_ERROR_DECODING_ERROR;
            }
            pthread_mutex_unlock(&vav4l2_api_lock);
            if (status != VA_STATUS_ERROR_TIMEDOUT) return status;
        } else if (lock_status != EBUSY) return VA_STATUS_ERROR_OPERATION_FAILED;
        if (clock_gettime(CLOCK_MONOTONIC, &now)) return VA_STATUS_ERROR_OPERATION_FAILED;
        uint64_t elapsed = (uint64_t)(now.tv_sec - start.tv_sec) * UINT64_C(1000000000);
        if (now.tv_nsec >= start.tv_nsec) elapsed += now.tv_nsec - start.tv_nsec;
        else elapsed -= start.tv_nsec - now.tv_nsec;
        if (timeout != VA_TIMEOUT_INFINITE && elapsed >= timeout) return VA_STATUS_ERROR_TIMEDOUT;
        uint64_t pause = timeout == VA_TIMEOUT_INFINITE || timeout - elapsed > 1000000 ? 1000000 : timeout - elapsed;
        struct timespec wait = {.tv_nsec = (long)pause};
        nanosleep(&wait, NULL);
    }
}

static VAStatus vav4l2_sync_surface2(VADriverContextP ctx, VASurfaceID surface, uint64_t timeout)
{
    return vav4l2_sync_object(ctx, surface, timeout, 0);
}

static VAStatus vav4l2_sync_buffer(VADriverContextP ctx, VABufferID buffer, uint64_t timeout)
{
    return vav4l2_sync_object(ctx, buffer, timeout, 1);
}

static VAStatus
vav4l2_query_surface_status(VADriverContextP ctx,
                            VASurfaceID render_target,
                            VASurfaceStatus *status)
{
    struct vav4l2_driver_data *driver_data = ctx->pDriverData;
    struct vav4l2_surface *surface;
    VAStatus st = VA_STATUS_ERROR_INVALID_SURFACE;

    if (!status) return VA_STATUS_ERROR_INVALID_PARAMETER;

    pthread_mutex_lock(&vav4l2_api_lock);
    surface = vav4l2_surface_by_id(driver_data, render_target);
    if (surface) {
        for (struct vav4l2_context *c = driver_data->contexts; c; c = c->next)
            if (vav4l2_decode_pending(c, render_target)) (void)vav4l2_decode_pump(c);
        *status = surface->status;
        st = VA_STATUS_SUCCESS;
    }
    pthread_mutex_unlock(&vav4l2_api_lock);
    return st;
}

static VAStatus
vav4l2_query_surface_error(VADriverContextP ctx,
                           VASurfaceID render_target,
                           VAStatus error_status,
                           void **error_info)
{
    (void)error_status;
    if (!error_info) return VA_STATUS_ERROR_INVALID_PARAMETER;
    *error_info = NULL;
    pthread_mutex_lock(&vav4l2_api_lock);
    VAStatus st = vav4l2_surface_by_id(ctx->pDriverData, render_target) ?
        VA_STATUS_SUCCESS : VA_STATUS_ERROR_INVALID_SURFACE;
    pthread_mutex_unlock(&vav4l2_api_lock);
    return st;
}

/*
 * Image / DMA-BUF access
 *
 * DeriveImage exposes directly writable surface backing. A disconnected
 * snapshot is only suitable for GetImage, not a successful DeriveImage.
 */

static VAStatus
vav4l2_vpp_query_filters_wrap(VADriverContextP ctx, VAContextID context,
                              VAProcFilterType *filters, unsigned int *num_filters)
{
    (void)ctx;
    (void)context;
    return vav4l2_vpp_query_filters(filters, num_filters);
}

static VAStatus
vav4l2_vpp_query_filter_caps_wrap(VADriverContextP ctx, VAContextID context,
                                  VAProcFilterType type, void *filter_caps,
                                  unsigned int *num_filter_caps)
{
    (void)ctx;
    (void)context;
    return vav4l2_vpp_query_filter_caps(type, filter_caps, num_filter_caps);
}

static VAStatus
vav4l2_vpp_query_pipeline_caps_wrap(VADriverContextP ctx, VAContextID context,
                                    VABufferID *filters, unsigned int num_filters,
                                    VAProcPipelineCaps *pipeline_caps)
{
    (void)filters;
    if (num_filters) return VA_STATUS_ERROR_UNSUPPORTED_FILTER;
    struct vav4l2_driver_data *dd = ctx->pDriverData;
    pthread_mutex_lock(&vav4l2_api_lock);
    struct vav4l2_context *c = dd->contexts;
    while (c && c->context_id != context) c = c->next;
    VAStatus st = !c || c->codec != VAV4L2_CODEC_VPP ? VA_STATUS_ERROR_INVALID_CONTEXT :
        vav4l2_vpp_query_device_caps(dd, pipeline_caps);
    pthread_mutex_unlock(&vav4l2_api_lock);
    return st;
}

static VAStatus
vav4l2_export_surface_handle2(VADriverContextP ctx, VASurfaceID surface_id,
                             uint32_t mem_type, uint32_t flags, void *descriptor)
{
    struct vav4l2_driver_data *driver_data = ctx->pDriverData;
    struct vav4l2_surface *surf;
    struct vav4l2_context *c;
    VAStatus st;

    pthread_mutex_lock(&vav4l2_api_lock);
    surf = vav4l2_surface_by_id(driver_data, surface_id);

    if (!surf) {
        pthread_mutex_unlock(&vav4l2_api_lock);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    if (!descriptor) {
        pthread_mutex_unlock(&vav4l2_api_lock);
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    if (mem_type != VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2) {
        pthread_mutex_unlock(&vav4l2_api_lock);
        return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
    }
    if ((flags & ~(VA_EXPORT_SURFACE_READ_WRITE | VA_EXPORT_SURFACE_SEPARATE_LAYERS | VA_EXPORT_SURFACE_COMPOSED_LAYERS)) ||
        (flags & (VA_EXPORT_SURFACE_SEPARATE_LAYERS | VA_EXPORT_SURFACE_COMPOSED_LAYERS)) ==
        (VA_EXPORT_SURFACE_SEPARATE_LAYERS | VA_EXPORT_SURFACE_COMPOSED_LAYERS)) {
        pthread_mutex_unlock(&vav4l2_api_lock);
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    if (surf->status == VASurfaceRendering || ((flags & VA_EXPORT_SURFACE_WRITE_ONLY) && surf->memfd_borrows)) {
        pthread_mutex_unlock(&vav4l2_api_lock); return VA_STATUS_ERROR_SURFACE_BUSY;
    }
    if (flags & VA_EXPORT_SURFACE_WRITE_ONLY) {
        for (c = driver_data->contexts; c; c = c->next) {
            if (vav4l2_decode_pending(c, surface_id)) {
                pthread_mutex_unlock(&vav4l2_api_lock); return VA_STATUS_ERROR_SURFACE_BUSY;
            }
        }
    }
    if (surf->prime) {
        st = vav4l2_prime_export(surf, flags, descriptor);
        pthread_mutex_unlock(&vav4l2_api_lock); return st;
    }
    if (surf->format == VA_FOURCC_NV12 && surf->has_pic && surf->last_writer != VAV4L2_WRITER_CPU &&
        surf->cap_fourcc && surf->cap_fourcc != V4L2_PIX_FMT_NV12) {
        pthread_mutex_unlock(&vav4l2_api_lock); return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }
    /* Writable handles use a persistent GBM backing, never a live VPU DPB
     * allocation. Every subsequent VA read refreshes this external backing. */
    if (surf->format != VA_FOURCC_NV12 && surf->has_pic && surf->last_writer != VAV4L2_WRITER_CPU) {
        if (vav4l2_surface_ensure_cpu(surf) < 0) {
            pthread_mutex_unlock(&vav4l2_api_lock); return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
        struct vav4l2_buffer backing = {.data = surf->cpu_ptr, .fourcc = surf->format,
            .pitch = surf->cpu_stride, .size = surf->cpu_size, .image_width = surf->width, .image_height = surf->height};
        st = vav4l2_surface_read_image_locked(driver_data, surf, &backing, 0, 0, surf->width, surf->height);
        if (st != VA_STATUS_SUCCESS) { pthread_mutex_unlock(&vav4l2_api_lock); return st; }
        if (surf->buf_index >= 0 && surf->last_writer != VAV4L2_WRITER_CPU) surf->prime_capture_valid = 1;
    }
    /*
     * Decode surfaces: EXPBUF single-object NV12/P010 capture storage. Chrome exports the pool before the first picture; we
     * claim a capture index at first export and decode into that same
     * slot. VAV4L2_EXPBUF_EXPORT=0 restores the GBM-copy fallback.
     */
    c = context_owning_capture(driver_data, surf);
    vav4l2_explog(
            "v4l2: export surf=%u idx=%d view=%p wanted=%d c=%p fd=%d\n",
            (unsigned)surface_id, surf->buf_index, surf->cap_view,
            vav4l2_expbuf_export_wanted(), (void *)c,
            c ? c->v4l2_fd : -1);
    fflush(stderr);
    /* Stateful completion chooses CAPTURE by timestamp, not by VA target.
     * Export persistent surface storage so retained imports see later frames. */
    if (c && !c->stateful && vav4l2_capture_layout_exportable(c) && surf->format == VA_FOURCC_NV12 && !surf->gbm_imported && !surf->gbm_writable &&
        !(flags & VA_EXPORT_SURFACE_WRITE_ONLY) && surf->last_writer != VAV4L2_WRITER_CPU && vav4l2_expbuf_export_wanted()) {
        if (surf->buf_index < 0 && vav4l2_claim_capture_for_export(c, surf) < 0) {
            pthread_mutex_unlock(&vav4l2_api_lock); return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        st = vav4l2_capture_layout_export(c, surf, flags, descriptor);
        pthread_mutex_unlock(&vav4l2_api_lock); return st;
    }
    if (!surf->gbm_imported && !surf->gbm_writable && !(flags & VA_EXPORT_SURFACE_WRITE_ONLY) &&
        (surf->format == VA_FOURCC_NV12 || (surf->format == VA_FOURCC_P010 && surf->cap_fourcc == V4L2_PIX_FMT_P010)) &&
        vav4l2_expbuf_export_wanted() && c && !c->stateful && !c->capture_layout && c->v4l2_fd >= 0 &&
        (!c->cap_pixelformat || c->cap_pixelformat == V4L2_PIX_FMT_NV12 || c->cap_pixelformat == V4L2_PIX_FMT_P010) &&
        surf->last_writer != VAV4L2_WRITER_CPU &&
        (!surf->cap_fourcc || surf->cap_fourcc == V4L2_PIX_FMT_NV12 || surf->cap_fourcc == V4L2_PIX_FMT_P010)) {
        if (surf->buf_index < 0)
            (void)vav4l2_claim_capture_for_export(c, surf);
        int efd = (surf->buf_index >= 0)
                  ? vav4l2_capture_expbuf(c, surf->buf_index) : -1;
        if (efd >= 0) {
            uint32_t stride = surf->stride ? surf->stride :
                              (c->cap_stride ? c->cap_stride : surf->width);
            uint32_t alh = surf->aligned_h ? surf->aligned_h :
                           (c->cap_height ? c->cap_height : surf->height);
            uint32_t plane = vav4l2_capture_plane_size(
                surf->cap_fourcc ? surf->cap_fourcc : V4L2_PIX_FMT_NV12,
                stride, alh);
            uint32_t object_size;
            if (stride < (surf->width + (surf->width & 1)) * (surf->format == VA_FOURCC_P010 ? 2u : 1u) || alh < surf->height ||
                vav4l2_export_fd_validate(efd, plane > c->cap_sizeimage ? plane : c->cap_sizeimage, &object_size) < 0) {
                close(efd);
                pthread_mutex_unlock(&vav4l2_api_lock);
                return VA_STATUS_ERROR_OPERATION_FAILED;
            }
            /* The descriptor transfers fd ownership to the caller. */
            vav4l2_explog("v4l2: EXPBUF export ok idx=%d fd=%d stride=%u alh=%u\n",
                    surf->buf_index, efd, stride, alh);
            fflush(stderr);
            vav4l2_fill_prime_layers(descriptor, efd, object_size,
                                     DRM_FORMAT_MOD_LINEAR,
                                     surf->format, surf->width, surf->height,
                                     stride, alh,
                                     surf->format == VA_FOURCC_P010 ? DRM_FORMAT_P010 : DRM_FORMAT_NV12,
                                     surf->format == VA_FOURCC_P010 ? DRM_FORMAT_R16 : DRM_FORMAT_R8,
                                     surf->format == VA_FOURCC_P010 ? DRM_FORMAT_GR1616 : DRM_FORMAT_GR88, flags);
            pthread_mutex_unlock(&vav4l2_api_lock);
            return VA_STATUS_SUCCESS;
        }
        if (surf->buf_index >= 0 && errno == EPROTO) {
            pthread_mutex_unlock(&vav4l2_api_lock); return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        fprintf(stderr, "v4l2: EXPBUF export failed, falling back\n");
    }
    if (vav4l2_gbm_surface_ensure_export(surf, flags) < 0) {
        pthread_mutex_unlock(&vav4l2_api_lock);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    if ((flags & VA_EXPORT_SURFACE_WRITE_ONLY) && vav4l2_gbm_enable_write(surf) < 0) {
        pthread_mutex_unlock(&vav4l2_api_lock); return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    st = vav4l2_surface_fill_prime_gbm(surf, flags, descriptor);
    pthread_mutex_unlock(&vav4l2_api_lock);
    return st;
}

/* PRIME3 adds flags/reserved fields to the PRIME2 payload. Publish a fully
 * initialized descriptor only after a successful export; FD ownership is identical. */
static VAStatus
vav4l2_export_surface_handle(VADriverContextP ctx, VASurfaceID surface_id,
                             uint32_t mem_type, uint32_t flags, void *descriptor)
{
    if (mem_type != VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_3)
        return vav4l2_export_surface_handle2(ctx, surface_id, mem_type, flags, descriptor);
    if (!descriptor) return VA_STATUS_ERROR_INVALID_PARAMETER;
    VADRMPRIMESurfaceDescriptor d;
    VAStatus status = vav4l2_export_surface_handle2(ctx, surface_id, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, flags, &d);
    if (status == VA_STATUS_SUCCESS) {
        VADRMPRIME3SurfaceDescriptor v3 = {0}; memcpy(&v3, &d, offsetof(VADRMPRIME3SurfaceDescriptor, flags));
        memcpy(descriptor, &v3, sizeof(v3));
    }
    return status;
}

/*
 * Driver initialization
 */

static VAStatus
vav4l2_init(VADriverContextP ctx)
{
    struct vav4l2_driver_data *driver_data;

    driver_data = calloc(1, sizeof(*driver_data));
    if (!driver_data)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;

    driver_data->gbm.fd = -1;
    driver_data->gbm.drm_fd = ctx->drm_state ? ((struct drm_state *)ctx->drm_state)->fd : -1;
    ctx->pDriverData = driver_data;

    vav4l2_scan_all_cached(driver_data->dev_h264, driver_data->dev_hevc,
                           driver_data->dev_av1, driver_data->dev_vp8,
                           driver_data->dev_mpeg2,
                           driver_data->dev_jpeg_enc, driver_data->dev_vpp, driver_data->dev_vp9,
                           sizeof(driver_data->dev_h264));
    if (driver_data->dev_h264[0])
        driver_data->h264_profiles_known = vav4l2_query_h264_profiles(driver_data->dev_h264, &driver_data->h264_profiles) == 0;
    if (driver_data->dev_hevc[0])
        driver_data->hevc_profiles_known = vav4l2_query_profiles(driver_data->dev_hevc, V4L2_CID_MPEG_VIDEO_HEVC_PROFILE, &driver_data->hevc_profiles) == 0;
    if (driver_data->dev_vp9[0])
        driver_data->vp9_profiles_known = vav4l2_query_profiles(driver_data->dev_vp9, V4L2_CID_MPEG_VIDEO_VP9_PROFILE, &driver_data->vp9_profiles) == 0;
    fprintf(stderr, "v4l2: probe VP9    -> %s\n", driver_data->dev_vp9[0] ? driver_data->dev_vp9 : "(none)");
    fprintf(stderr, "v4l2: probe H.264  -> %s\n",
            driver_data->dev_h264[0] ? driver_data->dev_h264 : "(none)");
    fprintf(stderr, "v4l2: probe HEVC   -> %s\n",
            driver_data->dev_hevc[0] ? driver_data->dev_hevc : "(none)");
    fprintf(stderr, "v4l2: probe AV1    -> %s\n",
            driver_data->dev_av1[0] ? driver_data->dev_av1 : "(none)");
    fprintf(stderr, "v4l2: probe VP8    -> %s\n",
            driver_data->dev_vp8[0] ? driver_data->dev_vp8 : "(none)");
    fprintf(stderr, "v4l2: probe MPEG-2 -> %s\n",
            driver_data->dev_mpeg2[0] ? driver_data->dev_mpeg2 : "(none)");
    fprintf(stderr, "v4l2: probe JPEG   -> %s\n",
            driver_data->dev_jpeg_enc[0] ? driver_data->dev_jpeg_enc : "(none)");
    fprintf(stderr, "v4l2: probe VPP    -> %s\n",
            driver_data->dev_vpp[0] ? driver_data->dev_vpp : "(none)");

    /* Set context limits. Chrome sizes vaQueryConfigAttributes from
     * vaMaxNumConfigAttributes() == max_attributes. */
    ctx->max_profiles = NUM_PROFILES;
    ctx->max_entrypoints = 4;
    ctx->max_attributes = 32;
    ctx->max_image_formats = 22;
    ctx->max_subpic_formats = 1;
    ctx->max_display_attributes = 0;
    ctx->str_vendor = "libva-v4l2 0.1.0";

    /* Fill supported callbacks and typed unsupported-operation stubs. */
    struct VADriverVTable *vtable = ctx->vtable;
    memset(vtable, 0, sizeof(*vtable));

    vtable->vaTerminate               = vav4l2_terminate;
    vtable->vaQueryConfigProfiles     = vav4l2_query_config_profiles;
    vtable->vaQueryConfigEntrypoints  = vav4l2_query_config_entrypoints;
    vtable->vaGetConfigAttributes     = vav4l2_get_config_attributes;
    vtable->vaCreateConfig            = vav4l2_create_config;
    vtable->vaDestroyConfig           = vav4l2_destroy_config;
    vtable->vaQueryConfigAttributes   = vav4l2_query_config_attributes;
    vtable->vaCreateSurfaces          = vav4l2_create_surfaces;
    vtable->vaCreateSurfaces2         = vav4l2_create_surfaces2;
    vtable->vaDestroySurfaces         = vav4l2_destroy_surfaces;
    vtable->vaCreateContext           = vav4l2_create_context;
    vtable->vaDestroyContext          = vav4l2_destroy_context;
    vtable->vaCreateBuffer            = vav4l2_create_buffer;
    vtable->vaDestroyBuffer           = vav4l2_destroy_buffer;
    vtable->vaMapBuffer               = vav4l2_map_buffer;
    vtable->vaUnmapBuffer             = vav4l2_unmap_buffer;
    vtable->vaBufferSetNumElements    = vav4l2_buffer_set_num_elements;
    vtable->vaBufferInfo              = vav4l2_buffer_info;
    vtable->vaBeginPicture            = vav4l2_begin_picture;
    vtable->vaRenderPicture           = vav4l2_render_picture;
    vtable->vaEndPicture              = vav4l2_end_picture;
    vtable->vaSyncSurface             = vav4l2_sync_surface;
    vtable->vaSyncSurface2            = vav4l2_sync_surface2;
    vtable->vaSyncBuffer              = vav4l2_sync_buffer;
    vtable->vaQuerySurfaceAttributes  = vav4l2_query_surface_attributes;
    vtable->vaQuerySurfaceStatus      = vav4l2_query_surface_status;
    vtable->vaQuerySurfaceError       = vav4l2_query_surface_error;
    vtable->vaQueryImageFormats       = vav4l2_query_image_formats;
    vtable->vaCreateImage             = vav4l2_create_image;
    vtable->vaDeriveImage             = vav4l2_derive_image;
    vtable->vaDestroyImage            = vav4l2_destroy_image;
    vtable->vaSetImagePalette         = vav4l2_set_image_palette;
    vtable->vaGetImage                = vav4l2_get_image;
    vtable->vaPutImage                = vav4l2_put_image;
    vtable->vaQuerySubpictureFormats  = vav4l2_query_subpicture_formats;
    vtable->vaCreateSubpicture        = vav4l2_create_subpicture;
    vtable->vaDestroySubpicture       = vav4l2_destroy_subpicture;
    vtable->vaSetSubpictureImage      = vav4l2_set_subpicture_image;
    vtable->vaSetSubpictureChromakey  = vav4l2_set_subpicture_chromakey;
    vtable->vaSetSubpictureGlobalAlpha = vav4l2_set_subpicture_alpha;
    vtable->vaAssociateSubpicture     = vav4l2_associate_subpicture;
    vtable->vaDeassociateSubpicture   = vav4l2_deassociate_subpicture;
    vtable->vaQueryDisplayAttributes  = vav4l2_query_display_attributes;
    vtable->vaGetDisplayAttributes    = vav4l2_display_attributes;
    vtable->vaSetDisplayAttributes    = vav4l2_display_attributes;
    vtable->vaPutSurface              = vav4l2_put_surface;
    vtable->vaExportSurfaceHandle     = vav4l2_export_surface_handle;

    if (ctx->vtable_vpp) {
        ctx->vtable_vpp->version = VA_DRIVER_VTABLE_VPP_VERSION;
        ctx->vtable_vpp->vaQueryVideoProcFilters = vav4l2_vpp_query_filters_wrap;
        ctx->vtable_vpp->vaQueryVideoProcFilterCaps = vav4l2_vpp_query_filter_caps_wrap;
        ctx->vtable_vpp->vaQueryVideoProcPipelineCaps = vav4l2_vpp_query_pipeline_caps_wrap;
    }

    pthread_mutex_lock(&vav4l2_api_lock);
    if (!g_driver_instances) vav4l2_debug = !!getenv("VAV4L2_DEBUG");
    g_driver_instances++;
    pthread_mutex_unlock(&vav4l2_api_lock);
    return VA_STATUS_SUCCESS;
}

/*
 * libva entry point
 */
VAStatus __attribute__((visibility("default"))) VAV4L2_DRIVER_INIT(VADriverContextP ctx)
{
    return vav4l2_init(ctx);
}
