/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* An independently compiled source module used through the production hooks. */
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
int vareq_test_storage_fault, vareq_test_rejected_fd = -1, vareq_test_coherent;
#include "compat/compat.h"
static int matches(const struct vareq_compat_identity *id)
{
    return !strcmp(id->driver, "fixture-driver") && !strcmp(id->card, "fixture-scaler");
}
static int job_score(const struct vareq_compat_identity *id, const struct vareq_compat_job *job)
{
    (void)id;
    return job->compose ? -1 : 25;
}
static int storage_score(const struct vareq_compat_identity *id, unsigned bytes, unsigned rows, int coherent)
{
    (void)id; (void)bytes; (void)rows; (void)coherent;
    return 100;
}
static int allocate(const struct vareq_compat_identity *id, const char *node, unsigned bytes, unsigned rows, int coherent, const struct vareq_storage_ops *ops)
{
    (void)id;
    struct v4l2_format f = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE};
    f.fmt.pix_mp.width = bytes; f.fmt.pix_mp.height = rows; f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_GREY;
    f.fmt.pix_mp.field = V4L2_FIELD_NONE; f.fmt.pix_mp.num_planes = 1;
    vareq_test_coherent = coherent;
    int fd = ops->export_capture(node, &f, bytes, rows, 0, "fixture-exported");
    if (fd >= 0 && vareq_test_storage_fault) {
        vareq_test_rejected_fd = fd;
        int ret = vareq_test_storage_fault == 1 ? ftruncate(fd, 1) : fcntl(fd, F_SETFD, 0);
        if (ret < 0) { close(fd); return -1; }
    }
    return fd;
}
const struct vareq_compat_module vareq_test_compat = {
    .api_version = VAREQ_COMPAT_API_VERSION, .name = "fixture-extension", .match = matches,
    .behaviors = VAREQ_COMPAT_SWAP_MIRROR_AXES, .vpp_score = job_score,
    .storage_score = storage_score, .allocate = allocate
};

#include <stdlib.h>
#include <errno.h>
#include <va/va.h>
int vareq_test_codec_fault, vareq_test_codec_setups, vareq_test_codec_finishes, vareq_test_codec_formats, vareq_test_codec_controls;
static int codec_match(const struct vareq_compat_identity *id)
{
    return !strcmp(id->driver, "fixture-driver") && !strcmp(id->card, "fixture-codec");
}
static int codec_score(const struct vareq_compat_identity *id, const struct vareq_compat_codec *codec)
{
    (void)id;
    return codec->coded_format == V4L2_PIX_FMT_VP8_FRAME || (codec->profile_known && codec->va_profile == VAProfileH264High10) ? -1 : 30;
}
static int codec_setup(const struct vareq_compat_identity *id, const struct vareq_compat_codec *codec, int fd, void **state)
{
    (void)id; (void)fd;
    unsigned *value = malloc(sizeof(*value));
    if (!value) return -1;
    *value = codec->direction; *state = value; vareq_test_codec_setups++;
    if (vareq_test_codec_fault == 1) { errno = EIO; return -1; }
    return 0;
}
static void codec_finish(const struct vareq_compat_identity *id, const struct vareq_compat_codec *codec, int fd, void *state)
{
    (void)id; (void)fd;
    if (state) { if (*(unsigned *)state != (unsigned)codec->direction) abort(); free(state); }
    vareq_test_codec_finishes++;
}
static int codec_format(const struct vareq_compat_identity *id, const struct vareq_compat_codec *codec, void *state, struct v4l2_format *format)
{
    (void)id;
    if (!state || *(unsigned *)state != (unsigned)codec->direction) abort();
    vareq_test_codec_formats++;
    if (vareq_test_codec_fault == 2) format->fmt.pix_mp.pixelformat = V4L2_PIX_FMT_GREY;
    else format->fmt.pix_mp.plane_fmt[0].sizeimage += 4096;
    return 0;
}
static int codec_controls(const struct vareq_compat_identity *id, const struct vareq_compat_codec *codec, void *state, struct v4l2_ext_controls *controls)
{
    (void)id;
    if (!state || *(unsigned *)state != (unsigned)codec->direction) abort();
    vareq_test_codec_controls++;
    if (vareq_test_codec_fault == 3) controls->request_fd++;
    else if (vareq_test_codec_fault == 4 && controls->count) controls->controls[0].id++;
    else if (vareq_test_codec_fault == 5) { errno = ENOTSUP; return -1; }
    else for (unsigned i = 0; i < controls->count; i++) {
        if (controls->controls[i].size) {
            unsigned char *bytes = controls->controls[i].ptr;
            if (bytes) bytes[0] = 17;
        } else controls->controls[i].value += 7;
    }
    return 0;
}
const struct vareq_compat_module vareq_test_codec_compat = {
    .api_version = VAREQ_COMPAT_API_VERSION, .name = "fixture-codec-extension", .match = codec_match,
    .codec_score = codec_score, .codec_setup = codec_setup, .codec_finish = codec_finish,
    .codec_format = codec_format, .codec_controls = codec_controls
};
