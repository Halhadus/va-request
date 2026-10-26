/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <drm_fourcc.h>
#include "v4l2/internal.h"
static int native_fd, exported[2], fail_plane = -1, queued, mode;
static int waits, fail_wait;
static unsigned plane_count = 2, plane_length[3] = {2048, 2048}, plane_step = 4096;
int __wrap_vav4l2_dmabuf_wait(int fd, int write)
{
    assert(write && fcntl(fd, F_GETFD) >= 0); waits++;
    if (fail_wait == waits) { errno = ETIMEDOUT; return -1; }
    return 0;
}
static int hook(int fd, unsigned long cmd, void *arg)
{
    assert(fd == native_fd);
    if (cmd == VIDIOC_QUERYBUF) {
        struct v4l2_buffer *b = arg; assert(b->length == plane_count && b->index == 0);
        for (unsigned p = 0; p < plane_count; p++) { b->m.planes[p].length = plane_length[p]; b->m.planes[p].m.mem_offset = p * plane_step; }
        return 0;
    }
    if (cmd == VIDIOC_QBUF) {
        struct v4l2_buffer *b = arg; assert(b->length == plane_count && b->memory == V4L2_MEMORY_MMAP && !(b->flags & V4L2_BUF_FLAG_REQUEST_FD)); queued++; return 0;
    }
    if (cmd == VIDIOC_DQBUF) {
        struct v4l2_buffer *b = arg; b->index = 0; assert(b->length == plane_count);
        for (unsigned p = 0; p < plane_count; p++) b->m.planes[p].bytesused = mode >= 2 ? 0 : plane_length[p] - (mode == 1 && p == plane_count - 1);
        b->flags = mode == 2 ? V4L2_BUF_FLAG_LAST : 0; return 0;
    }
    if (cmd == VIDIOC_EXPBUF) {
        struct v4l2_exportbuffer *b = arg; assert(b->plane < 2 && b->index == 0);
        if ((int)b->plane == fail_plane) return -1;
        b->fd = dup(exported[b->plane]); return 0;
    }
    assert(!"unexpected capture ioctl"); return -1;
}
static void layouts(void)
{
    const uint32_t formats[] = {V4L2_PIX_FMT_NV21M, V4L2_PIX_FMT_NV16M, V4L2_PIX_FMT_NV61M,
        V4L2_PIX_FMT_YUV420M, V4L2_PIX_FMT_YVU420M, V4L2_PIX_FMT_YUV422M, V4L2_PIX_FMT_YVU422M,
        V4L2_PIX_FMT_NV12MT_16X16, V4L2_PIX_FMT_NV12M_8L128};
    for (unsigned f = 0; f < sizeof(formats) / sizeof(formats[0]); f++) {
        plane_count = vav4l2_capture_layout_planes(formats[f]); plane_step = 65536;
        unsigned full = vav4l2_capture_layout_format(formats[f]) == V4L2_PIX_FMT_NV16;
        unsigned tw = f == 7 ? 16 : f == 8 ? 8 : 0, th = f == 7 ? 16 : f == 8 ? 128 : 0;
        int swapped = f == 0 || f == 2 || f == 4 || f == 6;
        struct v4l2_pix_format_mplane fmt = {.width = 64, .height = 256, .field = V4L2_FIELD_NONE,
            .pixelformat = formats[f], .num_planes = plane_count};
        native_fd = memfd_create("planar-native", MFD_CLOEXEC); assert(native_fd >= 0 && !ftruncate(native_fd, 3 * plane_step));
        uint8_t *map = mmap(NULL, 3 * plane_step, PROT_READ | PROT_WRITE, MAP_SHARED, native_fd, 0); assert(map != MAP_FAILED);
        memset(map, 0xa5, 3 * plane_step);
        for (unsigned p = 0; p < plane_count; p++) {
            unsigned pitch = plane_count == 3 && p ? 40 + p * 8 : 80 + p * 16;
            unsigned rows = p && !full ? 128 : 256, width = p && plane_count == 3 ? 32 : 64;
            plane_length[p] = pitch * rows; fmt.plane_fmt[p].bytesperline = pitch; fmt.plane_fmt[p].sizeimage = plane_length[p];
            for (unsigned y = 0; y < rows; y++) for (unsigned x = 0; x < width; x++) {
                unsigned component = !p ? 0 : plane_count == 3 ? 1 + ((p - 1) ^ swapped) : 1 + ((x & 1) ^ swapped);
                unsigned column = !p ? x : plane_count == 3 ? x : x / 2;
                size_t at = tw ? (size_t)(y / th) * pitch * th + (x / tw) * tw * th + y % th * tw + x % tw : (size_t)y * pitch + x;
                map[p * plane_step + at] = component * 61 + column * 3 + y * 7;
            }
        }
        struct vav4l2_context ctx = {.v4l2_fd = native_fd, .capture_bufs_allocd = 1};
        assert(!vav4l2_capture_layout_set(&ctx, &fmt) && !vav4l2_capture_layout_exportable(&ctx));
        assert(!vav4l2_decoder_queue_capture(&ctx, 0));
        struct v4l2_buffer done;
        assert(!vav4l2_decoder_dequeue_capture(&ctx, &done) && done.bytesused == ctx.cap_sizeimage);
        mode = 1; assert(vav4l2_decoder_dequeue_capture(&ctx, &done) < 0); mode = 0;
        struct vav4l2_surface surface = {.width = 64, .height = 256, .format = full ? VA_FOURCC_YUY2 : VA_FOURCC_NV12, .memfd_fd = -1};
        assert(!vav4l2_capture_layout_pull(&ctx, &surface, 0));
        const uint8_t *canonical = ctx.capture_buf_ptr[0];
        for (unsigned y = 0; y < 256; y++) for (unsigned x = 0; x < 64; x++) assert(canonical[y * ctx.cap_stride + x] == (uint8_t)(x * 3 + y * 7));
        canonical += ctx.cap_stride * 256;
        for (unsigned y = 0; y < (full ? 256u : 128u); y++) for (unsigned x = 0; x < 64; x++)
            assert(canonical[y * ctx.cap_stride + x] == (uint8_t)((1 + (x & 1)) * 61 + x / 2 * 3 + y * 7));
        surface.cap_view = NULL; vav4l2_unmap_capture_buffers(&ctx);
        free(surface.cpu_ptr); if (surface.memfd_fd >= 0) close(surface.memfd_fd);
        munmap(map, 3 * plane_step); close(native_fd);
    }
}
int main(void)
{
    setenv("VAV4L2_EXPBUF_EXPORT", "1", 1);
    native_fd = memfd_create("nv12m-native", MFD_CLOEXEC); assert(native_fd >= 0 && !ftruncate(native_fd, 8192));
    uint8_t *native = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, native_fd, 0); assert(native != MAP_FAILED);
    memset(native, 0x11, 2048); memset(native + 4096, 0x99, 2048);
    for (unsigned y = 0; y < 16; y++) memset(native + 4096 + y * 128, 0x22, 64);
    for (unsigned p = 0; p < 2; p++) { exported[p] = memfd_create("nv12m-export", MFD_CLOEXEC); assert(exported[p] >= 0 && !ftruncate(exported[p], 2048)); }
    struct vav4l2_context ctx = {.v4l2_fd = native_fd, .capture_bufs_allocd = 1};
    struct v4l2_pix_format_mplane fmt = {.width = 64, .height = 32, .pixelformat = V4L2_PIX_FMT_NV12M, .field = V4L2_FIELD_NONE, .num_planes = 2};
    fmt.plane_fmt[0].bytesperline = 64; fmt.plane_fmt[1].bytesperline = 128;
    fmt.plane_fmt[0].sizeimage = fmt.plane_fmt[1].sizeimage = 2048;
    fmt.plane_fmt[1].sizeimage--;
    assert(vav4l2_capture_layout_set(&ctx, &fmt) < 0 && !ctx.capture_layout);
    fmt.plane_fmt[1].sizeimage++;
    fmt.plane_fmt[0].bytesperline = UINT32_MAX / 32; fmt.plane_fmt[0].sizeimage = fmt.plane_fmt[0].bytesperline * 32;
    assert(vav4l2_capture_layout_set(&ctx, &fmt) < 0 && !ctx.capture_layout);
    fmt.plane_fmt[0].bytesperline = 64; fmt.plane_fmt[0].sizeimage = 2048;
    assert(!vav4l2_capture_layout_set(&ctx, &fmt) && ctx.cap_sizeimage == 3072);
    vav4l2_set_ioctl_hook(hook);
    assert(!vav4l2_decoder_queue_capture(&ctx, 0) && queued == 1);
    struct v4l2_buffer done = {0};
    assert(vav4l2_decoder_dequeue_capture(&ctx, &done) == 0 && done.bytesused == 3072);
    mode = 1; assert(vav4l2_decoder_dequeue_capture(&ctx, &done) < 0);
    mode = 2; assert(!vav4l2_decoder_dequeue_capture(&ctx, &done) && !done.bytesused && (done.flags & V4L2_BUF_FLAG_LAST));
    mode = 3; assert(vav4l2_decoder_dequeue_capture(&ctx, &done) < 0); mode = 0;
    struct vav4l2_surface s = {.width = 64, .height = 32, .format = VA_FOURCC_NV12, .memfd_fd = -1};
    assert(!vav4l2_capture_layout_pull(&ctx, &s, 0) && s.has_pic && s.capture_ctx == &ctx);
    const uint8_t *view = s.cap_view;
    for (unsigned i = 0; i < 3072; i++) assert(view[i] == (i < 2048 ? 0x11 : 0x22));
    for (unsigned separate = 0; separate < 2; separate++) {
        VADRMPRIMESurfaceDescriptor d = {0};
        assert(!vav4l2_capture_layout_export(&ctx, &s, separate ? VA_EXPORT_SURFACE_SEPARATE_LAYERS : VA_EXPORT_SURFACE_COMPOSED_LAYERS, &d));
        assert(d.num_objects == 2 && d.num_layers == (separate ? 2u : 1u));
        assert(d.layers[separate ? 1 : 0].object_index[separate ? 0 : 1] == 1 && d.layers[separate ? 1 : 0].pitch[separate ? 0 : 1] == 128);
        for (unsigned p = 0; p < 2; p++) close(d.objects[p].fd);
    }
    waits = 0; assert(!vav4l2_decoder_queue_capture(&ctx, 0) && waits == 2 && queued == 2);
    waits = 0; fail_wait = 2;
    assert(vav4l2_decoder_queue_capture(&ctx, 0) < 0 && queued == 2 && waits == 2);
    fail_wait = 0; vav4l2_capture_exports_release(&ctx);
    VADRMPRIMESurfaceDescriptor d, poison; memset(&d, 0xa5, sizeof(d)); poison = d; fail_plane = 1;
    assert(vav4l2_capture_layout_export(&ctx, &s, VA_EXPORT_SURFACE_COMPOSED_LAYERS, &d) != VA_STATUS_SUCCESS && !memcmp(&d, &poison, sizeof(d)));
    s.cap_view = NULL; vav4l2_unmap_capture_buffers(&ctx); assert(!ctx.capture_layout && !ctx.capture_buf_ptr[0]);
    munmap(native, 8192); close(native_fd);
    for (unsigned p = 0; p < 2; p++) close(exported[p]);
    layouts(); vav4l2_set_ioctl_hook(NULL);
    puts("Planar capture pitches, tile addressing, native objects, LAST and atomic export: PASS"); return 0;
}
