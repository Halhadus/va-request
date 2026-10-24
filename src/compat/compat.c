/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <stdlib.h>
#include "compat.h"

void vav4l2_compat_identity(const struct v4l2_capability *cap, struct vav4l2_compat_identity *identity)
{
    memset(identity, 0, sizeof(*identity));
    memcpy(identity->driver, cap->driver, sizeof(cap->driver));
    memcpy(identity->card, cap->card, sizeof(cap->card));
    memcpy(identity->bus_info, cap->bus_info, sizeof(cap->bus_info));
    identity->version = cap->version;
}

int vav4l2_compat_resolve_modules(const struct vav4l2_compat_identity *identity,
    const struct vav4l2_compat_module *const *modules, size_t count, struct vav4l2_compat *result)
{
    if (!result) { errno = EINVAL; return -1; }
    struct vav4l2_compat_identity input = identity ? *identity : (struct vav4l2_compat_identity){0};
    int has_identity = identity != NULL;
    identity = &input;
    memset(result, 0, sizeof(*result));
    if (!has_identity || (count && !modules) || !memchr(identity->driver, 0, sizeof(identity->driver)) ||
        !memchr(identity->card, 0, sizeof(identity->card)) || !memchr(identity->bus_info, 0, sizeof(identity->bus_info))) { errno = EINVAL; return -1; }
    struct vav4l2_compat resolved = {.identity = *identity};
    for (size_t i = 0; i < count; i++) {
        const struct vav4l2_compat_module *module = modules[i];
        if (!module || !module->name || !*module->name || !module->match) { errno = EINVAL; return -1; }
        for (size_t j = 0; j < i; j++) if (!strcmp(module->name, modules[j]->name)) { errno = EEXIST; return -1; }
        int matched = module->match(identity);
        if (matched < 0) { errno = EINVAL; return -1; }
        if (!matched) continue;
        if (module->api_version != VAV4L2_COMPAT_API_VERSION || (!!module->allocate != !!module->storage_score) ||
            (!!module->codec_setup != !!module->codec_finish) ||
            (module->behaviors & ~(VAV4L2_COMPAT_SWAP_MIRROR_AXES | VAV4L2_COMPAT_STREAM_BEFORE_QUEUE |
             VAV4L2_COMPAT_EXACT_IMPORT_SPAN | VAV4L2_COMPAT_KEEP_DMA_STREAMS))) { errno = EINVAL; return -1; }
        if (resolved.count == VAV4L2_COMPAT_MAX_MATCHES) { errno = EOVERFLOW; return -1; }
        resolved.modules[resolved.count++] = module;
        resolved.behaviors |= module->behaviors;
    }
    *result = resolved;
    return 0;
}

int vav4l2_compat_resolve(const struct vav4l2_compat_identity *identity, struct vav4l2_compat *result)
{
    size_t count;
    const struct vav4l2_compat_module *const *modules = vav4l2_compat_modules(&count);
    return vav4l2_compat_resolve_modules(identity, modules, count, result);
}

int vav4l2_compat_vpp_score(const struct vav4l2_compat *compat, const struct vav4l2_compat_job *job)
{
    int score = 10;
    if (!compat || !job || compat->count > VAV4L2_COMPAT_MAX_MATCHES) return -1;
    for (unsigned i = 0; i < compat->count; i++) {
        const struct vav4l2_compat_module *module = compat->modules[i];
        int adjustment = module->vpp_score ? module->vpp_score(&compat->identity, job) : 0;
        if (adjustment < 0 || adjustment > INT_MAX - score) return -1;
        score += adjustment;
    }
    return score;
}

int vav4l2_compat_codec_score(const struct vav4l2_compat_identity *identity, const struct vav4l2_compat_codec *codec)
{
    struct vav4l2_compat policy;
    if (!codec || !codec->coded_format || (codec->direction != VAV4L2_COMPAT_DECODE && codec->direction != VAV4L2_COMPAT_ENCODE)) { errno = EINVAL; return -1; }
    if (vav4l2_compat_resolve(identity, &policy) < 0) return -1;
    int score = 10;
    for (unsigned i = 0; i < policy.count; i++) {
        const struct vav4l2_compat_module *module = policy.modules[i];
        int value = module->codec_score ? module->codec_score(identity, codec) : 0;
        if (value < 0 || value > INT_MAX - score) return -1;
        score += value;
    }
    return score;
}

