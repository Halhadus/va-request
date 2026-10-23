/* Exercise buffer operations through the real driver vtable. */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include "va_request.h"

extern VAStatus __vaDriverInit_1_20(VADriverContextP ctx);
static int failures;
static void expect(int ok, const char *tag)
{
    if (!ok) { fprintf(stderr, "FAIL %s\n", tag); failures++; }
}
static void collector(void)
{
    VASliceParameterBufferAV1 tiles[33] = {0};
    uint8_t data[6] = {0};
    struct vareq_buffer params = {.type = VASliceParameterBufferType, .size = sizeof(tiles[0]), .num_elements = 4, .data = tiles};
    struct vareq_buffer payload = {.type = VASliceDataBufferType, .size = 2, .num_elements = 3, .data = data};
    struct vareq_buffer *b[] = {&params, &payload};
    struct vareq_collected c;
    vareq_collect_decode_buffers(b, 2, &c);
    expect(c.n_slice_params == 4 && c.slice_params[3] == &tiles[3] && c.largest_size == 6, "element-array-and-byte-count");
    params.num_elements = 33;
    vareq_collect_decode_buffers(b, 2, &c);
    expect(c.n_slice_params == 32 && c.n_slice_params_seen == 33, "untruncated-seen-count");
    params.allocated_elements = 33; params.num_elements = 0;
    vareq_collect_decode_buffers(b, 2, &c);
    expect(c.n_slice_params == 0, "zero-valid-elements");
}
static void capture_after_context(VADriverContextP driver)
{
    struct vareq_driver_data *dd = driver->pDriverData;
    uint8_t pixels[24];

    for (unsigned i = 0; i < sizeof(pixels); i++) pixels[i] = (uint8_t)(i + 17);
    for (int derive = 0; derive < 2; derive++) {
        VASurfaceID surface_id;
        VAImage derived = {0}, image = {0};
        VAImageFormat format = {.fourcc = VA_FOURCC_NV12};
        void *borrowed = NULL, *output = NULL;
        struct vareq_context *decode = calloc(1, sizeof(*decode));
        void *capture = mmap(NULL, sizeof(pixels), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

        if (!decode || capture == MAP_FAILED ||
            driver->vtable->vaCreateSurfaces(driver, 4, 4, VA_RT_FORMAT_YUV420, 1, &surface_id) != VA_STATUS_SUCCESS) {
            expect(0, "capture-lifetime-setup");
            free(decode);
            if (capture != MAP_FAILED) munmap(capture, sizeof(pixels));
            return;
        }
        memcpy(capture, pixels, sizeof(pixels));
        decode->context_id = ++dd->next_context_id;
        decode->driver_data = dd;
        decode->v4l2_fd = decode->media_fd = decode->request_fd = -1;
        decode->capture_bufs_allocd = 1;
        decode->capture_buf_ptr[0] = capture;
        decode->capture_buf_size = sizeof(pixels);
        decode->next = dd->contexts;
        dd->contexts = decode;
        struct vareq_surface *surface = dd->surfaces[surface_id];
        surface->has_pic = 1;
        surface->buf_index = 0;
        surface->cap_view = capture;
        surface->stride = surface->aligned_h = 4;
        surface->cap_fourcc = V4L2_PIX_FMT_NV12;
        if (derive) {
            expect(driver->vtable->vaDeriveImage(driver, surface_id, &derived) == VA_STATUS_ERROR_OPERATION_FAILED,
                   "live-capture-requires-indirect-image");
            expect(driver->vtable->vaCreateImage(driver, &format, 4, 4, &image) == VA_STATUS_SUCCESS &&
                   driver->vtable->vaGetImage(driver, surface_id, 0, 0, 4, 4, image.image_id) == VA_STATUS_SUCCESS,
                   "live-capture-indirect-fallback");
            driver->vtable->vaMapBuffer(driver, image.buf, &borrowed);
            expect(borrowed && borrowed != capture, "indirect-image-owns-its-storage");
        }
        expect(driver->vtable->vaDestroyContext(driver, decode->context_id) == VA_STATUS_SUCCESS,
               "destroy-context-before-download");
        expect(surface->has_pic && surface->buf_index == -1 && !surface->cap_view,
               "unregistered-surface-detaches-before-unmap");
        uint8_t cached[sizeof(pixels)];
        expect(pread(surface->memfd_fd, cached, sizeof(cached), 0) == (ssize_t)sizeof(cached) &&
               !memcmp(cached, pixels, sizeof(pixels)), "teardown-preserves-capture-bytes");
        if (derive) {
            if (borrowed && borrowed != capture)
                expect(!memcmp(borrowed, pixels, 4), "indirect-image-survives-context-teardown");
            driver->vtable->vaDestroyImage(driver, image.image_id);
            expect(driver->vtable->vaDeriveImage(driver, surface_id, &derived) == VA_STATUS_SUCCESS,
                   "detached-snapshot-is-direct-surface-backing");
            void *direct = NULL;
            driver->vtable->vaMapBuffer(driver, derived.buf, &direct);
            if (direct) ((uint8_t *)direct)[0] = ++pixels[0];
            driver->vtable->vaUnmapBuffer(driver, derived.buf);
            driver->vtable->vaDestroyImage(driver, derived.image_id);
        }
        if (!surface->cap_view) {
            expect(driver->vtable->vaCreateImage(driver, &format, 4, 4, &image) == VA_STATUS_SUCCESS,
                   "post-context-image");
            expect(driver->vtable->vaGetImage(driver, surface_id, 0, 0, 4, 4, image.image_id) == VA_STATUS_SUCCESS,
                   "post-context-readback");
            driver->vtable->vaMapBuffer(driver, image.buf, &output);
            if (output) {
                for (int row = 0; row < 4; row++)
                    expect(!memcmp((uint8_t *)output + row * image.pitches[0], pixels + row * 4, 4),
                           "post-context-luma-exact");
                for (int row = 0; row < 2; row++)
                    expect(!memcmp((uint8_t *)output + image.offsets[1] + row * image.pitches[1], pixels + 16 + row * 4, 4),
                           "post-context-chroma-exact");
            }
            driver->vtable->vaDestroyImage(driver, image.image_id);
        }
        driver->vtable->vaDestroySurfaces(driver, &surface_id, 1);
    }
}

static unsigned fake_cap_width, fake_cap_height, requested_cap_count;
static int capture_ioctl(int fd, unsigned long request, void *arg)
{
    (void)fd;
    if (request == VIDIOC_S_FMT) {
        struct v4l2_format *fmt = arg;
        if (fmt->type != V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) return -1;
        fake_cap_width = fmt->fmt.pix_mp.width;
        fake_cap_height = fmt->fmt.pix_mp.height;
        fmt->fmt.pix_mp.num_planes = 1;
        return 0;
    }
    if (request == VIDIOC_REQBUFS) {
        struct v4l2_requestbuffers *req = arg;
        if (req->type != V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) return -1;
        if (req->count) requested_cap_count = req->count;
        return 0;
    }
    if (request == VIDIOC_G_FMT) {
        struct v4l2_format *fmt = arg;
        if (fmt->type != V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) return -1;
        fmt->fmt.pix_mp.width = fake_cap_width;
        fmt->fmt.pix_mp.height = fake_cap_height;
        fmt->fmt.pix_mp.num_planes = 1;
        fmt->fmt.pix_mp.plane_fmt[0].bytesperline = fake_cap_width;
        fmt->fmt.pix_mp.plane_fmt[0].sizeimage = fake_cap_width * fake_cap_height * 3 / 2;
        return 0;
    }
    return -1;
}
static void av1_capture_pool(void)
{
    struct vareq_context c = {.codec = VAREQ_CODEC_AV1, .v4l2_fd = 99,
        .media_fd = -1, .request_fd = -1};
    vareq_set_ioctl_hook(capture_ioctl);
    expect(vareq_ensure_capture(&c, 1280, 720, V4L2_PIX_FMT_NV12) == 0 &&
           requested_cap_count == 40 && c.capture_bufs_allocd == 40 &&
           c.n_free_cap == 40 && c.free_cap_bufs[39] == 39, "av1-forty-capture-slots");
    expect(vareq_ensure_capture(&c, 3840, 2160, V4L2_PIX_FMT_NV12) == 0 &&
           requested_cap_count == VAREQ_NUM_CAPTURE_BUFS_AV1_4K &&
           c.n_free_cap == VAREQ_NUM_CAPTURE_BUFS_AV1_4K, "av1-4k-cma-budget");
    vareq_set_ioctl_hook(NULL);
    {
        struct vareq_context teardown = {.v4l2_fd = -1, .media_fd = -1, .request_fd = -1};
        void *last = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        expect(last != MAP_FAILED, "last-capture-map-setup");
        if (last != MAP_FAILED) {
            teardown.capture_buf_ptr[39] = last;
            teardown.capture_buf_size = 4096;
            vareq_release_context_device(&teardown);
            expect(!teardown.capture_buf_ptr[39], "last-capture-map-cleanup");
            if (teardown.capture_buf_ptr[39]) munmap(last, 4096);
        }
    }
}

static void buffer_lifetime(VADriverContextP driver)
{
    struct vareq_driver_data *dd = driver->pDriverData;
    struct vareq_context *c = calloc(1, sizeof(*c));
    VABufferID ids[2];
    VASurfaceID sid;
    VAImage image;
    void *data;
    expect(c != NULL, "lifetime-context-alloc");
    if (!c) return;
    c->context_id = ++dd->next_context_id;
    c->v4l2_fd = c->media_fd = c->request_fd = -1;
    c->next = dd->contexts; dd->contexts = c;
    expect(driver->vtable->vaCreateSurfaces(driver, 4, 4, VA_RT_FORMAT_YUV420, 1, &sid) == VA_STATUS_SUCCESS,
           "lifetime-surface");
    c->current_surface = dd->surfaces[sid];
    expect(driver->vtable->vaCreateBuffer(driver, c->context_id, VASliceDataBufferType, 8, 1, NULL, &ids[0]) == VA_STATUS_SUCCESS,
           "lifetime-buffer");
    ids[1] = VA_INVALID_ID;
    expect(driver->vtable->vaRenderPicture(driver, c->context_id, ids, 2) == VA_STATUS_ERROR_INVALID_BUFFER &&
           c->num_pending_buffers == 0, "render-batch-atomic");
    expect(driver->vtable->vaRenderPicture(driver, c->context_id, ids, 1) == VA_STATUS_SUCCESS, "render-live-buffer");
    expect(driver->vtable->vaDestroyBuffer(driver, ids[0]) == VA_STATUS_SUCCESS && !c->pending_buffers[0], "destroy-pending-detaches");
    expect(driver->vtable->vaEndPicture(driver, c->context_id) == VA_STATUS_ERROR_INVALID_BUFFER, "end-rejects-destroyed-buffer");
    expect(driver->vtable->vaCreateBuffer(driver, c->context_id, VASliceDataBufferType, 8, 1, NULL, &ids[0]) == VA_STATUS_SUCCESS,
           "context-owned-buffer");
    expect(driver->vtable->vaDestroyContext(driver, c->context_id) == VA_STATUS_SUCCESS, "context-frees-buffers");
    expect(driver->vtable->vaMapBuffer(driver, ids[0], &data) == VA_STATUS_ERROR_INVALID_BUFFER, "context-buffer-id-released");
    struct vareq_surface *surf = dd->surfaces[sid];
    surf->status = VASurfaceReady;
    expect(vareq_surface_ensure_cpu(surf) == 0, "derive-cpu-alloc");
    expect(driver->vtable->vaDeriveImage(driver, sid, &image) == VA_STATUS_SUCCESS, "derive-cpu-lifetime");
    expect(driver->vtable->vaDestroySurfaces(driver, &sid, 1) == VA_STATUS_ERROR_SURFACE_BUSY, "live-image-protects-surface");
    expect(driver->vtable->vaDestroyBuffer(driver, image.buf) == VA_STATUS_SUCCESS && !surf->memfd_borrows,
           "destroy-image-buffer-releases-borrow");
    expect(driver->vtable->vaDestroySurfaces(driver, &sid, 1) == VA_STATUS_SUCCESS, "destroy-after-image-buffer");
}
static void memfd_growth(void)
{
    struct vareq_surface s = {.width = 4, .height = 4, .format = VA_FOURCC_NV12, .memfd_fd = -1};
    expect(vareq_surface_alloc_export_fd(&s) == 0, "growth-memfd");
    void *first = vareq_surface_map_memfd(&s, 4096);
    expect(first != NULL, "growth-first-map");
    if (first) {
        memset(first, 0x73, 4096); s.memfd_borrows = 1;
        expect(vareq_surface_map_memfd(&s, 8192) != NULL, "growth-with-borrow");
        expect(vareq_surface_map_memfd(&s, 16384) == NULL && ((uint8_t *)first)[0] == 0x73,
               "second-growth-keeps-old-borrow-valid");
    }
    if (s.memfd_map) munmap(s.memfd_map, s.memfd_map_size);
    if (s.memfd_retired) munmap(s.memfd_retired, s.memfd_retired_size);
    if (s.memfd_fd >= 0) close(s.memfd_fd);
}

static void image_geometry(VADriverContextP driver)
{
    struct vareq_driver_data *dd = driver->pDriverData;
    VAImageFormat formats[18] = {0}, nv12 = {.fourcc = VA_FOURCC_NV12};
    int count = -1;
    VASurfaceID sid;
    VAImage input, output, tiny, derived;
    uint8_t *pixels, *copy;
    expect(driver->vtable->vaQueryImageFormats(driver, formats, &count) == VA_STATUS_SUCCESS &&
           count == 18 && driver->max_image_formats == 18 && formats[0].fourcc == VA_FOURCC_NV12 && formats[7].fourcc == VA_FOURCC_ARGB &&
           formats[8].fourcc == VA_FOURCC_NV21 && formats[9].fourcc == VA_FOURCC_YV12 && formats[10].fourcc == VA_FOURCC_UYVY && formats[11].fourcc == VA_FOURCC_VYUY &&
           formats[12].fourcc == VA_FOURCC_YVYU && formats[13].fourcc == VA_FOURCC_422H &&
           formats[14].fourcc == VA_FOURCC_YV16,
           "query-image-formats-count-is-output");
    expect(formats[15].fourcc == VA_FOURCC_RGB565 && formats[15].byte_order == VA_LSB_FIRST &&
        formats[15].bits_per_pixel == 16 && formats[15].red_mask == 0xf800 && formats[15].green_mask == 0x7e0 && formats[15].blue_mask == 0x1f &&
        formats[16].fourcc == VA_FOURCC_RGBA && formats[16].red_mask == 0xff && formats[16].alpha_mask == 0xff000000 &&
        formats[17].fourcc == VA_FOURCC_RGBX && formats[17].depth == 24 && !formats[17].alpha_mask,
        "RGB-image-formats-have-standard-byte-order-and-channel-masks");
    expect(driver->vtable->vaCreateSurfaces(driver, 4, 4, VA_RT_FORMAT_YUV422_10, 1, &sid) == VA_STATUS_SUCCESS &&
           dd->surfaces[sid]->format == VA_FOURCC_Y210, "ten-bit-422-default-is-y210");
    driver->vtable->vaDestroySurfaces(driver, &sid, 1);
    VAImageFormat i420 = {.fourcc = VA_FOURCC_I420};
    expect(driver->vtable->vaCreateImage(driver, &i420, 4, 3, &tiny) == VA_STATUS_SUCCESS && tiny.num_planes == 3 &&
           tiny.pitches[1] == tiny.pitches[0] / 2 && tiny.offsets[2] == tiny.offsets[1] + tiny.pitches[1] * 2 &&
           tiny.data_size == tiny.offsets[2] + tiny.pitches[2] * 2, "i420-three-planes-odd-height");
    driver->vtable->vaDestroyImage(driver, tiny.image_id);
    expect(driver->vtable->vaCreateImage(driver, &nv12, INT_MAX, 4, &tiny) == VA_STATUS_ERROR_INVALID_PARAMETER,
           "reject-image-dimension-overflow");
    expect(driver->vtable->vaCreateImage(driver, &nv12, 4, 4, &input) == VA_STATUS_SUCCESS, "upload-image");
    expect(driver->vtable->vaCreateSurfaces(driver, 4, 4, VA_RT_FORMAT_YUV420, 1, &sid) == VA_STATUS_SUCCESS, "upload-surface");
    driver->vtable->vaMapBuffer(driver, input.buf, (void **)&pixels);
    memset(pixels, 0x11, input.data_size);
    memset(pixels + input.offsets[1], 0x7a, input.data_size - input.offsets[1]);
    expect(driver->vtable->vaPutImage(driver, sid, input.image_id, 0, 0, 2, 2, 0, 0, 2, 2) == VA_STATUS_SUCCESS,
           "upload-subrectangle");
    struct vareq_surface *surf = dd->surfaces[sid];
    expect(((uint8_t *)surf->cpu_ptr)[surf->cpu_stride * 4] == 0x7a &&
           ((uint8_t *)surf->cpu_ptr)[surf->cpu_stride * 2] == 0, "upload-uses-image-and-surface-uv-offsets");
    expect(driver->vtable->vaCreateImage(driver, &nv12, 4, 4, &output) == VA_STATUS_SUCCESS, "download-image");
    driver->vtable->vaMapBuffer(driver, output.buf, (void **)&copy);
    memset(copy, 0x55, output.data_size);
    expect(driver->vtable->vaGetImage(driver, sid, 0, 0, 2, 2, output.image_id) == VA_STATUS_SUCCESS &&
           copy[output.offsets[1]] == 0x7a && copy[output.pitches[0] * 2] == 0x55, "download-keeps-image-uv-offset");
    expect(driver->vtable->vaCreateImage(driver, &nv12, 2, 2, &tiny) == VA_STATUS_SUCCESS, "small-image");
    expect(driver->vtable->vaGetImage(driver, sid, 0, 0, 4, 4, tiny.image_id) == VA_STATUS_ERROR_INVALID_PARAMETER,
           "reject-small-download-image");
    expect(driver->vtable->vaPutImage(driver, sid, tiny.image_id, 0, 0, 4, 4, 0, 0, 4, 4) == VA_STATUS_ERROR_INVALID_PARAMETER,
           "reject-small-upload-image");
    expect(driver->vtable->vaPutImage(driver, sid, input.image_id, 0, 0, 2, 2, 0, 0, 4, 4) == VA_STATUS_ERROR_INVALID_PARAMETER,
           "reject-unimplemented-upload-scaling");
    unsigned char stale_capture[24] = {0};
    surf->cap_view = stale_capture; surf->buf_index = 0; surf->has_pic = 1;
    surf->cap_fourcc = V4L2_PIX_FMT_NV12; surf->stride = surf->aligned_h = 4;
    expect(driver->vtable->vaGetImage(driver, sid, 0, 0, 2, 2, output.image_id) == VA_STATUS_SUCCESS &&
           copy[0] == 0x11 && copy[output.offsets[1]] == 0x7a,
           "uploaded-pixels-win-over-stale-capture");
    surf->cap_view = NULL; surf->buf_index = -1;
    expect(driver->vtable->vaDeriveImage(driver, sid, &derived) == VA_STATUS_SUCCESS && derived.format.fourcc == VA_FOURCC_NV12,
           "derive-cpu-format");
    expect(driver->vtable->vaGetImage(driver, sid, 0, 0, 2, 2, output.image_id) == VA_STATUS_ERROR_SURFACE_BUSY,
           "derived-image-surface-busy");
    void *direct = NULL;
    driver->vtable->vaMapBuffer(driver, derived.buf, &direct);
    if (direct) ((uint8_t *)direct)[0] = 0x33;
    driver->vtable->vaUnmapBuffer(driver, derived.buf);
    driver->vtable->vaDestroyImage(driver, derived.image_id);
    expect(driver->vtable->vaGetImage(driver, sid, 0, 0, 2, 2, output.image_id) == VA_STATUS_SUCCESS &&
           copy[0] == 0x33, "direct-cpu-image-writes-reach-surface");
    surf->has_pic = 1; surf->last_writer = VAREQ_WRITER_MEMFD; surf->cap_fourcc = V4L2_PIX_FMT_NV15;
    expect(driver->vtable->vaDeriveImage(driver, sid, &derived) == VA_STATUS_ERROR_OPERATION_FAILED,
           "packed-nv15-is-not-p010-image");
    driver->vtable->vaDestroyImage(driver, tiny.image_id);
    driver->vtable->vaDestroyImage(driver, input.image_id);
    driver->vtable->vaDestroyImage(driver, output.image_id);
    driver->vtable->vaDestroySurfaces(driver, &sid, 1);
    expect(vareq_capture_plane_size(V4L2_PIX_FMT_NV12, 64, 3) == 320 &&
           vareq_va_image_size(VA_FOURCC_NV12, 64, 3) == 320, "odd-height-includes-final-chroma-row");
    expect(vareq_capture_plane_size(V4L2_PIX_FMT_NV12, UINT_MAX, UINT_MAX) == 0 &&
           vareq_va_image_size(VA_FOURCC_P010, UINT_MAX, UINT_MAX) == 0, "image-size-overflow-rejected");
}

static void surface_contracts(VADriverContextP driver)
{
    struct vareq_driver_data *dd = driver->pDriverData;
    VASurfaceID sid = VA_INVALID_ID;
    VASurfaceAttrib pixel = {.type = VASurfaceAttribPixelFormat,
        .flags = VA_SURFACE_ATTRIB_SETTABLE,
        .value = {.type = VAGenericValueTypeInteger, .value.i = VA_FOURCC_NV12}};
    unsigned invalid_rt[] = {0, VA_RT_FORMAT_YUV444, VA_FOURCC_NV12,
        VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_YUV420_10};
    unsigned before = dd->next_surface_id;
    for (unsigned i = 0; i < sizeof(invalid_rt) / sizeof(invalid_rt[0]); i++) {
        expect(driver->vtable->vaCreateSurfaces2(driver, invalid_rt[i], 4, 4, &sid, 1, NULL, 0) ==
               VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT, "reject-unknown-or-ambiguous-rt");
        expect(driver->vtable->vaCreateSurfaces(driver, 4, 4, invalid_rt[i], 1, &sid) ==
               VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT, "legacy-rejects-fourcc-as-rt");
    }
    expect(driver->vtable->vaCreateSurfaces2(driver, VA_RT_FORMAT_YUV420_10, 4, 4, &sid, 1, &pixel, 1) ==
           VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT, "reject-nv12-for-ten-bit-rt");
    pixel.value.value.i = VA_FOURCC_P010;
    expect(driver->vtable->vaCreateSurfaces2(driver, VA_RT_FORMAT_YUV420, 4, 4, &sid, 1, &pixel, 1) ==
           VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT, "reject-p010-for-eight-bit-rt");
    pixel.value.type = VAGenericValueTypePointer;
    expect(driver->vtable->vaCreateSurfaces2(driver, VA_RT_FORMAT_YUV420, 4, 4, &sid, 1, &pixel, 1) ==
           VA_STATUS_ERROR_INVALID_PARAMETER, "reject-noninteger-pixel-format");
    expect(dd->next_surface_id == before && sid == VA_INVALID_ID,
           "invalid-surface-format-allocates-nothing");
    static const struct { unsigned rt, fourcc; } pairs[] = {
        {VA_RT_FORMAT_YUV420, VA_FOURCC_NV12}, {VA_RT_FORMAT_YUV420, VA_FOURCC_I420},
        {VA_RT_FORMAT_YUV420_10, VA_FOURCC_P010}, {VA_RT_FORMAT_YUV422, VA_FOURCC_YUY2},
        {VA_RT_FORMAT_YUV422_10, VA_FOURCC_Y210}, {VA_RT_FORMAT_RGB32, VA_FOURCC_BGRX},
        {VA_RT_FORMAT_RGB32, VA_FOURCC_BGRA}, {VA_RT_FORMAT_RGB32, VA_FOURCC_ARGB},
    };
    uint32_t vpp_formats[] = {VA_FOURCC_NV12, VA_FOURCC_I420, VA_FOURCC_NV21, VA_FOURCC_YV12,
        VA_FOURCC_YUY2, VA_FOURCC_BGRX, VA_FOURCC_BGRA, VA_FOURCC_ARGB, VA_FOURCC_UYVY, VA_FOURCC_VYUY, VA_FOURCC_YVYU, VA_FOURCC_422H, VA_FOURCC_YV16};
    dd->vpp_caps_ready = 1;
    dd->vpp_caps = (VAProcPipelineCaps){.num_input_pixel_formats = 13, .input_pixel_format = vpp_formats,
        .num_output_pixel_formats = 13, .output_pixel_format = vpp_formats,
        .min_input_width = 34, .min_input_height = 48, .min_output_width = 128, .min_output_height = 64,
        .max_input_width = 8192, .max_input_height = 4096, .max_output_width = 8128, .max_output_height = 8176};
    for (unsigned i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
        struct vareq_config config = {.config_id = ++dd->next_config_id, .rt_format = pairs[i].rt,
            .entrypoint = pairs[i].rt == VA_RT_FORMAT_YUV420_10 || pairs[i].rt == VA_RT_FORMAT_YUV422_10
                ? VAEntrypointVLD : VAEntrypointVideoProc, .next = dd->configs};
        VASurfaceAttrib attrs[16];
        unsigned count = 0, n = 16, pixels = 0, found = 0;
        dd->configs = &config;
        expect(driver->vtable->vaQuerySurfaceAttributes(driver, config.config_id, NULL, &count) ==
               VA_STATUS_SUCCESS, "surface-attribute-count");
        expect(driver->vtable->vaQuerySurfaceAttributes(driver, config.config_id, attrs, &n) ==
               VA_STATUS_SUCCESS && n == count, "surface-attributes");
        for (unsigned j = 0; j < n; j++) {
            if (attrs[j].type == VASurfaceAttribMemoryType)
                expect((unsigned)attrs[j].value.value.i == (VA_SURFACE_ATTRIB_MEM_TYPE_VA |
                    VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2 | VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_3),
                    "prime-import-is-advertised-for-supported-linear-formats");
            if (attrs[j].type != VASurfaceAttribPixelFormat) continue;
            pixels++;
            unsigned fcc = attrs[j].value.value.i;
            if (fcc == pairs[i].fourcc) found++;
            expect((pairs[i].rt == VA_RT_FORMAT_YUV420 && (fcc == VA_FOURCC_NV12 || fcc == VA_FOURCC_I420 || fcc == VA_FOURCC_NV21 || fcc == VA_FOURCC_YV12)) ||
                   (pairs[i].rt == VA_RT_FORMAT_YUV420_10 && fcc == VA_FOURCC_P010) ||
                   (pairs[i].rt == VA_RT_FORMAT_YUV422 && (fcc == VA_FOURCC_YUY2 || fcc == VA_FOURCC_UYVY || fcc == VA_FOURCC_VYUY || fcc == VA_FOURCC_YVYU || fcc == VA_FOURCC_422H || fcc == VA_FOURCC_YV16)) ||
                   (pairs[i].rt == VA_RT_FORMAT_YUV422_10 && fcc == VA_FOURCC_Y210) ||
                   (pairs[i].rt == VA_RT_FORMAT_RGB32 && (fcc == VA_FOURCC_BGRX || fcc == VA_FOURCC_BGRA || fcc == VA_FOURCC_ARGB)),
                   "enumeration-stays-within-config-rt");
        }
        expect(pixels > 0 && found == 1, "requested-format-is-enumerated");
        for (unsigned j = 0; j < n; j++) {
            int vpp = config.entrypoint == VAEntrypointVideoProc;
            if (attrs[j].type == VASurfaceAttribMinWidth)
                expect(attrs[j].value.value.i == (vpp ? 34 : 16), "surface-width-min-follows-device-envelope-only-for-vpp");
            if (attrs[j].type == VASurfaceAttribMinHeight)
                expect(attrs[j].value.value.i == (vpp ? 48 : 16), "surface-height-min-follows-device-envelope-only-for-vpp");
            if (attrs[j].type == VASurfaceAttribMaxWidth)
                expect(attrs[j].value.value.i == (vpp ? 8192 : 4096), "surface-width-max-is-not-a-decoder-limit-for-vpp");
            if (attrs[j].type == VASurfaceAttribMaxHeight)
                expect(attrs[j].value.value.i == (vpp ? 8176 : 4096), "surface-height-max-unions-input-and-output-roles");
        }
        if (pairs[i].rt == VA_RT_FORMAT_YUV420) {
            config.entrypoint = VAEntrypointVLD; n = 16;
            expect(driver->vtable->vaQuerySurfaceAttributes(driver, config.config_id, attrs, &n) ==
                   VA_STATUS_SUCCESS && n == 6 && attrs[0].value.value.i == VA_FOURCC_NV12,
                   "decode-enumerates-nv12-not-unimplemented-i420");
            config.entrypoint = VAEntrypointEncPicture; n = 16;
            expect(driver->vtable->vaQuerySurfaceAttributes(driver, config.config_id, attrs, &n) ==
                   VA_STATUS_SUCCESS && n == 9 && attrs[0].value.value.i == VA_FOURCC_NV12 &&
                   attrs[1].value.value.i == VA_FOURCC_I420 && attrs[2].value.value.i == VA_FOURCC_NV21 &&
                   attrs[3].value.value.i == VA_FOURCC_YV12, "jpeg-enumerates-the-four-implemented-420-inputs");
        }
        dd->configs = config.next;
        pixel.value.type = VAGenericValueTypeInteger; pixel.value.value.i = pairs[i].fourcc;
        expect(driver->vtable->vaCreateSurfaces2(driver, pairs[i].rt, 4, 4, &sid, 1, &pixel, 1) ==
               VA_STATUS_SUCCESS && dd->surfaces[sid]->format == pairs[i].fourcc, "compatible-surface-format");
        driver->vtable->vaDestroySurfaces(driver, &sid, 1);
    }
    struct vareq_config combined = {.config_id = ++dd->next_config_id,
        .rt_format = VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_YUV422 | VA_RT_FORMAT_RGB32,
        .entrypoint = VAEntrypointVideoProc, .next = dd->configs};
    VASurfaceAttrib combined_attrs[18], untouched[18]; unsigned combined_count = 17;
    memset(combined_attrs, 0xa5, sizeof(combined_attrs)); memcpy(untouched, combined_attrs, sizeof(untouched));
    dd->configs = &combined;
    expect(driver->vtable->vaQuerySurfaceAttributes(driver, combined.config_id, combined_attrs, &combined_count) ==
           VA_STATUS_ERROR_MAX_NUM_EXCEEDED && combined_count == 18 && !memcmp(combined_attrs, untouched, sizeof(untouched)),
           "combined-vpp-short-attribute-array-reports-required-capacity-without-writing");
    expect(driver->vtable->vaQuerySurfaceAttributes(driver, combined.config_id, combined_attrs, &combined_count) ==
           VA_STATUS_SUCCESS && combined_count == 18, "all-thirteen-vpp-layouts-and-five-attributes-fit");
    dd->configs = combined.next;
    uint32_t only_nv21 = VA_FOURCC_NV21;
    dd->vpp_caps.num_input_pixel_formats = dd->vpp_caps.num_output_pixel_formats = 1;
    dd->vpp_caps.input_pixel_format = dd->vpp_caps.output_pixel_format = &only_nv21;
    struct vareq_config narrow = {.config_id = ++dd->next_config_id, .rt_format = VA_RT_FORMAT_YUV420,
        .entrypoint = VAEntrypointVideoProc, .next = dd->configs};
    VASurfaceAttrib narrow_attrs[16]; unsigned narrow_count = 16;
    dd->configs = &narrow;
    expect(driver->vtable->vaQuerySurfaceAttributes(driver, narrow.config_id, narrow_attrs, &narrow_count) ==
           VA_STATUS_SUCCESS && narrow_count == 6 && narrow_attrs[0].value.value.i == VA_FOURCC_NV21,
           "vpp-surface-enumeration-follows-device-formats-without-inventing-yv12");
    expect(narrow_attrs[5].type == VASurfaceAttribMemoryType && narrow_attrs[5].value.value.i == (VA_SURFACE_ATTRIB_MEM_TYPE_VA | VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2 | VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_3),
        "non-nv12-vpp-config-can-import-its-own-linear-format");
    VAConfigAttrib maximum[] = {{.type = VAConfigAttribMaxPictureWidth}, {.type = VAConfigAttribMaxPictureHeight}};
    expect(driver->vtable->vaGetConfigAttributes(driver, VAProfileNone, VAEntrypointVideoProc, maximum, 2) == VA_STATUS_SUCCESS &&
           maximum[0].value == 8192 && maximum[1].value == 8176, "vpp-config-limits-match-surface-envelope");
    char original_path[sizeof(dd->dev_vpp)]; memcpy(original_path, dd->dev_vpp, sizeof(original_path));
    snprintf(dd->dev_vpp, sizeof(dd->dev_vpp), "/dev/mock-vpp-limits");
    VAConfigID accepted = VA_INVALID_ID;
    expect(driver->vtable->vaCreateConfig(driver, VAProfileNone, VAEntrypointVideoProc, maximum, 2, &accepted) == VA_STATUS_SUCCESS,
           "create-config-accepts-its-advertised-vpp-maxima");
    if (accepted != VA_INVALID_ID) {
        VAConfigAttrib queried[8]; int count = 0;
        expect(driver->vtable->vaQueryConfigAttributes(driver, accepted, NULL, NULL, queried, &count) == VA_STATUS_SUCCESS,
               "query-config-with-vpp-maxima");
        for (int i = 0; i < count; i++) {
            if (queried[i].type == VAConfigAttribMaxPictureWidth) expect(queried[i].value == 8192, "query-config-vpp-width-consistency");
            if (queried[i].type == VAConfigAttribMaxPictureHeight) expect(queried[i].value == 8176, "query-config-vpp-height-consistency");
        }
        driver->vtable->vaDestroyConfig(driver, accepted);
    }
    memcpy(dd->dev_vpp, original_path, sizeof(original_path));
    dd->vpp_caps.max_input_width = UINT32_MAX;
    expect(driver->vtable->vaGetConfigAttributes(driver, VAProfileNone, VAEntrypointVideoProc, maximum, 2) == VA_STATUS_SUCCESS &&
           maximum[0].value == UINT16_MAX, "vpp-max-dimension-respects-image-and-rectangle-representation");
    dd->vpp_caps.min_input_width = 0; narrow_count = 16;
    expect(driver->vtable->vaQuerySurfaceAttributes(driver, narrow.config_id, narrow_attrs, &narrow_count) ==
           VA_STATUS_ERROR_OPERATION_FAILED, "invalid-device-envelope-is-not-a-default-size-success");
    dd->configs = narrow.next; dd->vpp_caps_ready = 0; memset(&dd->vpp_caps, 0, sizeof(dd->vpp_caps));
}

static void api_contracts(VADriverContextP driver)
{
    struct vareq_driver_data *dd = driver->pDriverData;
    VAConfigID config = VA_INVALID_ID;
    VAConfigAttrib attr = {.type = VAConfigAttribRTFormat};
    VAProfile profiles[32];
    int n = -1;
    strcpy(dd->dev_h264, "/dev/mock-h264");
    expect(driver->vtable->vaQueryConfigProfiles(driver, profiles, &n) == VA_STATUS_SUCCESS && n > 0 &&
           profiles[0] == VAProfileH264ConstrainedBaseline, "query-profiles-count-is-output");
    expect(driver->vtable->vaCreateConfig(driver, VAProfileH264High, VAEntrypointEncPicture, NULL, 0, &config) ==
           VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT, "reject-h264-jpeg-entrypoint");
    attr.value = VA_RT_FORMAT_RGB32;
    expect(driver->vtable->vaCreateConfig(driver, VAProfileH264High, VAEntrypointVLD, &attr, 1, &config) ==
           VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT, "reject-unsupported-config-rt");
    expect(driver->vtable->vaCreateConfig(driver, VAProfileH264High10, VAEntrypointVLD, NULL, 0, &config) == VA_STATUS_SUCCESS,
           "high10-default-config");
    int count;
    VAConfigAttrib attrs[32];
    expect(driver->vtable->vaQueryConfigAttributes(driver, config, NULL, NULL, attrs, &count) == VA_STATUS_SUCCESS &&
           count > 0 && attrs[0].value == VA_RT_FORMAT_YUV420_10, "high10-default-rt-is-ten-bit");
    expect(count == 4, "decode-config-has-no-encoder-attributes");
    VAConfigAttrib query[] = {
        {.type = VAConfigAttribDecSliceMode}, {.type = VAConfigAttribEncPackedHeaders},
        {.type = VAConfigAttribEncQualityRange}, {.type = VAConfigAttribRateControl},
    };
    expect(driver->vtable->vaGetConfigAttributes(driver, VAProfileH264High10, VAEntrypointVLD, query, 4) ==
           VA_STATUS_SUCCESS && query[0].value == VA_DEC_SLICE_MODE_NORMAL &&
           query[1].value == VA_ATTRIB_NOT_SUPPORTED && query[2].value == VA_ATTRIB_NOT_SUPPORTED &&
           query[3].value == VA_ATTRIB_NOT_SUPPORTED, "decode-capabilities-are-entrypoint-specific");
    VAConfigID rejected = VA_INVALID_ID;
    VAConfigAttrib requested = {.type = VAConfigAttribDecSliceMode, .value = VA_DEC_SLICE_MODE_BASE};
    expect(driver->vtable->vaCreateConfig(driver, VAProfileH264High, VAEntrypointVLD, &requested, 1, &rejected) ==
           VA_STATUS_ERROR_ATTR_NOT_SUPPORTED && rejected == VA_INVALID_ID, "reject-unsupported-slice-mode");
    requested.type = VAConfigAttribEncPackedHeaders; requested.value = 0;
    expect(driver->vtable->vaCreateConfig(driver, VAProfileH264High, VAEntrypointVLD, &requested, 1, &rejected) ==
           VA_STATUS_ERROR_ATTR_NOT_SUPPORTED, "decode-rejects-encoder-attribute");
    expect(driver->vtable->vaGetConfigAttributes(driver, VAProfileJPEGBaseline, VAEntrypointEncPicture, query, 4) ==
           VA_STATUS_SUCCESS && query[0].value == VA_ATTRIB_NOT_SUPPORTED && query[1].value == 0 &&
           query[2].value == 1 && query[3].value == VA_ATTRIB_NOT_SUPPORTED,
           "jpeg-quality-range-is-not-quantization-quality");
    expect(driver->vtable->vaGetConfigAttributes(driver, VAProfileNone, VAEntrypointVideoProc, query, 4) ==
           VA_STATUS_SUCCESS && query[0].value == VA_ATTRIB_NOT_SUPPORTED &&
           query[1].value == VA_ATTRIB_NOT_SUPPORTED && query[2].value == VA_ATTRIB_NOT_SUPPORTED,
           "vpp-has-no-codec-attributes");
    strcpy(dd->dev_jpeg_enc, "/dev/mock-jpeg");
    requested.value = VA_ENC_PACKED_HEADER_RAW_DATA;
    expect(driver->vtable->vaCreateConfig(driver, VAProfileJPEGBaseline, VAEntrypointEncPicture, &requested, 1, &rejected) ==
           VA_STATUS_ERROR_ATTR_NOT_SUPPORTED, "reject-unsupported-packed-headers");
    requested.value = 0;
    expect(driver->vtable->vaCreateConfig(driver, VAProfileJPEGBaseline, VAEntrypointEncPicture, &requested, 1, &rejected) ==
           VA_STATUS_SUCCESS, "jpeg-default-packed-header-mode");
    expect(driver->vtable->vaQueryConfigAttributes(driver, rejected, NULL, NULL, attrs, &count) == VA_STATUS_SUCCESS &&
           count == 5, "jpeg-config-attribute-count");
    for (int i = 0; i < count; i++) {
        VAConfigAttrib cap = {.type = attrs[i].type};
        expect(driver->vtable->vaGetConfigAttributes(driver, VAProfileJPEGBaseline, VAEntrypointEncPicture, &cap, 1) ==
               VA_STATUS_SUCCESS && cap.value == attrs[i].value, "jpeg-query-and-get-agree");
    }
    driver->vtable->vaDestroyConfig(driver, rejected);
    dd->dev_jpeg_enc[0] = 0;
    driver->vtable->vaDestroyConfig(driver, config);
    dd->dev_h264[0] = 0;
    struct vareq_context *c = calloc(1, sizeof(*c));
    if (!c) { expect(0, "validation-context"); return; }
    c->context_id = ++dd->next_context_id; c->codec = VAREQ_CODEC_H264;
    c->v4l2_fd = c->media_fd = c->request_fd = -1;
    c->next = dd->contexts; dd->contexts = c;
    VASurfaceID sid;
    VABufferID id;
    expect(driver->vtable->vaCreateSurfaces(driver, 4, 4, VA_RT_FORMAT_YUV420, 1, &sid) == VA_STATUS_SUCCESS,
           "validation-surface");
    expect(driver->vtable->vaCreateBuffer(driver, c->context_id, VAPictureParameterBufferType, 1, 1, NULL, &id) == VA_STATUS_SUCCESS,
           "tiny-param-allocation");
    c->current_surface = dd->surfaces[sid];
    expect(driver->vtable->vaRenderPicture(driver, c->context_id, &id, 1) == VA_STATUS_SUCCESS &&
           driver->vtable->vaEndPicture(driver, c->context_id) == VA_STATUS_ERROR_INVALID_PARAMETER,
           "tiny-param-rejected-before-translation");
    driver->vtable->vaDestroyContext(driver, c->context_id);
    driver->vtable->vaDestroySurfaces(driver, &sid, 1);
}

int main(void)
{
    struct VADriverVTable v = {0};
    struct VADriverContext ctx = {.vtable = &v};
    VABufferID id = VA_INVALID_ID;
    void *data = NULL, *saved;
    unsigned char input[8] = {1,2,3,4,5,6,7,8};
    setenv("VAREQ_PROBE_NOCACHE", "1", 1);
    setenv("VAREQ_EXPBUF_EXPORT", "1", 1); /* Mock capture fixture uses the EXPBUF path. */
    if (__vaDriverInit_1_20(&ctx) != VA_STATUS_SUCCESS)
        return 1;
    expect(v.vaCreateBuffer(&ctx, 0, VASliceDataBufferType, UINT_MAX, 2, NULL, &id) == VA_STATUS_ERROR_INVALID_PARAMETER && id == VA_INVALID_ID, "allocation-overflow");
    expect(v.vaCreateBuffer(&ctx, 0, VASliceDataBufferType, 0, 1, NULL, &id) == VA_STATUS_ERROR_INVALID_PARAMETER, "zero-size");
    expect(v.vaCreateBuffer(&ctx, 0, VASliceDataBufferType, 4, 2, input, &id) == VA_STATUS_SUCCESS, "orphan-create");
    expect(v.vaMapBuffer(&ctx, id, &data) == VA_STATUS_SUCCESS && !memcmp(data, input, 8), "initial-copy");
    saved = data;
    expect(v.vaBufferSetNumElements(&ctx, id, 1) == VA_STATUS_SUCCESS && v.vaBufferSetNumElements(&ctx, id, 2) == VA_STATUS_SUCCESS, "shrink-regrow");
    v.vaMapBuffer(&ctx, id, &data);
    expect(saved == data, "retained-capacity");
    expect(v.vaBufferSetNumElements(&ctx, id, 4) == VA_STATUS_SUCCESS, "orphan-grow");
    v.vaMapBuffer(&ctx, id, &data);
    expect(!memcmp(data, input, 8), "grow-preserves-data");
    expect(v.vaBufferSetNumElements(&ctx, id, UINT_MAX) == VA_STATUS_ERROR_INVALID_PARAMETER, "resize-overflow");
    expect(v.vaBufferSetNumElements(&ctx, id, 0) == VA_STATUS_SUCCESS, "zero-valid-count");
    expect(v.vaDestroyBuffer(&ctx, id) == VA_STATUS_SUCCESS, "destroy");
    expect(v.vaBufferSetNumElements(&ctx, id, 1) == VA_STATUS_ERROR_INVALID_BUFFER, "invalid-buffer");
    expect(v.vaDestroyImage(&ctx, id) == VA_STATUS_ERROR_INVALID_IMAGE, "destroy-invalid-image");
    expect(v.vaCreateBuffer(&ctx, 0, VAEncCodedBufferType, 32, 1, NULL, &id) == VA_STATUS_SUCCESS, "coded-create");
    expect(v.vaDestroyImage(&ctx, id) == VA_STATUS_ERROR_INVALID_IMAGE, "destroy-image-rejects-coded-buffer");
    v.vaMapBuffer(&ctx, id, &data);
    VACodedBufferSegment *seg = data;
    expect(seg->buf == (uint8_t *)data + sizeof(*seg) && seg->size == 0, "coded-wrapper");
    expect(v.vaBufferSetNumElements(&ctx, id, 2) == VA_STATUS_ERROR_INVALID_PARAMETER && seg->buf == (uint8_t *)data + sizeof(*seg), "coded-resize-safe");
    v.vaDestroyBuffer(&ctx, id);
    struct vareq_driver_data *dd = ctx.pDriverData;
    uint8_t small[6] = {0};
    struct vareq_surface surface = {.width = 4, .height = 4, .cpu_ptr = small,
        .cpu_stride = 2, .cpu_size = sizeof(small), .memfd_fd = -1, .buf_index = -1, .last_writer = VAREQ_WRITER_CPU};
    VAImage image;
    VAImageFormat format = {.fourcc = VA_FOURCC_NV12};
    dd->surfaces[1] = &surface;
    expect(v.vaCreateImage(&ctx, &format, 4, 4, &image) == VA_STATUS_SUCCESS, "readback-image");
    expect(v.vaGetImage(&ctx, 1, 0, 0, 4, 4, image.image_id) == VA_STATUS_ERROR_INVALID_PARAMETER,
           "readback-rejects-short-source-stride");
    v.vaDestroyImage(&ctx, image.image_id);
    surface.width = surface.height = 2;
    expect(v.vaCreateImage(&ctx, &format, 2, 2, &image) == VA_STATUS_SUCCESS, "tight-source-image");
    expect(v.vaGetImage(&ctx, 1, 0, 0, 2, 2, image.image_id) == VA_STATUS_SUCCESS,
           "readback-accepts-tight-valid-source-stride");
    v.vaDestroyImage(&ctx, image.image_id);
    surface.cpu_ptr = NULL;
    surface.memfd_fd = memfd_create("undecoded", MFD_CLOEXEC);
    expect(surface.memfd_fd >= 0 && ftruncate(surface.memfd_fd, 6) == 0, "undecoded-backing");
    expect(v.vaCreateImage(&ctx, &format, 2, 2, &image) == VA_STATUS_SUCCESS, "undecoded-image");
    expect(v.vaGetImage(&ctx, 1, 0, 0, 2, 2, image.image_id) == VA_STATUS_ERROR_INVALID_SURFACE,
           "readback-rejects-undecoded-zero-backing");
    struct vareq_context decode = {.context_id = 99, .codec = VAREQ_CODEC_HEVC,
        .v4l2_fd = -1, .media_fd = -1, .request_fd = -1, .current_surface = &surface};
    dd->contexts = &decode;
    expect(v.vaEndPicture(&ctx, 99) == VA_STATUS_ERROR_OPERATION_FAILED, "missing-request-is-not-success");
    expect(surface.status == VASurfaceSkipped && v.vaSyncSurface(&ctx, 1) == VA_STATUS_ERROR_DECODING_ERROR &&
           surface.status == VASurfaceSkipped, "sync-preserves-decode-error");
    surface.has_pic = 1;
    expect(v.vaGetImage(&ctx, 1, 0, 0, 2, 2, image.image_id) == VA_STATUS_ERROR_INVALID_SURFACE,
           "readback-rejects-failed-reused-surface");
    v.vaDestroyImage(&ctx, image.image_id);
    dd->contexts = NULL;
    if (surface.memfd_fd >= 0) close(surface.memfd_fd);
    dd->surfaces[1] = NULL;
    surface_contracts(&ctx);
    api_contracts(&ctx);
    image_geometry(&ctx);
    buffer_lifetime(&ctx);
    memfd_growth();
    capture_after_context(&ctx);
    av1_capture_pool();
    v.vaTerminate(&ctx);
    collector();
    printf("Buffer failures: %d\n", failures);
    return failures != 0;
}
