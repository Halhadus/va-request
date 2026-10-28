/* Real PRIME objects and independent logical-plane checks with mocked DMA.
 * Physical RGA format/crop correctness is tested by va_dmabuf_extended. */
#include "test_config.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <linux/dma-buf.h>
#include <drm_fourcc.h>
#include "va_v4l2.h"
#include "page_size.h"
static unsigned failures, jobs, syncs, active_sync, maps, image_size, expected_raw, selections, padded_formats;
static int fault, capture_fd, last_output, last_capture, processing;
static void expect(int ok, const char *tag) { if (!ok) { fprintf(stderr, "FAIL extended DMA %s fault=%d\n", tag, fault); failures++; } }
void *__real_mmap(void *, size_t, int, int, int, off_t);
#if VAV4L2_TEST_LFS64
void *__real_mmap64(void *, size_t, int, int, int, off64_t);
#endif
int __real_munmap(void *, size_t);
void *__wrap_mmap(void *a, size_t n, int p, int f, int fd, off_t o)
{
    if (processing) expect(active_sync && !(p & PROT_WRITE), "only read-only CPU mappings inside SYNC");
    void *v = __real_mmap(a, n, p, f, fd, o); if (v != MAP_FAILED && processing) maps++; return v;
}
#if VAV4L2_TEST_LFS64
void *__wrap_mmap64(void *a, size_t n, int p, int f, int fd, off64_t o)
{
    if (processing) expect(active_sync && !(p & PROT_WRITE), "LFS CPU mapping inside read SYNC");
    void *v = __real_mmap64(a, n, p, f, fd, o); if (v != MAP_FAILED && processing) maps++; return v;
}
#endif
int __wrap_munmap(void *a, size_t n)
{
    if (processing) expect(active_sync && maps, "unmap before END");
    int ret = __real_munmap(a, n); if (!ret && processing) maps--; return ret;
}
int __wrap_poll(struct pollfd *p, nfds_t count, int timeout) { expect(count == 1 && timeout == 2000, "bounded wait"); p->revents = p->events; return 1; }
static int mock_ioctl(int fd, unsigned long req, void *arg)
{
    (void)fd;
    if (req == VIDIOC_QUERYCAP) { ((struct v4l2_capability *)arg)->capabilities = V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING; return 0; }
    if (req == DMA_BUF_IOCTL_SYNC) {
        struct dma_buf_sync *s = arg; syncs++;
        expect((s->flags & DMA_BUF_SYNC_RW) == DMA_BUF_SYNC_READ, "no CPU write in direct processing");
        if (s->flags & DMA_BUF_SYNC_END) {
            expect(active_sync && !maps, "CPU access ends after unmap"); active_sync = 0;
            if (fault == 5) { errno = EIO; return -1; }
        } else {
            expect(!active_sync && !maps, "non-nested CPU access");
            if (fault == 4) { errno = EIO; return -1; }
            active_sync = 1;
        }
        return 0;
    }
    expect(!active_sync && !maps, "hardware submission outside CPU access");
    if (req == VIDIOC_S_FMT) {
        struct v4l2_format *f = arg;
        expect(f->fmt.pix_mp.pixelformat == expected_raw && f->fmt.pix_mp.num_planes == 1, "kernel-compatible physical format");
        if (fault >= 9) {
            if (fault == 10) f->fmt.pix_mp.width = 64;
            if (f->fmt.pix_mp.width == 128) padded_formats++;
            f->fmt.pix_mp.plane_fmt[0].bytesperline = f->fmt.pix_mp.width;
            f->fmt.pix_mp.plane_fmt[0].sizeimage = f->fmt.pix_mp.width * f->fmt.pix_mp.height * 3 / 2;
            if (fault == 13) f->fmt.pix_mp.plane_fmt[0].sizeimage = vav4l2_test_page_align(image_size) + 1;
        } else f->fmt.pix_mp.plane_fmt[0].sizeimage = image_size;
        return 0;
    }
    if (req == VIDIOC_S_SELECTION && fault >= 9) {
        struct v4l2_selection *s = arg; selections++;
        expect(!s->r.left || fault == 14, "visible origin is not storage padding");
        expect(s->r.width == (fault == 14 ? 40u : 64u) && s->r.height == s->r.width, "visible region stays independent of padded storage width");
        if (fault == 11) { errno = EINVAL; return -1; }
        if (fault == 12) s->r.width--;
    }
    if (req == VIDIOC_QBUF) {
        struct v4l2_buffer *b = arg; expect(b->memory == V4L2_MEMORY_DMABUF && b->length == 1, "actual DMA queue transport"); jobs++;
        if (b->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) capture_fd = last_capture = b->m.planes[0].m.fd;
        else {
            last_output = b->m.planes[0].m.fd;
            unsigned char *pixels = malloc(image_size);
            expect(pixels && pread(last_output, pixels, image_size, 0) == (ssize_t)image_size &&
                pwrite(capture_fd, pixels, image_size, 0) == (ssize_t)image_size, "mock hardware transfers actual held objects"); free(pixels);
        }
    }
    if (req == VIDIOC_DQBUF) ((struct v4l2_buffer *)arg)->m.planes[0].bytesused = image_size;
    return 0;
}
static void exercise(uint32_t format, unsigned scenario)
{
    processing = 0;
    int rgb = format == VA_FOURCC_ARGB || format == VA_FOURCC_BGRA || format == VA_FOURCC_RGBA || format == VA_FOURCC_ABGR;
    int packed = format == VA_FOURCC_RGB565 || format == VA_FOURCC_RGBX || format == VA_FOURCC_XRGB || format == VA_FOURCC_XBGR || format == VA_FOURCC_Y210;
    int semiplanar = format == VA_FOURCC_P010;
    unsigned width = format == VA_FOURCC_422H || format == VA_FOURCC_YV16 ? 128 : 64;
    unsigned pitch = width * (format == VA_FOURCC_RGB565 || semiplanar ? 2 : rgb || packed ? 4 : 1), alpha = format == VA_FOURCC_ARGB || format == VA_FOURCC_ABGR ? 0 : 3;
    if (scenario >= 9) pitch = 128;
    image_size = vav4l2_va_image_size(format, pitch, width);
    expected_raw = format == VA_FOURCC_P010 ? V4L2_PIX_FMT_P010 : format == VA_FOURCC_Y210 ? V4L2_PIX_FMT_Y210 :
        format == VA_FOURCC_ABGR ? V4L2_PIX_FMT_BGRA32 : format == VA_FOURCC_XRGB ? V4L2_PIX_FMT_XRGB32 : format == VA_FOURCC_XBGR ? V4L2_PIX_FMT_BGRX32 :
        format == VA_FOURCC_RGB565 ? V4L2_PIX_FMT_RGB565 : format == VA_FOURCC_RGBX ? V4L2_PIX_FMT_RGBX32 :
        rgb ? format == VA_FOURCC_ARGB ? V4L2_PIX_FMT_ARGB32 : format == VA_FOURCC_RGBA ? V4L2_PIX_FMT_RGBA32 : V4L2_PIX_FMT_ABGR32 :
        (format == VA_FOURCC_YV12 || format == VA_FOURCC_I420) ? V4L2_PIX_FMT_YUV420 : V4L2_PIX_FMT_YUV422P;
    struct vav4l2_prime_layout l; expect(!vav4l2_prime_layout(format, width, width, &l), "public format layout");
    int owner[2] = {memfd_create("extended-source", MFD_CLOEXEC), memfd_create("extended-target", MFD_CLOEXEC)};
    struct vav4l2_surface s[2]; memset(s, 0, sizeof(s));
    unsigned char *pixels = malloc(image_size); expect(pixels != NULL, "source fixture allocation"); if (!pixels) exit(2);
    memset(pixels, 0x31, image_size);
    if (rgb) for (unsigned i = alpha; i < image_size; i += 4) pixels[i] = 255;
    else if (!packed && !semiplanar) { memset(pixels + pitch * width, 0x44, pitch * l.rows[1] / 2); memset(pixels + pitch * width + pitch * l.rows[1] / 2, 0xd2, pitch * l.rows[2] / 2); }
    if (rgb && (scenario == 1 || scenario == 2 || scenario == 3)) pixels[alpha] = 127;
    if (rgb && scenario == 6) pixels[51 * pitch + 51 * 4 + alpha] = 127;
    for (unsigned i = 0; i < 2; i++) {
        if (i) memset(pixels, 0xb9, image_size);
        expect(owner[i] >= 0 && !ftruncate(owner[i], vav4l2_test_page_align(image_size)), "real storage fixture");
        expect(pwrite(owner[i], pixels, image_size, 0) == (ssize_t)image_size, "initialize producer/target");
        VADRMPRIMESurfaceDescriptor d = {.fourcc = format, .width = width, .height = width, .num_objects = 1, .num_layers = 1};
        d.objects[0].fd = owner[i]; d.objects[0].size = vav4l2_test_page_align(image_size);
        d.layers[0].drm_format = l.drm_format; d.layers[0].num_planes = l.planes;
        unsigned offset = 0;
        for (unsigned p = 0; p < l.planes; p++) {
            d.layers[0].pitch[p] = p && l.planes == 3 ? pitch / 2 : pitch; d.layers[0].offset[p] = offset; offset += d.layers[0].pitch[p] * l.rows[p];
        }
        if ((format == VA_FOURCC_YV12 || format == VA_FOURCC_YV16) && scenario != 8) {
            unsigned first = d.layers[0].offset[1]; d.layers[0].offset[1] = d.layers[0].offset[2]; d.layers[0].offset[2] = first;
        }
        s[i] = (struct vav4l2_surface){.surface_id = i + 1, .width = width, .height = width, .format = format,
            .memfd_fd = -1, .import_fd = -1, .gbm_external_fd = -1, .buf_index = -1, .cpu_stride = pitch, .cpu_size = image_size};
        expect(vav4l2_prime_import(&s[i], &d) == VA_STATUS_SUCCESS, "retained public descriptor import");
    }
    free(pixels);
    if (rgb && scenario == 7) { unsigned char zero = 0; expect(pwrite(owner[1], &zero, 1, alpha) == 1, "nonopaque destination outside input contract"); }
    struct vav4l2_driver_data dd = {.vpp_devices_scanned = 1, .num_vpp_devices = 1};
    dd.surfaces[1] = &s[0]; dd.surfaces[2] = &s[1];
    struct vav4l2_vpp_device *dev = &dd.vpp_devices[0]; strcpy(dev->path, "extended-node"); strcpy(dev->identity.card, "rga2");
    const uint32_t raw[] = {V4L2_PIX_FMT_YUV420, V4L2_PIX_FMT_YUV422P, V4L2_PIX_FMT_NV16, V4L2_PIX_FMT_NV61, V4L2_PIX_FMT_ARGB32, V4L2_PIX_FMT_ABGR32, V4L2_PIX_FMT_RGBA32, V4L2_PIX_FMT_RGBX32, V4L2_PIX_FMT_RGB565, V4L2_PIX_FMT_XRGB32, V4L2_PIX_FMT_BGRA32, V4L2_PIX_FMT_BGRX32, V4L2_PIX_FMT_P010, V4L2_PIX_FMT_Y210};
    dev->num_input = dev->num_output = sizeof(raw) / sizeof(raw[0]); memcpy(dev->input, raw, sizeof(raw)); memcpy(dev->output, raw, sizeof(raw));
    struct vav4l2_context ctx = {.codec = VAV4L2_CODEC_VPP, .driver_data = &dd, .current_surface = &s[1],
        .device_path = strdup(dev->path), .v4l2_fd = memfd_create("extended-node", MFD_CLOEXEC)};
    VARectangle region = {12, 12, 40, 40}; int partial = scenario == 3 || scenario == 6 || scenario == 7 || scenario == 14;
    VAProcPipelineParameterBuffer pipe = {.surface = 1, .surface_region = partial ? &region : NULL, .output_region = partial ? &region : NULL};
    struct vav4l2_buffer parameter = {.type = VAProcPipelineParameterBufferType, .size = sizeof(pipe), .data = &pipe};
    struct vav4l2_buffer *buffers[] = {&parameter};
    jobs = syncs = active_sync = maps = selections = padded_formats = 0; last_output = last_capture = -1; fault = scenario; processing = 1;
    setenv("VAV4L2_VPP_DMABUF", scenario == 2 ? "auto" : "required", 1);
    VAStatus st = vav4l2_vpp_run(&ctx, buffers, 1);
    int success = scenario == 0 || scenario == 3 || scenario == 7 || scenario == 9 || scenario == 14;
    expect((st == VA_STATUS_SUCCESS) == success, "success or truthful rejection");
    if (scenario == 2) expect(st == VA_STATUS_ERROR_UNIMPLEMENTED, "nonopaque auto retains existing API rejection");
    expect(jobs == (success ? 2u : 0u) && !active_sync && !maps, "rejections queue no job and all CPU access closes");
    if (!success) { unsigned char got = 0; expect(pread(owner[1], &got, 1, 0) == 1 && got == 0xb9, "pre-submit rejection preserves destination poison"); }
    if (success) {
        if (scenario >= 9) expect(padded_formats == 2 && selections == 2, "both padded queues require exact visible selection");
        if (scenario == 9) expect(vav4l2_vpp_run(&ctx, buffers, 1) == VA_STATUS_SUCCESS && jobs == 4 && padded_formats == 2,
            "warm padded queues keep their storage geometry");
        expect(!s[0].cpu_ptr && !s[1].cpu_ptr && syncs == (rgb ? 2u : 0u), "copy-free processing with RGB read-only validation");
        expect(fcntl(last_output, F_GETFD) < 0 && fcntl(last_capture, F_GETFD) < 0, "temporary queue descriptor ownership");
        if (packed) {
            int read = vav4l2_prime_read(&s[1]); expect(!read, "packed-RGB-public-readback");
            if (!read) for (unsigned i = 0; i < image_size; i++) expect(((unsigned char *)s[1].cpu_ptr)[i] == 0x31, "packed-RGB-visible-bytes-preserved");
        }
        if (!partial && !rgb && !packed && !semiplanar) {
            int read = vav4l2_prime_read(&s[1]); expect(!read, "logical public CPU readback");
            unsigned first = pitch * width, second = first + pitch * l.rows[1] / 2;
            expect(!read && ((unsigned char *)s[1].cpu_ptr)[first] == (format == VA_FOURCC_422H || format == VA_FOURCC_I420 ? 0x44 : 0xd2) &&
                ((unsigned char *)s[1].cpu_ptr)[second] == (format == VA_FOURCC_422H || format == VA_FOURCC_I420 ? 0xd2 : 0x44), "logical chroma order independent of storage order");
        }
    }
    fault = 0; vav4l2_m2m_teardown(&ctx, &ctx.vpp_q); if (ctx.v4l2_fd >= 0) close(ctx.v4l2_fd); free(ctx.device_path);
    for (unsigned i = 0; i < 2; i++) { expect(fcntl(owner[i], F_GETFD) >= 0, "client retains owner FD"); vav4l2_prime_destroy(&s[i]); free(s[i].cpu_ptr); close(owner[i]); }
}
int main(void)
{
    unsetenv("VAV4L2_VPP_TRACE"); unsetenv("VAV4L2_DMABUF_TRACE"); unsetenv("VAV4L2_VPP_DMABUF_SESSION");
    vav4l2_set_ioctl_hook(mock_ioctl);
    exercise(VA_FOURCC_P010, 0); exercise(VA_FOURCC_Y210, 0); exercise(VA_FOURCC_XRGB, 0); exercise(VA_FOURCC_XBGR, 0);
    for (unsigned i = 0; i < 8; i++) exercise(VA_FOURCC_ABGR, i);
    exercise(VA_FOURCC_RGB565, 0); exercise(VA_FOURCC_RGBX, 0);
    exercise(VA_FOURCC_YV12, 0); exercise(VA_FOURCC_422H, 0); exercise(VA_FOURCC_YV16, 0);
    exercise(VA_FOURCC_YV12, 8); exercise(VA_FOURCC_YV16, 8);
    for (unsigned i = 0; i < 8; i++) { exercise(VA_FOURCC_ARGB, i); exercise(VA_FOURCC_BGRA, i); exercise(VA_FOURCC_RGBA, i); }
    for (unsigned i = 9; i <= 14; i++) { exercise(VA_FOURCC_YV12, i); exercise(VA_FOURCC_I420, i); }
    vav4l2_set_ioctl_hook(NULL); unsetenv("VAV4L2_VPP_DMABUF");
    printf("extended DMA: %s (format, alpha, pitch and ownership cases)\n", failures ? "FAIL" : "PASS"); return failures != 0;
}