int vav4l2_compat_backend_needed(void)
{
    size_t count;
    const struct vav4l2_compat_module *const *modules = vav4l2_compat_modules(&count);
    for (size_t i = 0; i < count; i++) if (modules[i] && (modules[i]->codec_score || modules[i]->codec_setup ||
        modules[i]->codec_finish || modules[i]->codec_format || modules[i]->codec_controls)) return 1;
    return 0;
}

void vav4l2_compat_backend_finish(struct vav4l2_compat_backend *backend, int fd)
{
    if (!backend) return;
    int saved = errno;
    while (backend->opened) {
        unsigned i = --backend->opened;
        const struct vav4l2_compat_module *module = backend->policy.modules[i];
        if (module->codec_finish) module->codec_finish(&backend->policy.identity, &backend->codec, fd, backend->state[i]);
    }
    memset(backend, 0, sizeof(*backend)); errno = saved;
}

int vav4l2_compat_backend_init(struct vav4l2_compat_backend *backend, const struct vav4l2_compat_identity *identity,
    const struct vav4l2_compat_codec *codec, int fd)
{
    if (!backend || !codec || fd < 0 || backend->initialized || backend->opened) { errno = EINVAL; return -1; }
    struct vav4l2_compat_backend next = {.codec = *codec};
    if (vav4l2_compat_resolve(identity, &next.policy) < 0) return -1;
    if (vav4l2_compat_codec_score(identity, codec) < 0) { errno = ENOTSUP; return -1; }
    for (unsigned i = 0; i < next.policy.count; i++) {
        const struct vav4l2_compat_module *module = next.policy.modules[i];
        next.opened = i + 1;
        if (module->codec_setup && module->codec_setup(identity, codec, fd, &next.state[i]) < 0) {
            int saved = errno ? errno : EIO; vav4l2_compat_backend_finish(&next, fd); errno = saved; return -1;
        }
    }
    next.initialized = 1; *backend = next;
    return 0;
}

int vav4l2_compat_prepare_format(struct vav4l2_compat_backend *backend, struct v4l2_format *format)
{
    if (!backend || !format) { errno = EINVAL; return -1; }
    if (!backend->initialized) return 0;
    struct v4l2_format original = *format;
    for (unsigned i = 0; i < backend->policy.count; i++) {
        const struct vav4l2_compat_module *module = backend->policy.modules[i];
        if (!module->codec_format) continue;
        int result = module->codec_format(&backend->policy.identity, &backend->codec, backend->state[i], format);
        if (result < 0 || format->type != original.type || format->fmt.pix_mp.pixelformat != original.fmt.pix_mp.pixelformat ||
            format->fmt.pix_mp.width != original.fmt.pix_mp.width || format->fmt.pix_mp.height != original.fmt.pix_mp.height ||
            format->fmt.pix_mp.field != original.fmt.pix_mp.field || format->fmt.pix_mp.num_planes != original.fmt.pix_mp.num_planes) {
            *format = original; if (result >= 0) errno = EINVAL; return -1;
        }
    }
    return 0;
}

int vav4l2_compat_prepare_controls(struct vav4l2_compat_backend *backend, struct v4l2_ext_controls *controls)
{
    if (!backend || !controls || (controls->count && !controls->controls)) { errno = EINVAL; return -1; }
    if (!backend->initialized) return 0;
    struct v4l2_ext_controls original = *controls;
    struct v4l2_ext_control *saved = NULL;
    for (unsigned i = 0; i < backend->policy.count; i++) {
        const struct vav4l2_compat_module *module = backend->policy.modules[i];
        if (!module->codec_controls) continue;
        if (original.count && !saved) {
            saved = calloc(original.count, sizeof(*saved));
            if (!saved) return -1;
            memcpy(saved, original.controls, original.count * sizeof(*saved));
        }
        int result = module->codec_controls(&backend->policy.identity, &backend->codec, backend->state[i], controls);
        int invalid = controls->count != original.count || controls->controls != original.controls ||
            controls->which != original.which || controls->request_fd != original.request_fd;
        for (unsigned j = 0; !invalid && j < original.count; j++) invalid = controls->controls[j].id != saved[j].id ||
            controls->controls[j].size != saved[j].size || (saved[j].size && controls->controls[j].ptr != saved[j].ptr);
        if (result < 0 || invalid) {
            *controls = original;
            if (saved) memcpy(controls->controls, saved, original.count * sizeof(*saved));
            free(saved); if (result >= 0) errno = EINVAL; return -1;
        }
    }
    free(saved); return 0;
}
