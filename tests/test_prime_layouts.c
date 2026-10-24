#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <linux/dma-buf.h>
#include <drm_fourcc.h>
#include "va_v4l2.h"
extern VAStatus __vaDriverInit_1_20(VADriverContextP ctx);
static int failures, starts, ends, active, fail_start, fail_end, poll_failure, poll_calls, sync_again;
static int stale_import_map, fail_cpu_map;
void *__real_mmap(void *, size_t, int, int, int, off_t);
void *__real_mmap64(void *, size_t, int, int, int, off64_t);
static void *map_fixture(void *map, size_t length)
{
    if (map == MAP_FAILED || !stale_import_map) return map;
    stale_import_map = 0;
    void *snapshot = __real_mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (snapshot != MAP_FAILED) memcpy(snapshot, map, length);
    munmap(map, length); return snapshot;
}
void *__wrap_mmap(void *address, size_t length, int prot, int flags, int fd, off_t offset)
{
    if (fail_cpu_map) { errno = ENOMEM; return MAP_FAILED; }
    return map_fixture(__real_mmap(address, length, prot, flags, fd, offset), length);
}
void *__wrap_mmap64(void *address, size_t length, int prot, int flags, int fd, off64_t offset)
{
    if (fail_cpu_map) { errno = ENOMEM; return MAP_FAILED; }
    return map_fixture(__real_mmap64(address, length, prot, flags, fd, offset), length);
}
static void expect(int value, const char *tag) { if (!value) { fprintf(stderr, "FAIL %s\n", tag); failures++; } }
int __wrap_poll(struct pollfd *p, nfds_t n, int timeout)
{
    expect(n == 1 && timeout == 2000, "dma-buf-poll-has-a-bounded-timeout"); poll_calls++;
    if (poll_failure) { p->revents = POLLERR; return 1; }
    p->revents = p->events; return 1;
}
static int sync_ioctl(int fd, unsigned long request, void *argument)
{
    expect(request == DMA_BUF_IOCTL_SYNC && (fcntl(fd, F_GETFD) & FD_CLOEXEC), "sync-uses-only-owned-cloexec-fd");
    if (sync_again) { if (sync_again > 0) sync_again--; errno = EAGAIN; return -1; }
    struct dma_buf_sync *s = argument;
    if (s->flags & DMA_BUF_SYNC_END) { ends++; active--; if (fail_end) { fail_end--; errno = EIO; return -1; } }
    else { starts++; if (fail_start && starts == fail_start) { errno = EIO; return -1; } active++; }
    return 0;
}
static unsigned rt(uint32_t f)
{
    if (f == VA_FOURCC_RGB565) return VA_RT_FORMAT_RGB16;
    if (f == VA_FOURCC_RGBA || f == VA_FOURCC_RGBX) return VA_RT_FORMAT_RGB32;
    if (f == VA_FOURCC_P010) return VA_RT_FORMAT_YUV420_10;
    if (f == VA_FOURCC_Y210) return VA_RT_FORMAT_YUV422_10;
    if (f == VA_FOURCC_ARGB || f == VA_FOURCC_BGRA || f == VA_FOURCC_BGRX) return VA_RT_FORMAT_RGB32;
    if (f == VA_FOURCC_NV12 || f == VA_FOURCC_NV21 || f == VA_FOURCC_I420 || f == VA_FOURCC_YV12) return VA_RT_FORMAT_YUV420;
    return VA_RT_FORMAT_YUV422;
}
static VAStatus create(VADriverContextP d, VADRMPRIMESurfaceDescriptor *desc, unsigned n, VASurfaceID *sid)
{
    VASurfaceAttrib a[] = {
        {.type = VASurfaceAttribMemoryType, .value = {.type = VAGenericValueTypeInteger, .value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2}},
        {.type = VASurfaceAttribPixelFormat, .value = {.type = VAGenericValueTypeInteger, .value.i = desc->fourcc}},
        {.type = VASurfaceAttribExternalBufferDescriptor, .value = {.type = VAGenericValueTypePointer, .value.p = desc}}};
    return d->vtable->vaCreateSurfaces2(d, rt(desc->fourcc), desc->width, desc->height, sid, n, a, 3);
}
static unsigned char value(unsigned plane, unsigned row, unsigned x, unsigned frame) { return 13 + plane * 67 + row * 11 + x * 3 + frame * 17; }
static void pattern(const VADRMPRIMESurfaceDescriptor *d, const struct vav4l2_prime_layout *l, unsigned frame)
{
    unsigned char pixels[4096]; memset(pixels, 0xcb, sizeof(pixels));
    for (unsigned o = 0; o < d->num_objects; o++) {
        memset(pixels, 0xcb, sizeof(pixels));
        for (unsigned i = 0; i < l->planes; i++) {
            unsigned layer = d->num_layers == 1 ? 0 : i, plane = d->num_layers == 1 ? i : 0;
            if (d->layers[layer].object_index[plane] != o) continue;
            for (unsigned y = 0; y < l->rows[i]; y++) for (unsigned x = 0; x < l->row[i]; x++)
                pixels[d->layers[layer].offset[plane] + y * d->layers[layer].pitch[plane] + x] = value(i, y, x, frame);
        }
        expect(pwrite(d->objects[o].fd, pixels, sizeof(pixels), 0) == sizeof(pixels), "fixture-write");
    }
}
static int matches(const VAImage *im, const void *pixels, const struct vav4l2_prime_layout *l, unsigned frame)
{
    for (unsigned i = 0; i < l->planes; i++) for (unsigned y = 0; y < l->rows[i]; y++) for (unsigned x = 0; x < l->row[i]; x++)
        if (((const unsigned char *)pixels)[im->offsets[i] + y * im->pitches[i] + x] != value(i, y, x, frame)) return 0;
    return 1;
}
static void one_format(VADriverContextP driver, uint32_t f, unsigned variant)
{
    struct VADriverVTable *v = driver->vtable; struct vav4l2_driver_data *dd = driver->pDriverData;
    struct vav4l2_prime_layout l; expect(vav4l2_prime_layout(f, 4, 4, &l) == 0, "format-layout");
    VADRMPRIMESurfaceDescriptor d = {.fourcc = f, .width = 4, .height = 4, .num_objects = variant & 4 ? 1 : variant & 1 ? l.planes : 1,
        .num_layers = variant & 2 ? l.planes : 1};
    for (unsigned i = 0; i < d.num_objects; i++) {
        d.objects[i].fd = memfd_create("prime-layout-fixture", 0); d.objects[i].size = 4096;
        expect(d.objects[i].fd >= 0 && ftruncate(d.objects[i].fd, 4096) == 0, "fixture-create");
    }
    for (unsigned i = 0; i < l.planes; i++) {
        unsigned layer = d.num_layers == 1 ? 0 : i, plane = d.num_layers == 1 ? i : 0;
        d.layers[layer].drm_format = d.num_layers == 1 ? l.drm_format : l.plane_format[i];
        d.layers[layer].num_planes = d.num_layers == 1 ? l.planes : 1;
        d.layers[layer].object_index[plane] = d.num_objects == 1 ? 0 : i;
        d.layers[layer].offset[plane] = 16 + (d.num_objects == 1 ? i * 256 : 0);
        d.layers[layer].pitch[plane] = l.row[i] + 8 + 4 * i;
        if (variant & 4) {
            unsigned offset = 16, pitch = 8;
            for (unsigned p = 0; p < l.planes; p++) {
                pitch += l.row[p];
                if ((variant & 1) ? p > i : p < i) offset += l.row[p];
            }
            d.layers[layer].offset[plane] = offset; d.layers[layer].pitch[plane] = pitch;
        }
    }
    pattern(&d, &l, 0); VASurfaceID sid = VA_INVALID_ID;
    expect(create(driver, &d, 1, &sid) == VA_STATUS_SUCCESS && sid != VA_INVALID_ID, "all-formats-composed-separate-multi-object-import");
    if (sid == VA_INVALID_ID) goto close_fds;
    struct vav4l2_surface *s = dd->surfaces[sid];
    expect(s->prime && !s->gbm_bo, "general-linear-import-does-not-require-gbm-format-support");
    VAImage im; VAImageFormat fmt = {.fourcc = f}; void *pixels;
    if (v->vaCreateImage(driver, &fmt, 4, 4, &im) || v->vaMapBuffer(driver, im.buf, &pixels)) { expect(0, "image-create-map"); goto destroy; }
    expect(v->vaGetImage(driver, sid, 0, 0, 4, 4, im.image_id) == VA_STATUS_SUCCESS && matches(&im, pixels, &l, 0), "import-independent-bytes-and-component-order");
    pattern(&d, &l, 1);
    expect(v->vaGetImage(driver, sid, 0, 0, 4, 4, im.image_id) == VA_STATUS_SUCCESS && matches(&im, pixels, &l, 1), "import-every-read-refreshes-all-objects");
    memset(pixels, 0x5a, im.data_size);
    expect(v->vaPutImage(driver, sid, im.image_id, 0, 0, 2, 2, 2, 2, 2, 2) == VA_STATUS_SUCCESS, "all-formats-partial-put-image");
    /* The descriptor supplies the pixel format when the optional PixelFormat
     * attribute is absent; an explicitly conflicting attribute still fails. */
    VASurfaceAttrib implicit[] = {
        {.type = VASurfaceAttribMemoryType, .value = {.type = VAGenericValueTypeInteger, .value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2}},
        {.type = VASurfaceAttribExternalBufferDescriptor, .value = {.type = VAGenericValueTypePointer, .value.p = &d}}};
    VASurfaceID inferred = VA_INVALID_ID;
    expect(v->vaCreateSurfaces2(driver, rt(f), 4, 4, &inferred, 1, implicit, 2) == VA_STATUS_SUCCESS &&
        dd->surfaces[inferred]->format == f, "import-can-infer-the-pixel-format-from-the-descriptor");
    if (inferred != VA_INVALID_ID) v->vaDestroySurfaces(driver, &inferred, 1);
    expect(s->memfd_fd == -1, "import-never-allocates-an-unrelated-snapshot");
    VADRMPRIME3SurfaceDescriptor v3[2] = {{0}};
    for (unsigned i = 0; i < 2; i++) memcpy(&v3[i], &d, offsetof(VADRMPRIME3SurfaceDescriptor, flags));
    VASurfaceAttrib a3[] = {
        {.type = VASurfaceAttribMemoryType, .value = {.type = VAGenericValueTypeInteger, .value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_3}},
        {.type = VASurfaceAttribExternalBufferDescriptor, .value = {.type = VAGenericValueTypePointer, .value.p = v3}}};
    VASurfaceID batch[2] = {1234, 1235};
    expect(v->vaCreateSurfaces2(driver, rt(f), 4, 4, batch, 2, a3, 2) == VA_STATUS_SUCCESS, "PRIME3-array-stride-and-inferred-format");
    if (batch[0] != 1234) v->vaDestroySurfaces(driver, batch, 2);
    for (unsigned fault = 0; fault < 3; fault++) {
        batch[0] = 1234; batch[1] = 1235;
        v3[1].flags = fault == 0 ? VA_SURFACE_EXTBUF_DESC_PROTECTED : fault == 1 ? 1 : 0;
        v3[1].reserved[0] = fault == 2;
        expect(v->vaCreateSurfaces2(driver, rt(f), 4, 4, batch, 2, a3, 2) ==
            (fault == 2 ? VA_STATUS_ERROR_INVALID_PARAMETER : VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE) &&
            batch[0] == 1234 && batch[1] == 1235, "PRIME3-batch-flags-reserved-rejection-is-atomic");
    }
    v3[1].reserved[0] = 0;
    VADRMPRIME3SurfaceDescriptor exported3; memset(&exported3, 0xa5, sizeof(exported3));
    expect(v->vaExportSurfaceHandle(driver, sid, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_3,
        VA_EXPORT_SURFACE_COMPOSED_LAYERS, &exported3) == VA_STATUS_SUCCESS && !exported3.flags,
        "PRIME3-export-initializes-flags");
    for (unsigned i = 0; i < sizeof(exported3.reserved) / sizeof(exported3.reserved[0]); i++) expect(!exported3.reserved[i], "PRIME3-export-zero-reserved");
    for (unsigned i = 0; i < exported3.num_objects; i++) { expect(fcntl(exported3.objects[i].fd, F_GETFD) & FD_CLOEXEC, "PRIME3-export-owned-cloexec-fd"); close(exported3.objects[i].fd); }
    unsigned char bytes[4096];
    for (unsigned o = 0; o < d.num_objects; o++) {
        expect(pread(d.objects[o].fd, bytes, sizeof(bytes), 0) == sizeof(bytes), "read-original-object");
        for (unsigned at = 0; at < sizeof(bytes); at++) {
            int found = 0; unsigned char wanted = 0xcb;
            for (unsigned i = 0; i < l.planes; i++) {
                unsigned layer = d.num_layers == 1 ? 0 : i, plane = d.num_layers == 1 ? i : 0;
                if (d.layers[layer].object_index[plane] != o) continue;
                for (unsigned y = 0; y < l.rows[i]; y++) for (unsigned x = 0; x < l.row[i]; x++) {
                    if (at != d.layers[layer].offset[plane] + y * d.layers[layer].pitch[plane] + x) continue;
                    unsigned x0 = l.row[i] / 2, y0 = l.rows[i] / 2;
                    wanted = x >= x0 && y >= y0 ? 0x5a : value(i, y, x, 1); found = 1;
                }
            }
            (void)found; expect(bytes[at] == wanted, "partial-write-preserves-background-gaps-and-all-padding");
        }
    }
    for (unsigned flags = 0; flags < 2; flags++) {
        VADRMPRIMESurfaceDescriptor e;
        expect(v->vaExportSurfaceHandle(driver, sid, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
            VA_EXPORT_SURFACE_READ_WRITE | (flags ? VA_EXPORT_SURFACE_SEPARATE_LAYERS : VA_EXPORT_SURFACE_COMPOSED_LAYERS), &e) == VA_STATUS_SUCCESS,
            "reexport-supports-composed-separate-and-read-write");
        expect(e.num_objects == d.num_objects && e.num_layers == (flags && l.planes > 1 ? l.planes : 1), "reexport-keeps-object-topology");
        for (unsigned i = 0; i < e.num_objects; i++) {
            struct stat a, b; fstat(e.objects[i].fd, &a); fstat(d.objects[i].fd, &b);
            expect(a.st_dev == b.st_dev && a.st_ino == b.st_ino && (fcntl(e.objects[i].fd, F_GETFD) & FD_CLOEXEC) && !fcntl(d.objects[i].fd, F_GETFD), "reexport-is-same-object-with-independent-cloexec-fd"); close(e.objects[i].fd);
        }
    }
    if (f == VA_FOURCC_NV12 || f == VA_FOURCC_P010 || f == VA_FOURCC_YUY2 || f == VA_FOURCC_Y210) {
        unsigned char capture[64]; memset(capture, 0x55, sizeof(capture));
        uint32_t cf = f == VA_FOURCC_NV12 ? V4L2_PIX_FMT_NV12 : f == VA_FOURCC_P010 ? V4L2_PIX_FMT_NV15 :
            f == VA_FOURCC_YUY2 ? V4L2_PIX_FMT_NV16 : V4L2_PIX_FMT_NV20;
        unsigned stride = cf == V4L2_PIX_FMT_NV15 || cf == V4L2_PIX_FMT_NV20 ? 5 : 4;
        unsigned size = vav4l2_capture_plane_size(cf, stride, 4);
        struct vav4l2_context decode = {.driver_data = dd, .v4l2_fd = d.objects[0].fd, .capture_bufs_allocd = 1,
            .capture_buf_ptr = {capture}, .capture_buf_size = size, .cap_sizeimage = size, .cap_stride = stride,
            .cap_width = 4, .cap_height = 4, .cap_pixelformat = cf};
        expect(vav4l2_surface_pull_capture(&decode, s, 0) == 0 && s->prime_capture_valid,
            "native-capture-writes-imported-nv12-p010-yuy2-y210-storage");
        expect(v->vaGetImage(driver, sid, 0, 0, 4, 4, im.image_id) == VA_STATUS_SUCCESS &&
            vav4l2_validate_reference(&decode, sid) == VA_STATUS_SUCCESS, "import-readback-keeps-native-dpb-reference-valid");
        expect(((unsigned char *)pixels)[0] == (f == VA_FOURCC_P010 || f == VA_FOURCC_Y210 ? 0x40 : 0x55),
            "capture-conversion-has-independent-first-sample-byte-oracle");
        fail_end = 1;
        expect(vav4l2_surface_pull_capture(&decode, s, 0) < 0 && !s->prime_capture_valid && !active,
            "decoder-writeback-error-is-not-a-successful-imported-frame");
        expect(v->vaPutImage(driver, sid, im.image_id, 0, 0, 4, 4, 0, 0, 4, 4) == VA_STATUS_SUCCESS &&
            vav4l2_validate_reference(&decode, sid) != VA_STATUS_SUCCESS, "cpu-write-invalidates-native-decoder-reference");
        s->buf_index = -1; s->cap_view = NULL; s->capture_ctx = NULL;
    }
    poll_failure = 1;
    expect(v->vaGetImage(driver, sid, 0, 0, 4, 4, im.image_id) == VA_STATUS_ERROR_OPERATION_FAILED && !active, "implicit-fence-error-does-not-touch-cache"); poll_failure = 0;
    fail_start = starts + d.num_objects;
    expect(v->vaGetImage(driver, sid, 0, 0, 4, 4, im.image_id) == VA_STATUS_ERROR_OPERATION_FAILED && !active, "partial-multi-object-start-failure-ends-previous-objects"); fail_start = 0;
    fail_end = 1;
    expect(v->vaPutImage(driver, sid, im.image_id, 0, 0, 4, 4, 0, 0, 4, 4) == VA_STATUS_ERROR_OPERATION_FAILED && !active &&
        v->vaGetImage(driver, sid, 0, 0, 4, 4, im.image_id) == VA_STATUS_ERROR_OPERATION_FAILED, "write-end-error-cannot-return-stale-success");
    expect(v->vaPutImage(driver, sid, im.image_id, 0, 0, 4, 4, 0, 0, 4, 4) == VA_STATUS_SUCCESS, "full-write-recovers-dirty-import");
    VAImage derived; expect(v->vaDeriveImage(driver, sid, &derived) == VA_STATUS_ERROR_OPERATION_FAILED, "import-does-not-expose-detached-direct-image");
    v->vaUnmapBuffer(driver, im.buf); v->vaDestroyImage(driver, im.image_id);
    for (unsigned fault = 0; fault < 8; fault++) {
        VADRMPRIMESurfaceDescriptor bad = d; VASurfaceID untouched = 1234;
        if (fault == 0) bad.num_objects = 5;
        if (fault == 1) bad.num_layers = 4;
        if (fault == 2) bad.layers[0].object_index[0] = d.num_objects;
        if (fault == 3) bad.layers[0].offset[0] = UINT32_MAX;
        if (fault == 4) bad.layers[0].pitch[0] = l.row[0] - 1;
        if (fault == 5) bad.objects[0].size = 4097;
        if (fault == 6) bad.objects[0].drm_format_modifier = DRM_FORMAT_MOD_INVALID;
        if (fault == 7) bad.layers[0].drm_format = DRM_FORMAT_RGB888;
        expect(create(driver, &bad, 1, &untouched) != VA_STATUS_SUCCESS && untouched == 1234, "invalid-import-remains-atomic");
    }
    if ((variant & 4) && l.planes > 1) {
        for (unsigned late = 0; late < 2; late++) {
            VADRMPRIMESurfaceDescriptor bad = d; VASurfaceID untouched = 1234;
            unsigned layer = d.num_layers == 1 ? 0 : 1, plane = d.num_layers == 1 ? 1 : 0;
            bad.layers[layer].offset[plane] = d.layers[0].offset[0] + (late ? l.rows[0] - 1 : 0) * d.layers[0].pitch[0] + l.row[0] - 1;
            expect(create(driver, &bad, 1, &untouched) == VA_STATUS_ERROR_INVALID_PARAMETER && untouched == 1234,
                "real-first-or-last-row-pixel-overlap-remains-atomic-rejection");
            bad.num_objects = 2; bad.objects[1] = bad.objects[0]; bad.layers[layer].object_index[plane] = 1; untouched = 1234;
            expect(create(driver, &bad, 1, &untouched) == VA_STATUS_ERROR_INVALID_PARAMETER && untouched == 1234,
                "different-object-indices-cannot-hide-aliased-pixel-overlap");
        }
    }
    VADRMPRIMESurfaceDescriptor pair[] = {d, d}; pair[1].objects[0].fd = -1; VASurfaceID untouched[] = {1234, 1235};
    expect(create(driver, pair, 2, untouched) != VA_STATUS_SUCCESS && untouched[0] == 1234 && untouched[1] == 1235, "multi-surface-rollback-does-not-publish-earlier-surface");
    for (unsigned i = 0; i < d.num_objects; i++) { close(d.objects[i].fd); d.objects[i].fd = -1; }
    expect(vav4l2_prime_read(s) == 0, "surface-survives-closing-every-client-fd");
destroy:
    v->vaDestroySurfaces(driver, &sid, 1);
close_fds:
    for (unsigned i = 0; i < d.num_objects; i++) if (d.objects[i].fd >= 0) close(d.objects[i].fd);
}
static int mapping_matches(const struct vav4l2_surface *surface, const unsigned char *pixels)
{
    for (unsigned y = 0; y < 4; y++) if (memcmp((unsigned char *)surface->cpu_ptr + y * surface->cpu_stride, pixels + y * 16, 16)) return 0;
    return 1;
}
static void external_mapping_checks(void)
{
    int owner = memfd_create("prime-mapping-generation", MFD_CLOEXEC);
    if (owner < 0 || ftruncate(owner, 4096)) { expect(0, "prime-mapping-fixture"); if (owner >= 0) close(owner); return; }
    unsigned char pixels[64]; memset(pixels, 0x19, sizeof(pixels));
    expect(pwrite(owner, pixels, sizeof(pixels), 0) == sizeof(pixels), "prime-initial-bytes");
    struct vav4l2_surface surface = {.width = 4, .height = 4, .format = VA_FOURCC_BGRX, .memfd_fd = -1, .cpu_stride = 64, .cpu_size = 256};
    VADRMPRIMESurfaceDescriptor d = {.fourcc = VA_FOURCC_BGRX, .width = 4, .height = 4, .num_objects = 1, .num_layers = 1};
    d.objects[0].fd = owner; d.objects[0].size = 4096; d.layers[0].drm_format = DRM_FORMAT_XRGB8888;
    d.layers[0].num_planes = 1; d.layers[0].pitch[0] = 16;
    vav4l2_set_ioctl_hook(sync_ioctl); stale_import_map = 1;
    expect(vav4l2_prime_import(&surface, &d) == VA_STATUS_SUCCESS && !stale_import_map, "prime-import-probes-mappable-storage");
    memset(pixels, 0x42, sizeof(pixels)); expect(pwrite(owner, pixels, sizeof(pixels), 0) == sizeof(pixels), "external-device-publishes-new-generation");
    vav4l2_prime_device_written(&surface, 1);
    expect(vav4l2_prime_read(&surface) == 0 && mapping_matches(&surface, pixels) && !active,
        "prime-read-maps-current-storage-after-device-ownership-not-import-snapshot");
    fail_cpu_map = 1;
    expect(vav4l2_prime_read(&surface) < 0 && !active, "prime-map-failure-still-ends-cpu-access");
    fail_cpu_map = 0;
    expect(vav4l2_prime_read(&surface) == 0 && mapping_matches(&surface, pixels), "prime-map-failure-can-retry-with-current-bytes");
    vav4l2_prime_destroy(&surface); free(surface.cpu_ptr); close(owner); vav4l2_set_ioctl_hook(NULL);
}
static void modifier_checks(VADriverContextP driver)
{
    uint64_t values[] = {DRM_FORMAT_MOD_INVALID, DRM_FORMAT_MOD_LINEAR};
    VADRMFormatModifierList list = {.num_modifiers = 2, .modifiers = values};
    VASurfaceAttrib a = {.type = VASurfaceAttribDRMFormatModifiers, .value = {.type = VAGenericValueTypePointer, .value.p = &list}};
    VASurfaceID id = 1234;
    expect(driver->vtable->vaCreateSurfaces2(driver, VA_RT_FORMAT_RGB32, 4, 4, &id, 1, &a, 1) == VA_STATUS_SUCCESS, "allocation-selects-LINEAR-from-modifier-list");
    if (id != 1234) driver->vtable->vaDestroySurfaces(driver, &id, 1);
    id = 1234; list.num_modifiers = 1;
    expect(driver->vtable->vaCreateSurfaces2(driver, VA_RT_FORMAT_RGB32, 4, 4, &id, 1, &a, 1) == VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE && id == 1234,
        "unsupported-modifier-must-not-silently-allocate-LINEAR");
    list.num_modifiers = 0;
    expect(driver->vtable->vaCreateSurfaces2(driver, VA_RT_FORMAT_RGB32, 4, 4, &id, 1, &a, 1) == VA_STATUS_ERROR_INVALID_PARAMETER, "empty-modifier-list-rejected");
    list.num_modifiers = 2; list.modifiers = NULL;
    expect(driver->vtable->vaCreateSurfaces2(driver, VA_RT_FORMAT_RGB32, 4, 4, &id, 1, &a, 1) == VA_STATUS_ERROR_INVALID_PARAMETER, "NULL-modifier-array-rejected");
    VADRMFormatModifierList batch[] = {{.num_modifiers = 2, .modifiers = values}, {.num_modifiers = 1, .modifiers = values}};
    a.value.value.p = batch; VASurfaceID ids[] = {1234, 1235};
    expect(driver->vtable->vaCreateSurfaces2(driver, VA_RT_FORMAT_RGB32, 4, 4, ids, 2, &a, 1) == VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE &&
        ids[0] == 1234 && ids[1] == 1235, "every-per-surface-modifier-list-is-validated-before-allocation");
    batch[1].num_modifiers = 2;
    expect(driver->vtable->vaCreateSurfaces2(driver, VA_RT_FORMAT_RGB32, 4, 4, ids, 2, &a, 1) == VA_STATUS_SUCCESS, "per-surface-LINEAR-modifier-lists-accepted");
    if (ids[0] != 1234) driver->vtable->vaDestroySurfaces(driver, ids, 2);
}
int main(void)
{
    external_mapping_checks();
    setenv("VAV4L2_PROBE_NOCACHE", "1", 1);
    struct VADriverVTable v = {0}; struct VADriverContext driver = {.vtable = &v};
    if (__vaDriverInit_1_20(&driver)) return 1;
    uint32_t formats[] = {VA_FOURCC_NV12, VA_FOURCC_NV21, VA_FOURCC_I420, VA_FOURCC_YV12, VA_FOURCC_P010,
        VA_FOURCC_YUY2, VA_FOURCC_UYVY, VA_FOURCC_VYUY, VA_FOURCC_YVYU, VA_FOURCC_422H, VA_FOURCC_YV16,
        VA_FOURCC_Y210, VA_FOURCC_ARGB, VA_FOURCC_BGRA, VA_FOURCC_BGRX, VA_FOURCC_RGB565, VA_FOURCC_RGBA, VA_FOURCC_RGBX};
    modifier_checks(&driver);
    vav4l2_set_ioctl_hook(sync_ioctl);
    for (unsigned i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
        struct vav4l2_prime_layout l; expect(!vav4l2_prime_layout(formats[i], 4, 4, &l), "fixture-plane-count");
        for (unsigned variant = 0; variant < (l.planes > 1 ? 8u : 4u); variant++) one_format(&driver, formats[i], variant);
    }
    expect(active == 0 && starts > 0 && ends > 0 && poll_calls > 0, "sync-and-fence-fixtures-were-exercised");
    int retry_fd = memfd_create("sync-retry", MFD_CLOEXEC);
    sync_again = 2;
    expect(vav4l2_dmabuf_sync(retry_fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ) == 0 && !sync_again &&
        vav4l2_dmabuf_sync(retry_fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ) == 0, "transient-cache-sync-eagain-is-retried");
    sync_again = -1;
    expect(vav4l2_dmabuf_sync(retry_fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ) < 0 && errno == ETIMEDOUT,
        "permanent-cache-sync-eagain-has-a-deadline");
    sync_again = 0; close(retry_fd);
    v.vaTerminate(&driver); vav4l2_set_ioctl_hook(NULL);
    printf("prime_layouts: %s\n", failures ? "FAIL" : "PASS"); return failures != 0;
}
