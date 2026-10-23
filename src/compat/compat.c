/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <errno.h>
#include <limits.h>
#include <string.h>
#include "compat.h"

void vareq_compat_identity(const struct v4l2_capability *cap, struct vareq_compat_identity *identity)
{
    memset(identity, 0, sizeof(*identity));
    memcpy(identity->driver, cap->driver, sizeof(cap->driver));
    memcpy(identity->card, cap->card, sizeof(cap->card));
    memcpy(identity->bus_info, cap->bus_info, sizeof(cap->bus_info));
    identity->version = cap->version;
}

int vareq_compat_resolve_modules(const struct vareq_compat_identity *identity,
    const struct vareq_compat_module *const *modules, size_t count, struct vareq_compat *result)
{
    if (!result) { errno = EINVAL; return -1; }
    struct vareq_compat_identity input = identity ? *identity : (struct vareq_compat_identity){0};
    int has_identity = identity != NULL;
    identity = &input;
    memset(result, 0, sizeof(*result));
    if (!has_identity || (count && !modules) || !memchr(identity->driver, 0, sizeof(identity->driver)) ||
        !memchr(identity->card, 0, sizeof(identity->card)) || !memchr(identity->bus_info, 0, sizeof(identity->bus_info))) { errno = EINVAL; return -1; }
    struct vareq_compat resolved = {.identity = *identity};
    for (size_t i = 0; i < count; i++) {
        const struct vareq_compat_module *module = modules[i];
        if (!module || !module->name || !*module->name || !module->match) { errno = EINVAL; return -1; }
        for (size_t j = 0; j < i; j++) if (!strcmp(module->name, modules[j]->name)) { errno = EEXIST; return -1; }
        int matched = module->match(identity);
        if (matched < 0) { errno = EINVAL; return -1; }
        if (!matched) continue;
        if (module->api_version != VAREQ_COMPAT_API_VERSION || (!!module->allocate != !!module->storage_score) ||
            (module->behaviors & ~(VAREQ_COMPAT_SWAP_MIRROR_AXES | VAREQ_COMPAT_STREAM_BEFORE_QUEUE |
             VAREQ_COMPAT_EXACT_IMPORT_SPAN | VAREQ_COMPAT_KEEP_DMA_STREAMS))) { errno = EINVAL; return -1; }
        if (resolved.count == VAREQ_COMPAT_MAX_MATCHES) { errno = EOVERFLOW; return -1; }
        resolved.modules[resolved.count++] = module;
        resolved.behaviors |= module->behaviors;
    }
    *result = resolved;
    return 0;
}

int vareq_compat_resolve(const struct vareq_compat_identity *identity, struct vareq_compat *result)
{
    size_t count;
    const struct vareq_compat_module *const *modules = vareq_compat_modules(&count);
    return vareq_compat_resolve_modules(identity, modules, count, result);
}

int vareq_compat_vpp_score(const struct vareq_compat *compat, const struct vareq_compat_job *job)
{
    int score = 10;
    if (!compat || !job || compat->count > VAREQ_COMPAT_MAX_MATCHES) return -1;
    for (unsigned i = 0; i < compat->count; i++) {
        const struct vareq_compat_module *module = compat->modules[i];
        int adjustment = module->vpp_score ? module->vpp_score(&compat->identity, job) : 0;
        if (adjustment < 0 || adjustment > INT_MAX - score) return -1;
        score += adjustment;
    }
    return score;
}
