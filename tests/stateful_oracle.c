/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Test-only VA transport shim: emit reconstructed packets, never decoded pixels. */
#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <va/va_backend.h>
#include "va_v4l2.h"
int drmGetNodeTypeFromFd(int fd) { (void)fd; return 2; }
static VAStatus create(VADriverContextP d, VAConfigID id, int w, int h, int flags, VASurfaceID *targets, int n, VAContextID *out)
{
    (void)flags; (void)targets; (void)n;
    struct vav4l2_driver_data *dd = d->pDriverData; struct vav4l2_config *config = dd->configs;
    while (config && config->config_id != id) config = config->next;
    if (!config) return VA_STATUS_ERROR_INVALID_CONFIG;
    struct vav4l2_context *c = calloc(1, sizeof(*c)); if (!c) return VA_STATUS_ERROR_ALLOCATION_FAILED;
    c->v4l2_fd = c->media_fd = c->request_fd = -1; c->codec = config->codec; c->profile = config->profile;
    c->entrypoint = config->entrypoint; c->rt_format = config->rt_format; c->width = w; c->height = h; c->driver_data = dd;
    *out = c->context_id = ++dd->next_context_id; c->next = dd->contexts; dd->contexts = c; return VA_STATUS_SUCCESS;
}
static VAStatus end(VADriverContextP d, VAContextID id)
{
    struct vav4l2_driver_data *dd = d->pDriverData; struct vav4l2_context *c = dd->contexts;
    while (c && c->context_id != id) c = c->next;
    if (!c || !c->current_surface) return VA_STATUS_ERROR_INVALID_CONTEXT;
    uint8_t *data = malloc(8 * 1024 * 1024); size_t bytes = 0;
    VAStatus status = vav4l2_stateful_packet(c, c->pending_buffers, c->num_pending_buffers, data, 8 * 1024 * 1024, &bytes);
    if (!status) {
        const char *path = getenv("VAV4L2_PACKET_ORACLE_OUT"); FILE *file = path ? fopen(path, "ab") : NULL;
        if (!file) status = VA_STATUS_ERROR_OPERATION_FAILED;
        else { if (fwrite(data, 1, bytes, file) != bytes) status = VA_STATUS_ERROR_OPERATION_FAILED; if (fclose(file)) status = VA_STATUS_ERROR_OPERATION_FAILED; }
    }
    fprintf(stderr,"packet-oracle: codec=%u status=%u bytes=%zu\n",c->codec,status,bytes);
    free(data); c->current_surface->status = status ? VASurfaceSkipped : VASurfaceReady; c->current_surface = NULL; c->num_pending_buffers = 0; return status;
}
VAStatus vaInitialize(VADisplay display, int *major, int *minor)
{
    VAStatus (*real)(VADisplay,int *,int *) = dlsym(RTLD_NEXT, "vaInitialize");
    VAStatus status = real(display, major, minor); if (status) return status;
    VADriverContextP d = ((VADisplayContextP)display)->pDriverContext;
    if (!d->str_vendor || strncmp(d->str_vendor, "libva-v4l2 ", 11)) return VA_STATUS_ERROR_OPERATION_FAILED;
    struct vav4l2_driver_data *dd = d->pDriverData;
    strcpy(dd->dev_h264, "/dev/null"); strcpy(dd->dev_hevc, "/dev/null");
    d->vtable->vaCreateContext = create; d->vtable->vaEndPicture = end; return status;
}
