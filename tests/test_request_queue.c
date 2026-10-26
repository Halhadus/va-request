/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <linux/media.h>
#include "va_v4l2.h"
extern VAStatus VAV4L2_DRIVER_INIT(VADriverContextP);
static struct VADriverVTable table;
int __wrap_vav4l2_scan_all_cached(char *h264, char *hevc, char *av1, char *vp8, char *mpeg2, char *jpeg, char *vpp, char *vp9, unsigned len)
{
    char *paths[] = {h264, hevc, av1, vp8, mpeg2, jpeg, vpp, vp9};
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) if (paths[i] && len) paths[i][0] = 0;
    return 0;
}
static int phase, cap_done, out_done, queued, reinitialized, stopped, bad_timestamp, exported;
int __wrap_poll(struct pollfd *p, nfds_t n, int timeout)
{
    if (timeout > 0) phase = queued;
    for (nfds_t i = 0; i < n; i++) p[i].revents = p[i].events;
    return n;
}
static int hook(int fd, unsigned long command, void *data)
{
    (void)fd;
    if (command == MEDIA_REQUEST_IOC_QUEUE) { queued++; return 0; }
    if (command == MEDIA_REQUEST_IOC_REINIT) { reinitialized++; return 0; }
    if (command == VIDIOC_STREAMOFF) { stopped++; return 0; }
    if (command == VIDIOC_QBUF) return 0;
    if (command == VIDIOC_EXPBUF) {
        struct v4l2_exportbuffer *e = data;
        assert(e->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE && !e->plane && e->index < 2);
        assert(phase == 2 && cap_done == 2 && reinitialized == 2);
        e->fd = memfd_create("request-export", MFD_CLOEXEC); assert(e->fd >= 0 && !ftruncate(e->fd, 384)); exported++; return 0;
    }
    if (command == VIDIOC_DQBUF) {
        struct v4l2_buffer *b = data;
        if (b->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
            if (out_done >= queued) { errno = EAGAIN; return -1; }
            b->index = 1 - out_done++; return 0;
        }
        if (cap_done >= phase) { errno = EAGAIN; return -1; }
        b->index = cap_done; b->timestamp.tv_usec = bad_timestamp ? 999 : cap_done;
        b->m.planes[0].bytesused = 384; cap_done++; return 0;
    }
    errno = EINVAL; return -1;
}
static void run(int corrupt, unsigned early_export)
{
    struct vav4l2_driver_data *dd = calloc(1, sizeof(*dd)); assert(dd);
    struct VADriverContext driver = {.vtable = &table, .pDriverData = dd};
    struct vav4l2_surface s[2] = {{0}};
    struct vav4l2_context ctx = {.context_id = 1, .driver_data = dd, .codec = VAV4L2_CODEC_H264, .media_fd = -1, .request_fd = -1,
        .v4l2_fd = open("/dev/null", O_RDWR | O_CLOEXEC), .streamed = 1, .width = 16, .height = 16,
        .output_bufs_allocd = 2, .output_buf_size = 64, .capture_bufs_allocd = 2,
        .cap_width = 16, .cap_height = 16, .cap_stride = 16, .cap_sizeimage = 384, .capture_buf_size = 384, .cap_pixelformat = V4L2_PIX_FMT_NV12};
    assert(ctx.v4l2_fd >= 0 && !vav4l2_request_pipeline_init(&ctx));
    dd->contexts = &ctx; phase = cap_done = out_done = queued = reinitialized = stopped = bad_timestamp = exported = 0;
    for (unsigned i = 0; i < 2; i++) {
        s[i] = (struct vav4l2_surface){.surface_id = i+1, .driver_data = dd, .format = VA_FOURCC_NV12,
            .width = 16, .height = 16, .memfd_fd = -1, .buf_index = -1, .status = VASurfaceRendering, .timestamp = i*1000};
        dd->surfaces[i+1] = &s[i]; vav4l2_cap_pool_push(&ctx, i); vav4l2_out_pool_push(&ctx, i);
        ctx.capture_buf_ptr[i] = mmap(NULL, 384, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        assert(ctx.capture_buf_ptr[i] != MAP_FAILED); memset(ctx.capture_buf_ptr[i], 0x11+i, 384);
    }
    vav4l2_set_ioctl_hook(hook);
    if (early_export) {
        VADRMPRIMESurfaceDescriptor descriptor, previous;
        memset(&descriptor, 0xa5, sizeof(descriptor)); previous = descriptor;
        assert(table.vaExportSurfaceHandle(&driver, 1, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
            VA_EXPORT_SURFACE_READ_ONLY, &descriptor) == VA_STATUS_ERROR_SURFACE_BUSY &&
            !memcmp(&descriptor, &previous, sizeof(descriptor)) && !exported);
    }
    for (unsigned i = 0; i < 2; i++) {
        assert(!vav4l2_request_begin(&ctx));
        ctx.request_fd = memfd_create("request", MFD_CLOEXEC); assert(ctx.request_fd >= 0);
        ctx.current_surface = &s[i]; ctx.current_surface_id = i+1;
        if (i) assert(vav4l2_validate_reference(&ctx, 1) == VA_STATUS_SUCCESS);
        int index = ctx.free_out_bufs[--ctx.n_free_out];
        int capture = vav4l2_decode_submit(&ctx, index, 8, i*1000);
        assert(capture == 1-(int)i && !vav4l2_surface_pull_capture(&ctx, &s[i], capture));
        s[i].h264_frame_num = i + 7; s[i].h264_frame_num_valid = 1;
        assert(ctx.request_fd == -1 && !s[i].has_pic && s[i].status == VASurfaceRendering);
    }
    ctx.current_surface = NULL;
    assert(queued == 2 && vav4l2_request_pending(&ctx, 1) && vav4l2_request_busy(&ctx, 1));
    assert(vav4l2_request_sync(&ctx, &s[0], 0) == VA_STATUS_ERROR_TIMEDOUT && !ctx.n_free_out);
    if (early_export) {
        VADRMPRIME3SurfaceDescriptor descriptor, previous;
        memset(&descriptor, 0xa5, sizeof(descriptor)); previous = descriptor;
        unsigned type = early_export == 2 ? VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_3 : VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
        unsigned flags = VA_EXPORT_SURFACE_READ_ONLY | (early_export == 2 ? VA_EXPORT_SURFACE_COMPOSED_LAYERS : VA_EXPORT_SURFACE_SEPARATE_LAYERS);
        assert(table.vaExportSurfaceHandle(&driver, 1, type, VA_EXPORT_SURFACE_WRITE_ONLY, &descriptor) == VA_STATUS_ERROR_SURFACE_BUSY &&
            !memcmp(&descriptor, &previous, sizeof(descriptor)) && !phase && !exported);
        bad_timestamp = corrupt;
        VAStatus status = table.vaExportSurfaceHandle(&driver, 1, type, flags, &descriptor);
        if (corrupt) assert(status == VA_STATUS_ERROR_DECODING_ERROR && !memcmp(&descriptor, &previous, sizeof(descriptor)) && !exported && stopped == 2);
        else {
            assert(status == VA_STATUS_SUCCESS && exported == 1 && phase == 2 && s[0].buf_index == 0 && s[0].has_pic);
            assert(descriptor.fourcc == VA_FOURCC_NV12 && descriptor.num_objects == 1 && descriptor.objects[0].size == 384 &&
                (fcntl(descriptor.objects[0].fd, F_GETFD) & FD_CLOEXEC) && descriptor.layers[0].pitch[0] == 16);
            assert(descriptor.num_layers == (early_export == 2 ? 1u : 2u));
            assert(table.vaSyncSurface(&driver, 1) == VA_STATUS_SUCCESS && table.vaSyncSurface(&driver, 2) == VA_STATUS_SUCCESS);
            assert(ctx.n_free_out == 2 && reinitialized == 2);
            close(descriptor.objects[0].fd);
        }
        goto done;
    }
    phase = 1; bad_timestamp = corrupt;
    if (corrupt) {
        assert(vav4l2_request_pump(&ctx) < 0 && stopped == 2);
        assert(s[0].status == VASurfaceSkipped && s[1].status == VASurfaceSkipped && !vav4l2_request_busy(&ctx, 1));
    } else {
        assert(!vav4l2_request_pump(&ctx) && s[0].status == VASurfaceReady && s[0].buf_index == 0 && s[1].buf_index == 1);
        assert(s[0].cap_view == ctx.capture_buf_ptr[0] && !vav4l2_request_pending(&ctx, 1) && vav4l2_request_busy(&ctx, 1));
        assert(vav4l2_request_sync(&ctx, &s[0], 0) == VA_STATUS_ERROR_TIMEDOUT);
        VADRMPRIMESurfaceDescriptor descriptor = {0}; VAImage image; VASurfaceID reference = 1;
        assert(table.vaExportSurfaceHandle(&driver, 1, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
            VA_EXPORT_SURFACE_WRITE_ONLY, &descriptor) == VA_STATUS_ERROR_SURFACE_BUSY && !descriptor.num_objects);
        assert(table.vaDestroySurfaces(&driver, &reference, 1) == VA_STATUS_ERROR_SURFACE_BUSY);
        assert(table.vaDeriveImage(&driver, 1, &image) == VA_STATUS_ERROR_SURFACE_BUSY);
        assert(table.vaBeginPicture(&driver, 1, 1) == VA_STATUS_SUCCESS && phase == 2 && ctx.current_surface == &s[0]);
        ctx.current_surface = NULL; s[0].status = VASurfaceReady;
        assert(vav4l2_request_sync(&ctx, &s[1], 0) == VA_STATUS_SUCCESS && s[1].buf_index == 1);
        assert(s[0].h264_frame_num_valid && s[0].h264_frame_num == 7 && s[1].h264_frame_num_valid && s[1].h264_frame_num == 8);
        assert(ctx.n_free_out == 2 && reinitialized == 2 && !vav4l2_request_busy(&ctx, 1));
    }
done:
    ctx.current_surface = NULL; vav4l2_capture_exports_release(&ctx); vav4l2_request_finish(&ctx); vav4l2_set_ioctl_hook(NULL);
    for (unsigned i = 0; i < 2; i++) munmap(ctx.capture_buf_ptr[i], 384);
    if (ctx.request_fd >= 0) close(ctx.request_fd);
    close(ctx.v4l2_fd); free(dd);
}
int main(void)
{
    setenv("VAV4L2_EXPBUF_EXPORT", "1", 1); setenv("VAV4L2_GPU_COPY", "0", 1);
    struct VADriverContext driver = {.vtable = &table};
    assert(VAV4L2_DRIVER_INIT(&driver) == VA_STATUS_SUCCESS);
    run(0, 0); run(1, 0); run(0, 1); run(0, 2); run(1, 1); run(1, 2);
    assert(table.vaTerminate(&driver) == VA_STATUS_SUCCESS); return 0;
}
