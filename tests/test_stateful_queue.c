/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include "stateful_fixture.h"
#include "probe.h"
#include <va/va_backend_vpp.h>
#include <drm_fourcc.h>
#include <gbm.h>
extern VAStatus VAV4L2_DRIVER_INIT(VADriverContextP);
static struct { struct timeval time; unsigned index, flags, bytes; } done[16];
static unsigned tail, head, output[4], queued[40], stops, starts, captures, source, submitted;
static uint64_t timestamps[8];
static int no_complete, bad_format, stop_error, short_buffer, extra_event, hold_output, probe_only;
static uint32_t active_coded, capture_format = V4L2_PIX_FMT_NV12;
static unsigned output_stops;
static unsigned display_mode, delay_queries, delay_sets, output_started, output_allocations, complete_on_poll;
static int qbuf_error, output_error, empty_event_error;
static unsigned discovery_calls;
static int vp9, av1, one_output;
static unsigned payload_sizes[8], payload_first[8], payload_indices[8];
static struct vav4l2_context *active_context;
struct export_bo { int fd; uint32_t pitch; size_t size; void *pixels; };
int __wrap_vav4l2_alloc_shared_storage(unsigned bytes, unsigned rows) { (void)bytes; (void)rows; errno = ENOTSUP; return -1; }
int __wrap_vav4l2_alloc_coherent_storage(unsigned bytes, unsigned rows) { return __wrap_vav4l2_alloc_shared_storage(bytes, rows); }
int __wrap_vav4l2_alloc_drm_storage(int fd, unsigned bytes, unsigned rows, int coherent)
{ (void)fd; (void)coherent; return __wrap_vav4l2_alloc_shared_storage(bytes, rows); }
struct gbm_bo *__wrap_gbm_bo_create(struct gbm_device *d, uint32_t w, uint32_t h, uint32_t format, uint32_t flags)
{
    (void)d; (void)flags; assert(format == GBM_FORMAT_R8);
    struct export_bo *bo = calloc(1, sizeof(*bo)); assert(bo);
    bo->fd = memfd_create("stateful-client-backing", MFD_CLOEXEC); bo->pitch = w; bo->size = (size_t)w * h;
    assert(bo->fd >= 0 && !ftruncate(bo->fd, bo->size));
    bo->pixels = mmap(NULL, bo->size, PROT_READ | PROT_WRITE, MAP_SHARED, bo->fd, 0); assert(bo->pixels != MAP_FAILED);
    return (struct gbm_bo *)bo;
}
void *__wrap_gbm_bo_map(struct gbm_bo *b, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t flags, uint32_t *pitch, void **cookie)
{ (void)x; (void)y; (void)w; (void)h; (void)flags; struct export_bo *bo = (void *)b; *pitch = bo->pitch; *cookie = bo; return bo->pixels; }
void __wrap_gbm_bo_unmap(struct gbm_bo *b, void *cookie) { (void)b; (void)cookie; }
uint32_t __wrap_gbm_bo_get_stride_for_plane(struct gbm_bo *b, int p) { (void)p; return ((struct export_bo *)b)->pitch; }
uint64_t __wrap_gbm_bo_get_modifier(struct gbm_bo *b) { (void)b; return DRM_FORMAT_MOD_LINEAR; }
int __wrap_gbm_bo_get_fd_for_plane(struct gbm_bo *b, int p) { (void)p; return fcntl(((struct export_bo *)b)->fd, F_DUPFD_CLOEXEC, 0); }
void __wrap_gbm_bo_destroy(struct gbm_bo *b) { struct export_bo *bo = (void *)b; munmap(bo->pixels, bo->size); close(bo->fd); free(bo); }
void __wrap_gbm_device_destroy(struct gbm_device *d) { (void)d; }
/* Driver initialization must not probe the host through the queue mock. */
int __wrap_vav4l2_scan_all_cached(char *h264, char *hevc, char *av1, char *vp8, char *mpeg2, char *jpeg, char *vpp, char *vp9, unsigned len)
{
    char *paths[] = {h264, hevc, av1, vp8, mpeg2, jpeg, vpp, vp9};
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) if (paths[i] && len) paths[i][0] = 0;
    discovery_calls++; return 0;
}
static void result(unsigned index, uint64_t timestamp, unsigned flags, unsigned bytes)
{
    assert(tail < 16); done[tail].index = index; done[tail].time.tv_sec = timestamp / 1000000000;
    done[tail].time.tv_usec = timestamp / 1000 % 1000000; done[tail].flags = flags; done[tail++].bytes = bytes;
}
static int mock(int fd, unsigned long request, void *arg)
{
    (void)fd;
    if (request == VIDIOC_QUERYCAP) { ((struct v4l2_capability *)arg)->capabilities = V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING; return 0; }
    if (request == VIDIOC_SUBSCRIBE_EVENT || request == VIDIOC_UNSUBSCRIBE_EVENT || request == VIDIOC_TRY_DECODER_CMD) return 0;
    if (request == VIDIOC_QUERYCTRL) {
        struct v4l2_queryctrl *q = arg;
        if (q->id == V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY || q->id == V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY_ENABLE) {
            assert(!output_started && !output_allocations); delay_queries++;
            int enable = q->id == V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY_ENABLE;
            if (!display_mode || (display_mode == 3 && enable)) { errno = enable ? ENOTTY : EINVAL; return -1; }
            if (display_mode == 8) { errno = EIO; return -1; }
            q->type = enable ? V4L2_CTRL_TYPE_BOOLEAN : V4L2_CTRL_TYPE_INTEGER; q->minimum = 0; q->maximum = 1; q->step = 1;
            if (display_mode == 2) q->flags = V4L2_CTRL_FLAG_READ_ONLY;
            if (display_mode == 4 || (display_mode == 10 && enable)) q->minimum = 1;
            if (display_mode == 5 && enable) q->flags = V4L2_CTRL_FLAG_INACTIVE;
            if (display_mode == 6 && !enable) { q->minimum = -1; q->step = 2; }
            if (display_mode == 7 && enable) q->type = V4L2_CTRL_TYPE_INTEGER;
            if (display_mode == 11) q->step = 0;
            if (display_mode == 12) q->flags = V4L2_CTRL_FLAG_DISABLED;
            if (display_mode == 13) q->flags = V4L2_CTRL_FLAG_GRABBED;
            return 0;
        }
        assert(q->id == (active_coded == V4L2_PIX_FMT_HEVC ? V4L2_CID_MPEG_VIDEO_HEVC_PROFILE :
            active_coded == V4L2_PIX_FMT_AV1 ? V4L2_CID_MPEG_VIDEO_AV1_PROFILE :
            active_coded == V4L2_PIX_FMT_VP9 ? V4L2_CID_MPEG_VIDEO_VP9_PROFILE : V4L2_CID_MPEG_VIDEO_H264_PROFILE));
        q->type = V4L2_CTRL_TYPE_MENU; q->maximum = 17; return 0;
    }
    if (request == VIDIOC_QUERYMENU) return 0;
    if (request == VIDIOC_S_EXT_CTRLS) {
        struct v4l2_ext_controls *set = arg; delay_sets++;
        assert(!output_started && !output_allocations && !active_context->output_bufs_allocd && set->which == V4L2_CTRL_WHICH_CUR_VAL && set->count == 2);
        assert(set->controls[0].id == V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY && !set->controls[0].value);
        assert(set->controls[1].id == V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY_ENABLE && set->controls[1].value == 1);
        if (display_mode == 9) { errno = EIO; return -1; }
        return 0;
    }
    if (request == VIDIOC_S_FMT || request == VIDIOC_G_FMT) {
        struct v4l2_format *f = arg;
        if (f->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            f->fmt.pix_mp.width = active_context->width; f->fmt.pix_mp.height = active_context->height; f->fmt.pix_mp.num_planes = 1;
            f->fmt.pix_mp.pixelformat = bad_format ? V4L2_PIX_FMT_YUV420 : capture_format;
            f->fmt.pix_mp.field = V4L2_FIELD_NONE; f->fmt.pix_mp.plane_fmt[0].bytesperline = active_context->width * (capture_format == V4L2_PIX_FMT_P010 ? 2 : 1);
            f->fmt.pix_mp.plane_fmt[0].sizeimage = f->fmt.pix_mp.plane_fmt[0].bytesperline * active_context->height * 3 / 2;
            unsigned planes = vav4l2_capture_layout_planes(capture_format);
            if (planes) {
                if (capture_format == V4L2_PIX_FMT_NV12M_8L128) f->fmt.pix_mp.height = 128;
                f->fmt.pix_mp.num_planes = planes;
                for (unsigned p = 0; p < planes; p++) {
                    unsigned pitch = active_context->width / (p && planes == 3 ? 2 : 1);
                    unsigned rows = p ? f->fmt.pix_mp.height / 2 : f->fmt.pix_mp.height;
                    if (capture_format == V4L2_PIX_FMT_NV12M_8L128 && p) rows = 128;
                    if (capture_format == V4L2_PIX_FMT_NV12MT_16X16 && p) rows = (rows + 15) & ~15u;
                    f->fmt.pix_mp.plane_fmt[p].bytesperline = pitch; f->fmt.pix_mp.plane_fmt[p].sizeimage = pitch * rows;
                }
            }
        } else {
            if (request == VIDIOC_S_FMT) active_coded = f->fmt.pix_mp.pixelformat;
            else f->fmt.pix_mp.pixelformat = active_coded;
            f->fmt.pix_mp.num_planes = 1;
        }
        return 0;
    }
    if (request == VIDIOC_ENUM_FMT) {
        struct v4l2_fmtdesc *f = arg; if (f->index && !(probe_only && f->index == 1)) { errno = EINVAL; return -1; }
        f->pixelformat = f->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE ? capture_format : f->index ? V4L2_PIX_FMT_HEVC : V4L2_PIX_FMT_H264; return 0;
    }
    if (request == VIDIOC_G_SELECTION) { struct v4l2_selection *s = arg; s->r.width = active_context->width; s->r.height = active_context->height; return 0; }
    if (request == VIDIOC_G_CTRL) { ((struct v4l2_control *)arg)->value = 4; return 0; }
    if (request == VIDIOC_REQBUFS) {
        struct v4l2_requestbuffers *b = arg;
        if (b->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE && b->count) output_allocations++;
        if (b->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE && b->count && one_output) b->count = 1;
        if (b->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE && b->count) { assert(b->count >= 8); captures++; }
        return 0;
    }
    if (request == VIDIOC_EXPBUF) {
        struct v4l2_exportbuffer *e = arg;
        e->fd = memfd_create("capture-export", MFD_CLOEXEC); assert(e->fd >= 0 && !ftruncate(e->fd, 1024)); return 0;
    }
    if (request == VIDIOC_QUERYBUF) {
        struct v4l2_buffer *b = arg; b->m.planes[0].length = b->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE ?
            active_context->width * active_context->height * (capture_format == V4L2_PIX_FMT_P010 ? 3 : 2) : 65536;
        b->m.planes[0].m.mem_offset = b->index * 65536; return 0;
    }
    if (request == VIDIOC_STREAMON || request == VIDIOC_STREAMOFF) {
        if (request == VIDIOC_STREAMON && *(unsigned *)arg == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) output_started++;
        if (request == VIDIOC_STREAMOFF) {
            if (*(unsigned *)arg == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) memset(queued, 0, sizeof(queued));
            else output_stops++;
        }
        return 0;
    }
    if (request == VIDIOC_QBUF) {
        struct v4l2_buffer *b = arg;
        if (b->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) { assert(b->length == (vav4l2_capture_layout_planes(capture_format) ? vav4l2_capture_layout_planes(capture_format) : 1)); assert(!queued[b->index]); queued[b->index] = 1; }
        else {
            if (qbuf_error && submitted + 1 == (unsigned)qbuf_error) { errno = EIO; return -1; }
            assert(!(b->flags & V4L2_BUF_FLAG_REQUEST_FD) && b->m.planes[0].bytesused); output[b->index] = 1;
            assert(submitted < 8);
            payload_sizes[submitted] = b->m.planes[0].bytesused; payload_indices[submitted] = b->index;
            payload_first[submitted] = ((const uint8_t *)active_context->output_buf_ptr[b->index])[0];
            timestamps[submitted++] = ((uint64_t)b->timestamp.tv_sec * 1000000 + b->timestamp.tv_usec) * 1000;
            if (!captures) source = 1;
        }
        return 0;
    }
    if (request == VIDIOC_DQEVENT) {
        if (source || extra_event) {
            struct v4l2_event *e = arg; e->type = extra_event ? V4L2_EVENT_EOS : V4L2_EVENT_SOURCE_CHANGE;
            e->u.src_change.changes = V4L2_EVENT_SRC_CH_RESOLUTION; source = extra_event = 0; return 0;
        }
        errno = empty_event_error; return -1;
    }
    if (request == VIDIOC_DQBUF) {
        struct v4l2_buffer *b = arg;
        if (b->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
            if (!hold_output) for (unsigned i = 0; i < 4; i++) if (output[i]) { b->index = i; b->flags = output_error ? V4L2_BUF_FLAG_ERROR : 0; output[i] = 0; return 0; }
        } else if (head < tail) {
            b->index = done[head].index; b->timestamp = done[head].time; b->flags = done[head].flags;
            b->m.planes[0].bytesused = short_buffer ? 1 : done[head].bytes;
            assert(queued[b->index]); queued[b->index] = 0; head++; return 0;
        }
        errno = EAGAIN; return -1;
    }
    if (request == VIDIOC_DECODER_CMD) {
        struct v4l2_decoder_cmd *c = arg;
        if (c->cmd == V4L2_DEC_CMD_STOP) {
            if (stop_error) { errno = EIO; return -1; }
            stops++;
            if (!no_complete) { for (unsigned i = 0; i < submitted; i++) result(i, timestamps[i], 0, 384); result(submitted, 0, V4L2_BUF_FLAG_LAST, 0); }
        } else { assert(c->cmd == V4L2_DEC_CMD_START); starts++; }
        return 0;
    }
    errno = EINVAL; return -1;
}
int __wrap_ioctl(int fd, unsigned long request, ...) { va_list ap; va_start(ap, request); void *p = va_arg(ap, void *); va_end(ap); return mock(fd, request, p); }
int __wrap_poll(struct pollfd *fds, nfds_t count, int timeout) { (void)timeout; if (complete_on_poll) { result(0, timestamps[0], 0, 384); complete_on_poll = 0; } for (nfds_t i = 0; i < count; i++) fds[i].revents = fds[i].events; return 1; }
#ifdef __GLIBC__
/* Fortified glibc can redirect a bounded poll array to this entry point. */
int __wrap___poll_chk(struct pollfd *fds, nfds_t count, int timeout, size_t size)
{ assert(count <= size / sizeof(*fds)); return __wrap_poll(fds, count, timeout); }
#endif
static void init(struct vav4l2_context *c, struct vav4l2_driver_data *dd, struct vav4l2_surface *surfaces)
{
    tail = head = stops = starts = captures = source = submitted = output_stops = 0;
    delay_queries = delay_sets = output_started = output_allocations = complete_on_poll = 0;
    qbuf_error = output_error = 0; empty_event_error = ENOENT;
    no_complete = bad_format = stop_error = short_buffer = extra_event = hold_output = probe_only = 0;
    memset(output, 0, sizeof(output)); memset(queued, 0, sizeof(queued));
    memset(c, 0, sizeof(*c)); memset(dd, 0, sizeof(*dd)); memset(surfaces, 0, 2 * sizeof(*surfaces));
    dd->gbm.device = (struct gbm_device *)dd; dd->gbm.fd = dd->gbm.drm_fd = -1;
    active_context = c;
    c->codec = capture_format == V4L2_PIX_FMT_P010 ? VAV4L2_CODEC_HEVC : VAV4L2_CODEC_H264;
    c->profile = capture_format == V4L2_PIX_FMT_P010 ? VAProfileHEVCMain10 : VAProfileH264High;
    c->rt_format = capture_format == V4L2_PIX_FMT_P010 ? VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_YUV420_10 : VA_RT_FORMAT_YUV420;
    c->coded_format = capture_format == V4L2_PIX_FMT_P010 ? V4L2_PIX_FMT_HEVC : V4L2_PIX_FMT_H264;
    if (vp9) { c->codec = VAV4L2_CODEC_VP9; c->coded_format = V4L2_PIX_FMT_VP9;
        c->profile = capture_format == V4L2_PIX_FMT_P010 ? VAProfileVP9Profile2 : VAProfileVP9Profile0; }
    c->width = c->height = 16; c->driver_data = dd; c->media_fd = c->request_fd = -1;
    if (av1) { c->codec = VAV4L2_CODEC_AV1; c->coded_format = V4L2_PIX_FMT_AV1; c->profile = VAProfileAV1Profile0; c->width = c->height = 128; }
    c->v4l2_fd = memfd_create("stateful-queue", MFD_CLOEXEC); assert(c->v4l2_fd >= 0 && !ftruncate(c->v4l2_fd, 4 * 65536));
    for (unsigned i = 0; i < 2; i++) { surfaces[i].driver_data = dd; surfaces[i].surface_id = i; surfaces[i].width = c->width; surfaces[i].height = c->height;
        surfaces[i].memfd_fd = surfaces[i].buf_index = -1; surfaces[i].format = capture_format == V4L2_PIX_FMT_P010 ? VA_FOURCC_P010 : VA_FOURCC_NV12; surfaces[i].cpu_stride = vav4l2_default_image_stride(surfaces[i].format, c->width);
        surfaces[i].cpu_size = vav4l2_va_image_size(surfaces[i].format, surfaces[i].cpu_stride, c->height);
        surfaces[i].status = VASurfaceRendering; dd->surfaces[i] = &surfaces[i]; }
    vav4l2_set_ioctl_hook(mock);
    const char *modes[] = {"VAV4L2_CAPTURE_DMABUF", "VAV4L2_CAPTURE_POOL_DMABUF"};
    for (unsigned i = 0; i < 2; i++) {
        setenv(modes[i], "required", 1); assert(vav4l2_stateful_init(c) < 0 && !c->stateful); unsetenv(modes[i]);
    }
    int status = vav4l2_stateful_init(c);
    if (display_mode == 8 || display_mode == 9) { assert(status < 0 && !output_started); return; }
    assert(status == 0 && c->media_fd == -1 && c->request_fd == -1 && !captures);
}
static void submit(struct vav4l2_context *c, struct vav4l2_surface *surface, struct stateful_fixture *f, uint64_t timestamp)
{
    c->current_surface = surface; c->current_surface_id = surface->surface_id; surface->timestamp = timestamp;
    assert(vav4l2_stateful_translate(c, f->batch, 3) == VA_STATUS_SUCCESS); c->current_surface = NULL;
}
static void finish(struct vav4l2_context *c, struct vav4l2_surface *surfaces)
{
    vav4l2_release_context_device(c);
    for (unsigned i = 0; i < 2; i++) { vav4l2_gbm_surface_destroy(&surfaces[i]); if (surfaces[i].memfd_map) munmap(surfaces[i].memfd_map, surfaces[i].memfd_map_size); if (surfaces[i].memfd_fd >= 0) close(surfaces[i].memfd_fd); free(surfaces[i].cpu_ptr); }
    vav4l2_set_ioctl_hook(NULL);
}
int main(void)
{
    setenv("VAV4L2_EXPBUF_EXPORT", "1", 1); setenv("VAV4L2_GPU_COPY", "0", 1);
    struct vav4l2_context c; struct vav4l2_driver_data dd; struct vav4l2_surface surfaces[2]; struct stateful_fixture f; stateful_fixture(&f);
    vav4l2_set_ioctl_hook(mock); probe_only = 1; active_coded = V4L2_PIX_FMT_HEVC;
    uint32_t profiles;
    assert(vav4l2_query_h264_profiles("/dev/null", &profiles) == 0 && (profiles & (1u << V4L2_MPEG_VIDEO_H264_PROFILE_HIGH)));
    assert(active_coded == V4L2_PIX_FMT_H264);
    assert(vav4l2_query_profiles("/dev/null", V4L2_CID_MPEG_VIDEO_HEVC_PROFILE, &profiles) == 0 && profiles == ((1u << V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN) | (1u << V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN_10)));
    assert(active_coded == V4L2_PIX_FMT_HEVC); vav4l2_set_ioctl_hook(NULL);
    const uint32_t planar[] = {V4L2_PIX_FMT_NV12M, V4L2_PIX_FMT_NV21M, V4L2_PIX_FMT_YUV420M,
        V4L2_PIX_FMT_YVU420M, V4L2_PIX_FMT_NV12MT_16X16, V4L2_PIX_FMT_NV12M_8L128};
    for (unsigned i = 0; i < sizeof(planar) / sizeof(planar[0]); i++) {
        capture_format = planar[i]; init(&c, &dd, surfaces); submit(&c, &surfaces[0], &f, 0);
        assert(c.capture_layout && c.cap_pixelformat == V4L2_PIX_FMT_NV12 && captures == 1);
        finish(&c, surfaces);
    }
    capture_format = V4L2_PIX_FMT_NV12;
    for (display_mode = 0; display_mode <= 13; display_mode++) {
        init(&c, &dd, surfaces);
        if (display_mode == 8 || display_mode == 9) { assert(delay_sets == (display_mode == 9)); finish(&c, surfaces); continue; }
        int enabled = display_mode == 1 || display_mode == 10;
        assert(delay_sets == (unsigned)enabled && delay_queries && output_started == 1);
        submit(&c, &surfaces[0], &f, 0);
        assert(vav4l2_stateful_sync(&c, &surfaces[0], 0) == VA_STATUS_ERROR_TIMEDOUT && !stops);
        complete_on_poll = enabled;
        assert(vav4l2_stateful_sync(&c, &surfaces[0], 100000000) == VA_STATUS_SUCCESS && surfaces[0].status == VASurfaceReady);
        assert(stops == (unsigned)!enabled && starts == (unsigned)!enabled);
        finish(&c, surfaces);
    }
    display_mode = 0;
    capture_format = V4L2_PIX_FMT_P010; init(&c, &dd, surfaces);
    assert(vav4l2_stateful_pump(&c) == 0); empty_event_error = EAGAIN;
    assert(vav4l2_stateful_pump(&c) == 0); empty_event_error = ENOENT;
    VAPictureParameterBufferHEVC hp = {.pic_width_in_luma_samples = 16, .pic_height_in_luma_samples = 16};
    hp.log2_diff_max_min_luma_coding_block_size = 1; hp.pic_fields.bits.chroma_format_idc = 1;
    hp.bit_depth_luma_minus8 = hp.bit_depth_chroma_minus8 = 2;
    hp.slice_parsing_fields.bits.IdrPicFlag = hp.slice_parsing_fields.bits.RapPicFlag = 1;
    for (unsigned i = 0; i < 15; i++) hp.ReferenceFrames[i].flags = VA_PICTURE_HEVC_INVALID;
    VASliceParameterBufferHEVC hs = {.slice_data_size = 7}; uint8_t hn[] = {0,0,0,1,0x26,1,0xa0};
    f.buffers[0].data = &hp; f.buffers[0].size = sizeof(hp); f.buffers[1].data = &hs; f.buffers[1].size = sizeof(hs);
    f.buffers[2].data = hn; f.buffers[2].size = sizeof(hn);
    submit(&c, &surfaces[0], &f, 0);
    assert(c.cap_pixelformat == V4L2_PIX_FMT_P010 && c.cap_stride == 32 && c.cap_sizeimage == 768 && !c.num_render_targets);
    result(0, 0, 0, 768); assert(surfaces[0].status == VASurfaceRendering);
    struct VADriverVTable native_table = {0}; struct VADriverContext native_driver = {.vtable = &native_table};
    assert(VAV4L2_DRIVER_INIT(&native_driver) == VA_STATUS_SUCCESS); void *native_original = native_driver.pDriverData;
    native_driver.pDriverData = &dd; dd.contexts = &c;
    VADRMPRIMESurfaceDescriptor descriptor;
    assert(native_table.vaExportSurfaceHandle(&native_driver, 0, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
        VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_COMPOSED_LAYERS, &descriptor) == VA_STATUS_SUCCESS);
    assert(native_table.vaSyncSurface(&native_driver, 0) == VA_STATUS_SUCCESS && surfaces[0].status == VASurfaceReady);
    assert(descriptor.fourcc == VA_FOURCC_P010 && descriptor.layers[0].drm_format == DRM_FORMAT_P010 &&
        descriptor.layers[0].pitch[0] >= 32 && descriptor.layers[0].offset[1] == descriptor.layers[0].pitch[0] * 16);
    int retained = descriptor.objects[0].fd; struct stat first, second; assert(!fstat(retained, &first));
    c.frame_count = 1;
    assert(native_table.vaBeginPicture(&native_driver, c.context_id, 0) == VA_STATUS_SUCCESS);
    assert(vav4l2_stateful_translate(&c, f.batch, 3) == VA_STATUS_SUCCESS); c.current_surface = NULL;
    c.capture_buf_ptr[1] = calloc(1, 1024); assert(c.capture_buf_ptr[1]); memset(c.capture_buf_ptr[1], 0x22, 1024);
    result(1, 1000, 0, 768); assert(surfaces[0].status == VASurfaceRendering);
    assert(native_table.vaExportSurfaceHandle(&native_driver, 0, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
        VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS, &descriptor) == VA_STATUS_SUCCESS);
    assert(native_table.vaSyncSurface(&native_driver, 0) == VA_STATUS_SUCCESS && surfaces[0].status == VASurfaceReady);
    assert(descriptor.num_layers == 2 && descriptor.layers[0].drm_format == DRM_FORMAT_R16 &&
        descriptor.layers[1].drm_format == DRM_FORMAT_GR1616 && descriptor.layers[1].offset[0] == descriptor.layers[0].pitch[0] * 16);
    assert(!fstat(descriptor.objects[0].fd, &second) && first.st_dev == second.st_dev && first.st_ino == second.st_ino && surfaces[0].buf_index == 1);
    uint8_t value; assert(pread(retained, &value, 1, 0) == 1 && value == 0x22);
    close(retained); close(descriptor.objects[0].fd); native_driver.pDriverData = native_original; native_table.vaTerminate(&native_driver);
    finish(&c, surfaces);
    init(&c, &dd, surfaces); qbuf_error = 1;
    c.current_surface = &surfaces[0]; c.current_surface_id = 0;
    assert(vav4l2_stateful_translate(&c, f.batch, 3) == VA_STATUS_ERROR_DECODING_ERROR &&
        !c.packet_state.hevc_started && c.v4l2_fd == -1);
    c.current_surface = NULL; finish(&c, surfaces);
    capture_format = V4L2_PIX_FMT_NV12; stateful_fixture(&f);
    init(&c, &dd, surfaces); submit(&c, &surfaces[0], &f, 0); submit(&c, &surfaces[1], &f, 1000);
    assert(captures == 1 && c.n_free_out == 4 && vav4l2_stateful_pending(&c, 0) && vav4l2_stateful_pending(&c, 1));
    result(1, 1000, 0, 384); result(0, 0, 0, 384); assert(vav4l2_stateful_pump(&c) == 0);
    assert(surfaces[0].status == VASurfaceReady && surfaces[1].status == VASurfaceReady && surfaces[0].buf_index == 0 && surfaces[1].buf_index == 1);
    assert(!stops && !starts && !vav4l2_stateful_pending(&c, 0)); finish(&c, surfaces);
    init(&c, &dd, surfaces); submit(&c, &surfaces[0], &f, 0);
    assert(vav4l2_stateful_sync(&c, &surfaces[0], 0) == VA_STATUS_ERROR_TIMEDOUT && !stops && vav4l2_stateful_pending(&c, 0));
    assert(vav4l2_stateful_sync(&c, &surfaces[0], 100000000) == VA_STATUS_SUCCESS && stops == 1 && starts == 1 && surfaces[0].status == VASurfaceReady); finish(&c, surfaces);
    init(&c, &dd, surfaces); hold_output = 1; submit(&c, &surfaces[0], &f, 0);
    assert(vav4l2_stateful_sync(&c, &surfaces[0], 100000000) == VA_STATUS_SUCCESS && stops == 1 && !starts && c.n_free_out == 3);
    hold_output = 0; assert(vav4l2_stateful_pump(&c) == 0 && starts == 1 && c.n_free_out == 4); finish(&c, surfaces);
    init(&c, &dd, surfaces); submit(&c, &surfaces[0], &f, 0); no_complete = 1;
    assert(vav4l2_stateful_sync(&c, &surfaces[0], 1000000) == VA_STATUS_ERROR_TIMEDOUT && vav4l2_stateful_pending(&c, 0) && c.v4l2_fd >= 0);
    result(0, 0, 0, 384); result(1, 0, V4L2_BUF_FLAG_LAST, 0); assert(vav4l2_stateful_sync(&c, &surfaces[0], 100000000) == VA_STATUS_SUCCESS && starts == 1); finish(&c, surfaces);
    init(&c, &dd, surfaces); submit(&c, &surfaces[0], &f, 0); no_complete = 1;
    assert(vav4l2_stateful_sync(&c, &surfaces[0], 1000000) == VA_STATUS_ERROR_TIMEDOUT && stops == 1);
    source = 1; result(0, 0, 0, 384); result(1, 0, V4L2_BUF_FLAG_LAST, 0);
    assert(!vav4l2_stateful_pump(&c) && captures == 2 && !starts);
    submit(&c, &surfaces[1], &f, 1000); assert(submitted == 2 && c.v4l2_fd >= 0); finish(&c, surfaces);
    init(&c, &dd, surfaces); submit(&c, &surfaces[0], &f, 0);
    result(0, 0, 0, 384); assert(vav4l2_stateful_pump(&c) == 0 && surfaces[0].cap_view);
    source = 1; result(1, 0, V4L2_BUF_FLAG_LAST, 0);
    assert(vav4l2_stateful_pump(&c) == 0 && captures == 2 && !output_stops && surfaces[0].has_pic &&
        !surfaces[0].cap_view && surfaces[0].buf_index == -1 && surfaces[0].memfd_map); finish(&c, surfaces);
    init(&c, &dd, surfaces); submit(&c, &surfaces[0], &f, 0); stop_error = 1;
    assert(vav4l2_stateful_sync(&c, &surfaces[0], 100000000) == VA_STATUS_ERROR_DECODING_ERROR && c.v4l2_fd == -1); finish(&c, surfaces);
    for (unsigned error = 0; error < 7; error++) {
        init(&c, &dd, surfaces); if (error == 5) hold_output = 1;
        submit(&c, &surfaces[0], &f, 0);
        if (error == 0) result(0, 999000, 0, 384);
        if (error == 1) { result(0, 0, 0, 384); short_buffer = 1; }
        if (error == 2) result(0, 0, V4L2_BUF_FLAG_ERROR, 384);
        if (error == 3) result(0, 0, V4L2_BUF_FLAG_LAST, 0);
        if (error == 4) extra_event = 1;
        if (error == 6) empty_event_error = EIO;
        if (error == 5) { output_error = 1; hold_output = 0; result(0, 0, 0, 384); }
        assert(vav4l2_stateful_pump(&c) < 0 && c.v4l2_fd == -1 && surfaces[0].status == VASurfaceSkipped); finish(&c, surfaces);
    }
    /* Hidden and show-existing frames need separate OUTPUT ownership, while
     * completing one VA surface. A one-slot grant must also make progress. */
    vp9 = 1;
    for (unsigned mode = 0; mode < 4; mode++) {
        one_output = mode == 1; capture_format = mode == 2 ? V4L2_PIX_FMT_P010 : V4L2_PIX_FMT_NV12;
        init(&c, &dd, surfaces);
        VADecPictureParameterBufferVP9 p = {.frame_width = 16, .frame_height = 16, .profile = mode == 2 ? 2 : 0,
            .bit_depth = mode == 2 ? 10 : 8, .frame_header_length_in_bytes = 2, .first_partition_size = 1};
        p.pic_fields.bits.subsampling_x = p.pic_fields.bits.subsampling_y = 1;
        VASliceParameterBufferVP9 slice = {.slice_data_size = 4}; uint8_t packet[] = {mode == 2 ? 0x90 : 0x80, 0x49, 0x83, 0x42};
        struct vav4l2_buffer pic = {.type = VAPictureParameterBufferType, .data = &p, .size = sizeof(p)},
            params = {.type = VASliceParameterBufferType, .data = &slice, .size = sizeof(slice)},
            data = {.type = VASliceDataBufferType, .data = packet, .size = sizeof(packet)};
        struct vav4l2_buffer *batch[] = {&pic, &params, &data};
        c.current_surface = &surfaces[0]; c.current_surface_id = 0; surfaces[0].timestamp = 0;
        if (mode == 3) qbuf_error = 2;
        VAStatus rc = vav4l2_stateful_translate(&c, batch, 3); c.current_surface = NULL;
        if (mode == 3) assert(rc == VA_STATUS_ERROR_DECODING_ERROR && submitted == 1 && c.v4l2_fd == -1 &&
            surfaces[0].status == VASurfaceSkipped && !vav4l2_stateful_pending(&c, 0));
        else {
            assert(rc == VA_STATUS_SUCCESS && submitted == 2 && payload_sizes[0] == 4 && payload_sizes[1] == 1 &&
                payload_first[0] == packet[0] && payload_first[1] == (unsigned)(mode == 2 ? 0x98 : 0x88) &&
                timestamps[0] == 0 && timestamps[1] == 0 && vav4l2_stateful_pending(&c, 0));
            assert(one_output ? payload_indices[0] == payload_indices[1] : payload_indices[0] != payload_indices[1]);
            result(0, 0, 0, mode == 2 ? 768 : 384);
            assert(vav4l2_stateful_pump(&c) == 0 && surfaces[0].status == VASurfaceReady && !vav4l2_stateful_pending(&c, 0));
        }
        finish(&c, surfaces);
    }
    vp9 = one_output = 0; capture_format = V4L2_PIX_FMT_NV12; av1 = 1;
    for (unsigned depth = 0; depth < 2; depth++) {
        capture_format = depth ? V4L2_PIX_FMT_P010 : V4L2_PIX_FMT_NV12;
        init(&c, &dd, surfaces);
        VADecPictureParameterBufferAV1 p = {.frame_width_minus1 = 127, .frame_height_minus1 = 127,
            .tile_cols = 1, .tile_rows = 1, .primary_ref_frame = 7, .bit_depth_idx = depth};
        p.seq_info_fields.fields.subsampling_x = p.seq_info_fields.fields.subsampling_y = 1;
        p.pic_info_fields.bits.show_frame = p.pic_info_fields.bits.uniform_tile_spacing_flag = 1;
        for (unsigned i = 0; i < 8; i++) p.ref_frame_map[i] = VA_INVALID_SURFACE;
        uint8_t bytes[] = {1,2,3,4}; VASliceParameterBufferAV1 tile = {.slice_data_size = sizeof(bytes)};
        struct vav4l2_buffer b[] = {{.type = VAPictureParameterBufferType, .size = sizeof(p), .data = &p},
            {.type = VASliceParameterBufferType, .size = sizeof(tile), .data = &tile},
            {.type = VASliceDataBufferType, .size = sizeof(bytes), .data = bytes}};
        struct vav4l2_buffer *batch[] = {b, b + 1, b + 2};
        c.current_surface = &surfaces[0]; c.current_surface_id = 0;
        assert(vav4l2_stateful_translate(&c, batch, 3) == VA_STATUS_SUCCESS && c.packet_state.av1 && submitted == 1);
        c.current_surface = NULL; result(0, 0, 0, 128 * 128 * 3 / 2 * (depth ? 2 : 1));
        assert(!vav4l2_stateful_pump(&c) && surfaces[0].status == VASurfaceReady); finish(&c, surfaces);
    }
    av1 = 0; capture_format = V4L2_PIX_FMT_NV12;
    struct VADriverVTable table = {0}; struct VADriverVTableVPP vpp = {0};
    struct VADriverContext driver = {.vtable = &table, .vtable_vpp = &vpp};
    assert(VAV4L2_DRIVER_INIT(&driver) == VA_STATUS_SUCCESS); void *original = driver.pDriverData;
    init(&c, &dd, surfaces); c.context_id = 1; dd.contexts = &c; driver.pDriverData = &dd;
    submit(&c, &surfaces[0], &f, 0); VASurfaceID id = 0; VASurfaceStatus status;
    assert(table.vaBeginPicture(&driver, 1, 0) == VA_STATUS_ERROR_SURFACE_BUSY);
    assert(table.vaDestroySurfaces(&driver, &id, 1) == VA_STATUS_ERROR_SURFACE_BUSY && dd.surfaces[0] == &surfaces[0]);
    assert(table.vaSyncSurface2(&driver, 0, 0) == VA_STATUS_ERROR_TIMEDOUT && !stops);
    assert(table.vaQuerySurfaceStatus(&driver, 0, &status) == VA_STATUS_SUCCESS && status == VASurfaceRendering);
    result(0, 0, 0, 384);
    assert(table.vaQuerySurfaceStatus(&driver, 0, &status) == VA_STATUS_SUCCESS && status == VASurfaceReady);
    finish(&c, surfaces); driver.pDriverData = original; table.vaTerminate(&driver);
    assert(discovery_calls == 2);
    puts("stateful queue order, drain, deadlines, reconfiguration and VA lifetime: PASS"); return 0;
}
