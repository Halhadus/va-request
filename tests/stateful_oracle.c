/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Test-only VA transport shim: emit reconstructed packets, never decoded pixels. */
#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <va/va_backend.h>
#include "va_v4l2.h"
#include "codecs/vp9_request.h"
#include "codecs/request_slices.h"
int drmGetNodeTypeFromFd(int fd) { (void)fd; return 2; }
/* The shim supplies reference availability for parser checks; it has no
 * kernel CAPTURE allocations. Queue/storage ownership is tested separately. */
VAStatus __wrap_vav4l2_validate_reference(const struct vav4l2_context *ctx, VASurfaceID id)
{
    const struct vav4l2_surface *s = vav4l2_surface_by_id(ctx->driver_data, id);
    return s && s->status == VASurfaceReady ? VA_STATUS_SUCCESS : VA_STATUS_ERROR_INVALID_SURFACE;
}
static int parser_query(int fd, unsigned long request, void *arg)
{
    (void)fd;
    if (request != VIDIOC_QUERY_EXT_CTRL) return -1;
    struct v4l2_query_ext_ctrl *q = arg;
    if (q->id != V4L2_CID_STATELESS_HEVC_ENTRY_POINT_OFFSETS) return -1;
    q->elem_size = sizeof(uint32_t); q->elems = 65536; return 0;
}
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
    struct vav4l2_packet_state prepared = {0};
    struct vav4l2_packet_layout layout;
    VAStatus status = vav4l2_stateful_packet_prepare(c, c->pending_buffers, c->num_pending_buffers, data, 8 * 1024 * 1024, &bytes, &prepared, &layout);
    struct vav4l2_vp9_state vp9_pending = c->vp9_state ? *c->vp9_state : (struct vav4l2_vp9_state){0};
    if (!status && c->codec == VAV4L2_CODEC_VP9) {
        struct vav4l2_collected cb; vav4l2_collect_decode_buffers(c->pending_buffers, c->num_pending_buffers, &cb);
        size_t raw_size = 0; uint8_t *raw = malloc(8 * 1024 * 1024);
        struct v4l2_ctrl_vp9_frame frame; struct v4l2_ctrl_vp9_compressed_hdr compressed;
        if (!raw) status = VA_STATUS_ERROR_ALLOCATION_FAILED;
        else {
            status = vav4l2_packetize_slices(VAV4L2_CODEC_VP9, c->pending_buffers, c->num_pending_buffers, raw, 8 * 1024 * 1024, &raw_size);
            if (!status) status = vav4l2_vp9_request_parse(c, cb.pic, raw, raw_size, &vp9_pending, &frame, &compressed);
            free(raw);
        }
        if (!status && !c->vp9_state) c->vp9_state = calloc(1, sizeof(*c->vp9_state));
        if (!status && !c->vp9_state) status = VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    if (!status && (c->codec == VAV4L2_CODEC_H264 || c->codec == VAV4L2_CODEC_HEVC)) {
        struct vav4l2_collected cb; vav4l2_collect_decode_buffers(c->pending_buffers, c->num_pending_buffers, &cb);
        uint8_t *raw = malloc(8 * 1024 * 1024); size_t raw_size = 0;
        struct v4l2_ctrl_h264_decode_params h264;
        struct v4l2_ctrl_hevc_decode_params hevc;
        if (!raw) status = VA_STATUS_ERROR_ALLOCATION_FAILED;
        else {
            status = vav4l2_packetize_slices(c->codec, c->pending_buffers, c->num_pending_buffers, raw, 8 * 1024 * 1024, &raw_size);
            if (!status) {
                if (c->codec == VAV4L2_CODEC_H264) h264_fill_decode_params(&h264, cb.pic, c->driver_data, cb.slice_params[0]);
                else vav4l2_hevc_fill_decode_params(&hevc, cb.pic, c->driver_data);
                struct vav4l2_slice_plan *plan = NULL;
                vav4l2_set_ioctl_hook(parser_query);
                status = vav4l2_request_slice_plan(c, c->pending_buffers, c->num_pending_buffers, raw, raw_size, cb.pic,
                    c->codec == VAV4L2_CODEC_H264 ? (void *)&h264 : (void *)&hevc, &plan);
                vav4l2_request_slice_free(plan); vav4l2_set_ioctl_hook(NULL);
            }
            free(raw);
        }
    }
    if (!status) {
        const char *path = getenv("VAV4L2_PACKET_ORACLE_OUT"); FILE *file = path ? fopen(path, "ab") : NULL;
        if (!file) status = VA_STATUS_ERROR_OPERATION_FAILED;
        else {
            if (c->codec == VAV4L2_CODEC_VP9) {
                if (fseek(file, 0, SEEK_END)) status = VA_STATUS_ERROR_OPERATION_FAILED;
                if (!ftell(file)) {
                    uint8_t header[32] = {'D','K','I','F',0,0,32,0,'V','P','9','0'};
                    header[12] = c->width; header[13] = c->width >> 8; header[14] = c->height; header[15] = c->height >> 8;
                    header[16] = 8; header[20] = 1; /* Coded frame count is not known yet. */
                    if (fwrite(header, 1, sizeof(header), file) != sizeof(header)) status = VA_STATUS_ERROR_OPERATION_FAILED;
                }
                size_t offset = 0;
                for (unsigned j = 0; j < 2 && layout.frame_size[j]; j++) {
                    size_t length = layout.frame_size[j];
                    uint8_t frame[12] = {length, length >> 8, length >> 16, length >> 24};
                    for (unsigned i = 0; i < 8; i++) frame[4 + i] = c->frame_count >> (8 * i);
                    if (fwrite(frame, 1, sizeof(frame), file) != sizeof(frame) || fwrite(data + offset, 1, length, file) != length)
                        status = VA_STATUS_ERROR_OPERATION_FAILED;
                    offset += length;
                }
                if (offset != bytes) status = VA_STATUS_ERROR_OPERATION_FAILED;
            }
            else if (fwrite(data, 1, bytes, file) != bytes) status = VA_STATUS_ERROR_OPERATION_FAILED;
            if (fclose(file)) status = VA_STATUS_ERROR_OPERATION_FAILED;
        }
    }
    if (!status) { vav4l2_packet_state_commit(c, &prepared); if (c->vp9_state) *c->vp9_state = vp9_pending; }
    else if (prepared.av1 != c->packet_state.av1) vav4l2_packet_state_release(&prepared);
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
    strcpy(dd->dev_h264, "/dev/null"); strcpy(dd->dev_hevc, "/dev/null"); strcpy(dd->dev_vp9, "/dev/null"); strcpy(dd->dev_av1, "/dev/null");
    dd->vp9_profiles_known = 1; dd->vp9_profiles = (1u << V4L2_MPEG_VIDEO_VP9_PROFILE_0) | (1u << V4L2_MPEG_VIDEO_VP9_PROFILE_2);
    d->vtable->vaCreateContext = create; d->vtable->vaEndPicture = end; return status;
}
