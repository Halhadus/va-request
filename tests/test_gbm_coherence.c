#include "test_config.h"
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <gbm.h>
#include <drm_fourcc.h>
#include <linux/dma-buf.h>
#include "va_v4l2.h"
extern VAStatus VAV4L2_DRIVER_INIT(VADriverContextP ctx);
int __wrap_vav4l2_alloc_drm_storage(int fd, unsigned bytes, unsigned rows, int coherent)
{ (void)fd; (void)bytes; (void)rows; (void)coherent; errno = ENOTSUP; return -1; }
struct fake_bo { uint32_t width, height, pitch; int fd; size_t map_size; unsigned char *pixels; };
static int use_shared_storage, coherent_calls, coherent_fail, last_coherent = -1;
int __wrap_vav4l2_alloc_coherent_storage(unsigned bytes, unsigned rows)
{
    coherent_calls++;
    if (!use_shared_storage || coherent_fail) { errno = ENOMEM; return -1; }
    int fd = memfd_create("coherent-retry-fixture", MFD_CLOEXEC);
    if (fd >= 0 && ftruncate(fd, (size_t)bytes * rows + 4096)) { close(fd); return -1; }
    last_coherent = fd; return fd;
}
int __wrap_vav4l2_alloc_shared_storage(unsigned bytes, unsigned rows)
{
    if (!use_shared_storage) return -1;
    int fd = memfd_create("shared-native-fixture", MFD_CLOEXEC);
    if (fd >= 0 && ftruncate(fd, (size_t)bytes * rows + 4096)) { close(fd); return -1; }
    return fd;
}
static int failures, fail_map, maps, destroys, device_destroys, device_fd = -1, exported;
static int bad_modifier, short_export, last_export = -1;
static int import_calls, fail_import, bad_import_stride, fail_shared_map, stale_gbm_read;
static unsigned char stale_pixels[65536];
void *__real_mmap(void *, size_t, int, int, int, off_t);
void *__wrap_mmap(void *address, size_t length, int prot, int flags, int fd, off_t offset)
{
    if (fail_shared_map) { errno = ENOMEM; return MAP_FAILED; }
    return __real_mmap(address, length, prot, flags, fd, offset);
}
/* Meson enables large-file redirects, including mmap -> mmap64 on glibc. */
#if VAV4L2_TEST_LFS64
void *__real_mmap64(void *, size_t, int, int, int, off64_t);
void *__wrap_mmap64(void *address, size_t length, int prot, int flags, int fd, off64_t offset)
{
    if (fail_shared_map) { errno = ENOMEM; return MAP_FAILED; }
    return __real_mmap64(address, length, prot, flags, fd, offset);
}
#endif
static void expect(int ok, const char *tag)
{
    if (!ok) { fprintf(stderr, "FAIL %s\n", tag); failures++; }
}
static int queue_ioctl(int fd, unsigned long request, void *argument)
{
    (void)fd;
    if (request == VIDIOC_DQBUF) ((struct v4l2_buffer *)argument)->m.planes[0].bytesused = 24;
    return 0;
}
struct gbm_device *__wrap_gbm_create_device(int fd) { device_fd = fd; return (struct gbm_device *)&device_fd; }
void __wrap_gbm_device_destroy(struct gbm_device *device) { (void)device; device_destroys++; }
struct gbm_bo *__wrap_gbm_bo_create(struct gbm_device *device, uint32_t w, uint32_t h, uint32_t format, uint32_t flags)
{
    (void)device; (void)format; (void)flags;
    struct fake_bo *bo = calloc(1, sizeof(*bo));
    if (bo) {
        bo->width = bo->pitch = w; bo->height = h; bo->fd = memfd_create("gbm-fixture", MFD_CLOEXEC);
        if (bo->fd < 0 || ftruncate(bo->fd, (size_t)w * h + 64) < 0) { if (bo->fd >= 0) close(bo->fd); free(bo); return NULL; }
        bo->map_size = (size_t)w * h + 64;
        bo->pixels = mmap(NULL, bo->map_size, PROT_READ | PROT_WRITE, MAP_SHARED, bo->fd, 0);
        if (bo->pixels == MAP_FAILED) { close(bo->fd); free(bo); return NULL; }
    }
    return (struct gbm_bo *)bo;
}
struct gbm_bo *__wrap_gbm_bo_import(struct gbm_device *device, uint32_t type, void *data, uint32_t flags)
{
    (void)device;
    struct gbm_import_fd_modifier_data *in = data;
    expect(type == GBM_BO_IMPORT_FD_MODIFIER && in->num_fds == 1 && !in->offsets[0] && in->modifier == DRM_FORMAT_MOD_LINEAR &&
        in->format == GBM_FORMAT_R8 && flags == GBM_BO_USE_LINEAR && (fcntl(in->fds[0], F_GETFD) & FD_CLOEXEC),
        "import-uses-owned-cloexec-fd-and-explicit-linear-r8-geometry");
    if (++import_calls == fail_import) return NULL;
    struct fake_bo *bo = calloc(1, sizeof(*bo));
    if (!bo) return NULL;
    bo->width = in->width; bo->height = in->height; bo->pitch = in->strides[0];
    bo->fd = fcntl(in->fds[0], F_DUPFD_CLOEXEC, 0); bo->map_size = lseek(in->fds[0], 0, SEEK_END);
    if (bo->fd < 0) { free(bo); return NULL; }
    bo->pixels = mmap(NULL, bo->map_size, PROT_READ | PROT_WRITE, MAP_SHARED, bo->fd, 0);
    if (bo->pixels == MAP_FAILED) { close(bo->fd); free(bo); return NULL; }
    return (struct gbm_bo *)bo;
}
void *__wrap_gbm_bo_map(struct gbm_bo *handle, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                        uint32_t flags, uint32_t *stride, void **data)
{
    (void)flags; maps++;
    struct fake_bo *bo = (struct fake_bo *)handle;
    if (fail_map || x || y || w > bo->width || h > bo->height) return NULL;
    *stride = bo->pitch; *data = bo;
    if (stale_gbm_read && flags == GBM_BO_TRANSFER_READ) return stale_pixels;
    return bo->pixels;
}
void __wrap_gbm_bo_unmap(struct gbm_bo *bo, void *data) { (void)bo; (void)data; }
uint32_t __wrap_gbm_bo_get_stride_for_plane(struct gbm_bo *bo, int plane) { (void)plane; return ((struct fake_bo *)bo)->pitch + bad_import_stride; }
uint64_t __wrap_gbm_bo_get_modifier(struct gbm_bo *bo) { (void)bo; return bad_modifier ? 1 : 0; }
int __wrap_gbm_bo_get_fd_for_plane(struct gbm_bo *bo, int plane)
{
    (void)plane; exported++;
    last_export = short_export ? memfd_create("short-gbm-fixture", 0) : dup(((struct fake_bo *)bo)->fd);
    if (short_export && last_export >= 0 && ftruncate(last_export, 1) < 0) { close(last_export); last_export = -1; }
    return last_export;
}
void __wrap_gbm_bo_destroy(struct gbm_bo *handle)
{
    struct fake_bo *bo = (struct fake_bo *)handle;
    destroys++; munmap(bo->pixels, bo->map_size); close(bo->fd); free(bo);
}
static int sync_active, sync_start_fail, sync_end_fail, sync_eagain, sync_eintr, sync_reads, sync_writes, foreign_queue;
static struct vav4l2_m2m_state *import_queue;
static int import_ioctl(int fd, unsigned long request, void *argument)
{
    expect(fcntl(fd, F_GETFD) >= 0, "import-keeps-a-live-fd");
    if (request == DMA_BUF_IOCTL_SYNC) {
        struct dma_buf_sync *sync = argument; unsigned access = sync->flags & DMA_BUF_SYNC_RW;
        expect((fcntl(fd, F_GETFD) & FD_CLOEXEC) && (access == DMA_BUF_SYNC_READ || access == DMA_BUF_SYNC_WRITE), "import-cpu-access-flags-and-cloexec");
        if (!(sync->flags & DMA_BUF_SYNC_END)) {
            if (sync_eagain) { sync_eagain--; errno = EAGAIN; return -1; }
            if (sync_eintr) { sync_eintr--; errno = EINTR; return -1; }
            if (sync_start_fail) { sync_start_fail--; errno = EIO; return -1; }
            expect(!sync_active, "import-cpu-access-is-not-nested"); sync_active = access;
            if (access == DMA_BUF_SYNC_READ) sync_reads++; else sync_writes++;
        } else {
            expect(sync_active == (int)access, "import-cpu-access-end-matches-start"); sync_active = 0;
            if (sync_end_fail) { sync_end_fail--; errno = EIO; return -1; }
        }
        return 0;
    }
    expect(!sync_active, "v4l2-queue-is-outside-dma-buf-cpu-access");
    if (request == VIDIOC_EXPBUF) { foreign_queue++; errno = EINVAL; return -1; }
    if (import_queue && request == VIDIOC_QBUF && ((struct v4l2_buffer *)argument)->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
        memcpy(import_queue->cap_map, import_queue->out_map[0], 24);
    if (request == VIDIOC_DQBUF) ((struct v4l2_buffer *)argument)->m.planes[0].bytesused = 24;
    return 0;
}
static VAStatus import_surfaces(struct VADriverContext *d, VADRMPRIMESurfaceDescriptor *descs, unsigned count, VASurfaceID *ids)
{
    VASurfaceAttrib attrs[] = {
        {.type = VASurfaceAttribMemoryType, .flags = VA_SURFACE_ATTRIB_SETTABLE,
         .value = {.type = VAGenericValueTypeInteger, .value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2}},
        {.type = VASurfaceAttribExternalBufferDescriptor, .flags = VA_SURFACE_ATTRIB_SETTABLE,
         .value = {.type = VAGenericValueTypePointer, .value.p = descs}}};
    return d->vtable->vaCreateSurfaces2(d, VA_RT_FORMAT_YUV420, 4, 4, ids, count, attrs, 2);
}
static void pattern(unsigned char *data, unsigned frame)
{
    memset(data, 0xcd, 112);
    for (unsigned y = 0; y < 4; y++) for (unsigned x = 0; x < 4; x++) data[y * 8 + x] = 16 + frame * 9 + y * 4 + x;
    for (unsigned y = 0; y < 2; y++) for (unsigned x = 0; x < 2; x++) {
        data[32 + y * 8 + 2 * x] = 64 + frame + y + x; data[33 + y * 8 + 2 * x] = 160 + frame + 3 * y + x;
    }
}
static int image_matches(const VAImage *image, const void *pixels, const unsigned char *want)
{
    for (unsigned y = 0; y < 4; y++) if (memcmp((const char *)pixels + image->offsets[0] + y * image->pitches[0], want + y * 8, 4)) return 0;
    for (unsigned y = 0; y < 2; y++) if (memcmp((const char *)pixels + image->offsets[1] + y * image->pitches[1], want + 32 + y * 8, 4)) return 0;
    return 1;
}
static void display_device_checks(void)
{
    const char *override = getenv("VAV4L2_RENDER_NODE");
    char *saved = override ? strdup(override) : NULL;
    unsetenv("VAV4L2_RENDER_NODE");
    int fd[2] = {memfd_create("display-a", MFD_CLOEXEC), memfd_create("display-b", MFD_CLOEXEC)};
    struct vav4l2_driver_data dd[2] = {0};
    struct vav4l2_surface surfaces[2] = {0};
    for (unsigned i = 0; i < 2; i++) {
        dd[i].gbm.fd = -1; dd[i].gbm.drm_fd = fd[i];
        surfaces[i] = (struct vav4l2_surface){.driver_data = &dd[i], .width = 4, .height = 4,
            .format = VA_FOURCC_NV12, .memfd_fd = -1, .import_fd = -1, .gbm_external_fd = -1};
        expect(fd[i] >= 0 && !vav4l2_gbm_surface_ensure(&surfaces[i]), "display-uses-libva-drm-connection");
        expect(dd[i].gbm.fd >= 0 && dd[i].gbm.fd != fd[i] && (fcntl(dd[i].gbm.fd, F_GETFD) & FD_CLOEXEC),
            "gbm-retains-owned-cloexec-display-fd");
    }
    int retained = dd[1].gbm.fd;
    vav4l2_gbm_surface_destroy(&surfaces[0]); vav4l2_gbm_driver_cleanup(&dd[0]);
    expect(fcntl(fd[0], F_GETFD) >= 0 && fcntl(retained, F_GETFD) >= 0,
        "display-cleanup-preserves-client-fd-and-other-display");
    vav4l2_gbm_surface_destroy(&surfaces[1]); vav4l2_gbm_driver_cleanup(&dd[1]);
    close(fd[0]); close(fd[1]);
    if (saved) { setenv("VAV4L2_RENDER_NODE", saved, 1); free(saved); }
}

static void import_checks(void)
{
    struct VADriverVTable va = {0}; struct VADriverContext driver = {.vtable = &va};
    expect(VAV4L2_DRIVER_INIT(&driver) == VA_STATUS_SUCCESS, "import-driver-init");
    int client = memfd_create("import-client", 0), owner = -1;
    if (client < 0 || ftruncate(client, 112) < 0) { expect(0, "import-fixture-storage"); va.vaTerminate(&driver); if (client >= 0) close(client); return; }
    owner = dup(client); unsigned char data[112], got[112]; pattern(data, 1);
    expect(pwrite(owner, data, sizeof(data), 0) == (ssize_t)sizeof(data), "import-fixture-pixels");
    VADRMPRIMESurfaceDescriptor desc = {.fourcc = VA_FOURCC_NV12, .width = 4, .height = 4, .num_objects = 1, .num_layers = 1};
    desc.objects[0].fd = client; desc.objects[0].size = 112;
    desc.layers[0].drm_format = DRM_FORMAT_NV12; desc.layers[0].num_planes = 2;
    desc.layers[0].offset[1] = 32; desc.layers[0].pitch[0] = desc.layers[0].pitch[1] = 8;
    struct vav4l2_driver_data *dd = driver.pDriverData;
    vav4l2_set_ioctl_hook(import_ioctl);
    VASurfaceID sid = VA_INVALID_ID;
    expect(import_surfaces(&driver, &desc, 1, &sid) == VA_STATUS_SUCCESS && sid != VA_INVALID_ID, "import-linear-padded-nv12");
    if (sid == VA_INVALID_ID) { va.vaTerminate(&driver); close(client); close(owner); vav4l2_set_ioctl_hook(NULL); return; }
    struct vav4l2_surface *s = dd->surfaces[sid]; int held = s->import_fd;
    expect(s->gbm_imported && held != client && (fcntl(held, F_GETFD) & FD_CLOEXEC) && !fcntl(client, F_GETFD), "import-does-not-close-or-change-client-fd-flags");
    VAImage image; VAImageFormat fmt = {.fourcc = VA_FOURCC_NV12}; void *pixels;
    expect(va.vaCreateImage(&driver, &fmt, 4, 4, &image) == VA_STATUS_SUCCESS && va.vaMapBuffer(&driver, image.buf, &pixels) == VA_STATUS_SUCCESS, "import-readback-image");
    expect(va.vaGetImage(&driver, sid, 0, 0, 4, 4, image.image_id) == VA_STATUS_SUCCESS && image_matches(&image, pixels, data), "import-independent-y-uv-byte-oracle");
    pattern(data, 2); expect(pwrite(owner, data, sizeof(data), 0) == (ssize_t)sizeof(data), "external-writer-updates-object");
    sync_eagain = sync_eintr = 1;
    expect(va.vaGetImage(&driver, sid, 0, 0, 4, 4, image.image_id) == VA_STATUS_SUCCESS && image_matches(&image, pixels, data) &&
        !sync_eagain && !sync_eintr && !sync_active, "import-refreshes-snapshot-and-retries-cpu-sync");
    pattern(data, 3); expect(pwrite(owner, data, sizeof(data), 0) == (ssize_t)sizeof(data), "external-before-partial-upload");
    memset(pixels, 0x6a, image.data_size);
    ((unsigned char *)pixels)[image.offsets[1]] = 0xb1; ((unsigned char *)pixels)[image.offsets[1] + 1] = 0xc2;
    expect(va.vaPutImage(&driver, sid, image.image_id, 0, 0, 2, 2, 2, 2, 2, 2) == VA_STATUS_SUCCESS, "import-partial-put-image");
    for (unsigned y = 2; y < 4; y++) for (unsigned x = 2; x < 4; x++) data[y * 8 + x] = 0x6a;
    data[42] = 0xb1; data[43] = 0xc2;
    expect(pread(owner, got, sizeof(got), 0) == (ssize_t)sizeof(got) && !memcmp(got, data, sizeof(data)), "partial-import-write-preserves-current-external-background-and-padding");
    sync_start_fail = 1; memset(pixels, 0xa5, image.data_size);
    expect(va.vaGetImage(&driver, sid, 0, 0, 4, 4, image.image_id) == VA_STATUS_ERROR_OPERATION_FAILED && ((unsigned char *)pixels)[0] == 0xa5 && !sync_active,
        "import-sync-start-failure-does-not-return-stale-pixels");
    sync_end_fail = 1;
    expect(va.vaGetImage(&driver, sid, 0, 0, 4, 4, image.image_id) == VA_STATUS_ERROR_OPERATION_FAILED && ((unsigned char *)pixels)[0] == 0xa5 && !sync_active,
        "import-sync-end-failure-propagates-before-image-copy");
    sync_start_fail = 1;
    expect(va.vaPutImage(&driver, sid, image.image_id, 0, 0, 4, 4, 0, 0, 4, 4) == VA_STATUS_ERROR_OPERATION_FAILED && s->gbm_dirty &&
        va.vaGetImage(&driver, sid, 0, 0, 4, 4, image.image_id) == VA_STATUS_ERROR_OPERATION_FAILED, "failed-import-upload-cannot-expose-old-bo-as-current");
    expect(va.vaPutImage(&driver, sid, image.image_id, 0, 0, 4, 4, 0, 0, 4, 4) == VA_STATUS_SUCCESS && !s->gbm_dirty, "full-import-upload-can-recover");
    sync_end_fail = 1;
    expect(va.vaPutImage(&driver, sid, image.image_id, 0, 0, 4, 4, 0, 0, 4, 4) == VA_STATUS_ERROR_OPERATION_FAILED && s->gbm_dirty && !sync_active,
        "import-write-end-failure-keeps-object-dirty");
    expect(va.vaPutImage(&driver, sid, image.image_id, 0, 0, 4, 4, 0, 0, 4, 4) == VA_STATUS_SUCCESS && !s->gbm_dirty, "import-write-end-failure-can-recover");
    fail_shared_map = 1;
    expect(va.vaGetImage(&driver, sid, 0, 0, 4, 4, image.image_id) == VA_STATUS_ERROR_OPERATION_FAILED && !sync_active,
        "import-map-failure-still-ends-cpu-access");
    fail_shared_map = 0;
    VAImage derived;
    expect(va.vaDeriveImage(&driver, sid, &derived) == VA_STATUS_ERROR_OPERATION_FAILED, "import-does-not-offer-a-detached-snapshot-as-direct-image");
    struct vav4l2_context *decode = calloc(1, sizeof(*decode));
    if (!decode) exit(1);
    decode->context_id = ++dd->next_context_id; decode->driver_data = dd; decode->codec = VAV4L2_CODEC_H264; decode->entrypoint = VAEntrypointVLD;
    decode->v4l2_fd = dup(device_fd); decode->media_fd = decode->request_fd = -1; decode->capture_bufs_allocd = decode->n_free_cap = 1;
    decode->rt_format = VA_RT_FORMAT_YUV420;
    decode->cap_width = decode->cap_height = decode->cap_stride = 4; decode->cap_pixelformat = V4L2_PIX_FMT_NV12; dd->contexts = decode;
    VADRMPRIMESurfaceDescriptor exported_desc = {.objects = {{.fd = -1}}};
    expect(va.vaExportSurfaceHandle(&driver, sid, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, VA_EXPORT_SURFACE_COMPOSED_LAYERS, &exported_desc) == VA_STATUS_SUCCESS &&
        !foreign_queue && decode->n_free_cap == 1 && s->buf_index == -1, "import-reexport-never-claims-an-unrelated-decoder-capture");
    expect(va.vaBeginPicture(&driver, decode->context_id, sid) == VA_STATUS_SUCCESS && decode->current_surface == s, "imported-nv12-can-be-a-staged-vld-target");
    decode->current_surface = NULL; s->status = VASurfaceReady;
    dd->contexts = NULL; close(decode->v4l2_fd); free(decode);
    VASurfaceID untouched[2] = {1234, 1235}; VADRMPRIMESurfaceDescriptor pair[] = {desc, desc};
    int old_destroys = destroys, old_imports = import_calls; fail_import = import_calls + 2;
    expect(import_surfaces(&driver, pair, 2, untouched) == VA_STATUS_SUCCESS &&
        dd->surfaces[untouched[1]]->prime && !dd->surfaces[untouched[1]]->gbm_bo &&
        import_calls == old_imports + 2 && fcntl(client, F_GETFD) >= 0,
        "rejected-gbm-import-falls-back-to-mappable-prime-storage");
    va.vaDestroySurfaces(&driver, untouched, 2);
    untouched[0] = 1234; untouched[1] = 1235; pair[1].objects[0].fd = -1;
    old_destroys = destroys; fail_import = 0;
    expect(import_surfaces(&driver, pair, 2, untouched) != VA_STATUS_SUCCESS && untouched[0] == 1234 && untouched[1] == 1235 &&
        destroys == old_destroys + 1 && fcntl(client, F_GETFD) >= 0,
        "invalid-second-import-is-atomic-and-releases-earlier-owned-objects");
    fail_import = 0;
    for (unsigned fault = 0; fault < 14; fault++) {
        VADRMPRIMESurfaceDescriptor bad = desc; sid = 1234;
        if (fault == 0) bad.fourcc = VA_FOURCC_NV21;
        if (fault == 1) bad.num_objects = 2;
        if (fault == 2) bad.num_layers = 2;
        if (fault == 3) bad.layers[0].num_planes = 1;
        if (fault == 4) bad.layers[0].drm_format = DRM_FORMAT_NV21;
        if (fault == 5) bad.objects[0].drm_format_modifier = 1;
        if (fault == 6) bad.width = 3;
        if (fault == 7) bad.layers[0].pitch[1] = 3;
        if (fault == 8) bad.layers[0].object_index[1] = 1;
        if (fault == 9) bad.layers[0].offset[0] = 32;
        if (fault == 10) bad.layers[0].offset[1] = 19;
        if (fault == 11) bad.objects[0].size = 47;
        if (fault == 12) bad.objects[0].size = 113;
        if (fault == 13) bad.objects[0].fd = -1;
        expect(import_surfaces(&driver, &bad, 1, &sid) != VA_STATUS_SUCCESS && sid == 1234 && fcntl(client, F_GETFD) >= 0,
            "invalid-import-descriptor-does-not-publish-id-or-consume-client-fd");
    }
    sid = s->surface_id;
    for (unsigned fault = 0; fault < 2; fault++) {
        old_destroys = destroys;
        bad_import_stride = fault == 0; bad_modifier = fault == 1; untouched[0] = 1234;
        expect(import_surfaces(&driver, &desc, 1, untouched) == VA_STATUS_SUCCESS && dd->surfaces[untouched[0]]->prime &&
            destroys == old_destroys + 1 && fcntl(client, F_GETFD) >= 0,
            "incompatible-gbm-layout-uses-validated-linear-prime-fallback");
        va.vaDestroySurfaces(&driver, untouched, 1);
        bad_import_stride = bad_modifier = 0;
    }
    VASurfaceID native;
    expect(va.vaCreateSurfaces(&driver, 4, 4, VA_RT_FORMAT_YUV420, 1, &native) == VA_STATUS_SUCCESS, "import-vpp-native-target");
    struct vav4l2_context *vpp = calloc(1, sizeof(*vpp)); if (!vpp) exit(1);
    vpp->driver_data = dd; vpp->codec = VAV4L2_CODEC_VPP; vpp->entrypoint = VAEntrypointVideoProc;
    vpp->context_id = ++dd->next_context_id; vpp->v4l2_fd = dup(device_fd); vpp->media_fd = vpp->request_fd = -1;
    vpp->current_surface = dd->surfaces[native]; dd->contexts = vpp;
    struct vav4l2_m2m_state *q = &vpp->vpp_q;
    q->valid = 1; q->out_planes = 1; q->out_len[0] = q->cap_len = 24;
    q->out_map[0] = mmap(NULL, 24, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    q->cap_map = mmap(NULL, 24, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (q->out_map[0] == MAP_FAILED || q->cap_map == MAP_FAILED) exit(1);
    q->ofmt.fmt.pix_mp.plane_fmt[0].bytesperline = q->cfmt.fmt.pix_mp.plane_fmt[0].bytesperline = 4;
    uint32_t key[] = {4, 4, VA_FOURCC_NV12, 4, 4, VA_FOURCC_NV12, 0, 0}; memcpy(q->key, key, sizeof(key));
    pattern(data, 5); expect(pwrite(owner, data, sizeof(data), 0) == (ssize_t)sizeof(data), "external-before-vpp-input");
    VAProcPipelineParameterBuffer pipe = {.surface = sid};
    struct vav4l2_buffer params = {.type = VAProcPipelineParameterBufferType, .data = &pipe, .size = sizeof(pipe)};
    struct vav4l2_buffer *pending[] = {&params}; import_queue = q;
    expect(vav4l2_vpp_run(vpp, pending, 1) == VA_STATUS_SUCCESS, "import-is-a-vpp-input");
    struct vav4l2_surface *native_surface = dd->surfaces[native];
    VAImage layout = {.offsets = {0, native_surface->cpu_stride * 4}, .pitches = {native_surface->cpu_stride, native_surface->cpu_stride}};
    expect(native_surface->cpu_ptr && image_matches(&layout, native_surface->cpu_ptr, data), "vpp-input-reads-current-external-planes");
    unsigned char background[112]; pattern(background, 6);
    expect(pwrite(owner, background, sizeof(background), 0) == (ssize_t)sizeof(background), "external-before-vpp-output");
    vpp->current_surface = NULL; pipe.surface = native; VABufferID pipeline;
    expect(va.vaCreateBuffer(&driver, vpp->context_id, VAProcPipelineParameterBufferType, sizeof(pipe), 1, &pipe, &pipeline) == VA_STATUS_SUCCESS &&
        va.vaBeginPicture(&driver, vpp->context_id, sid) == VA_STATUS_SUCCESS && va.vaRenderPicture(&driver, vpp->context_id, &pipeline, 1) == VA_STATUS_SUCCESS &&
        va.vaEndPicture(&driver, vpp->context_id) == VA_STATUS_SUCCESS, "import-is-a-public-vpp-output");
    expect(pread(owner, got, sizeof(got), 0) == (ssize_t)sizeof(got) && !memcmp(got, data, sizeof(data)), "vpp-output-updates-the-same-external-object");
    va.vaDestroyBuffer(&driver, pipeline); va.vaDestroyContext(&driver, vpp->context_id); import_queue = NULL;
    va.vaDestroySurfaces(&driver, &native, 1);
    close(client); client = -1; pattern(data, 4); expect(pwrite(owner, data, sizeof(data), 0) == (ssize_t)sizeof(data), "exporter-remains-live-after-client-fd-close");
    expect(va.vaGetImage(&driver, sid, 0, 0, 4, 4, image.image_id) == VA_STATUS_SUCCESS && image_matches(&image, pixels, data), "import-survives-client-fd-release");
    va.vaUnmapBuffer(&driver, image.buf); va.vaDestroyImage(&driver, image.image_id);
    expect(va.vaDestroySurfaces(&driver, &sid, 1) == VA_STATUS_SUCCESS && fcntl(held, F_GETFD) < 0 && fcntl(owner, F_GETFD) >= 0 &&
        fcntl(exported_desc.objects[0].fd, F_GETFD) >= 0 && sync_reads && sync_writes && !sync_active, "destroy-releases-only-import-owned-resources");
    int padded_fd = memfd_create("equal-pitch-import", MFD_CLOEXEC);
    if (padded_fd < 0 || ftruncate(padded_fd, 512) < 0) expect(0, "equal-pitch-import-storage");
    else {
        unsigned char padding[512]; memset(padding, 0xcd, sizeof(padding));
        expect(pwrite(padded_fd, padding, sizeof(padding), 0) == sizeof(padding), "equal-pitch-import-fill");
        VADRMPRIMESurfaceDescriptor padded = desc; padded.objects[0].fd = padded_fd; padded.objects[0].size = 512;
        padded.layers[0].pitch[0] = padded.layers[0].pitch[1] = 64; padded.layers[0].offset[1] = 256;
        VASurfaceID target = VA_INVALID_ID; VAImage upload; void *bytes;
        if (import_surfaces(&driver, &padded, 1, &target) || va.vaCreateImage(&driver, &fmt, 4, 4, &upload)) expect(0, "equal-pitch-import-image");
        else {
            va.vaMapBuffer(&driver, upload.buf, &bytes); memset(bytes, 0x6a, upload.data_size);
            expect(va.vaPutImage(&driver, target, upload.image_id, 0, 0, 4, 4, 0, 0, 4, 4) == VA_STATUS_SUCCESS &&
                pread(padded_fd, padding, sizeof(padding), 0) == sizeof(padding), "equal-pitch-import-write");
            for (unsigned at = 0; at < sizeof(padding); at++)
                expect(padding[at] == (at < 384 && at % 64 < 4 ? 0x6a : 0xcd), "equal-cpu-and-object-pitch-does-not-overwrite-external-padding");
            va.vaUnmapBuffer(&driver, upload.buf); va.vaDestroyImage(&driver, upload.image_id);
        }
        if (target != VA_INVALID_ID) va.vaDestroySurfaces(&driver, &target, 1);
    }
    if (padded_fd >= 0) close(padded_fd);
    va.vaTerminate(&driver); close(owner); if (exported_desc.objects[0].fd >= 0) close(exported_desc.objects[0].fd); vav4l2_set_ioctl_hook(NULL);
}
static void export_formats(void)
{
    struct VADriverVTable v = {0}; struct VADriverContext d = {.vtable = &v};
    if (VAV4L2_DRIVER_INIT(&d)) { expect(0, "format-export-init"); return; }
    const struct { uint32_t va, drm, rt; } formats[] = {
        {VA_FOURCC_NV12, DRM_FORMAT_NV12, VA_RT_FORMAT_YUV420}, {VA_FOURCC_NV21, DRM_FORMAT_NV21, VA_RT_FORMAT_YUV420},
        {VA_FOURCC_I420, DRM_FORMAT_YUV420, VA_RT_FORMAT_YUV420}, {VA_FOURCC_YV12, DRM_FORMAT_YVU420, VA_RT_FORMAT_YUV420},
        {VA_FOURCC_P010, DRM_FORMAT_P010, VA_RT_FORMAT_YUV420_10}, {VA_FOURCC_YUY2, DRM_FORMAT_YUYV, VA_RT_FORMAT_YUV422},
        {VA_FOURCC_UYVY, DRM_FORMAT_UYVY, VA_RT_FORMAT_YUV422}, {VA_FOURCC_VYUY, DRM_FORMAT_VYUY, VA_RT_FORMAT_YUV422},
        {VA_FOURCC_YVYU, DRM_FORMAT_YVYU, VA_RT_FORMAT_YUV422}, {VA_FOURCC_422H, DRM_FORMAT_YUV422, VA_RT_FORMAT_YUV422},
        {VA_FOURCC_YV16, DRM_FORMAT_YVU422, VA_RT_FORMAT_YUV422}, {VA_FOURCC_Y210, DRM_FORMAT_Y210, VA_RT_FORMAT_YUV422_10},
        {VA_FOURCC_ARGB, DRM_FORMAT_BGRA8888, VA_RT_FORMAT_RGB32}, {VA_FOURCC_BGRA, DRM_FORMAT_ARGB8888, VA_RT_FORMAT_RGB32},
        {VA_FOURCC_BGRX, DRM_FORMAT_XRGB8888, VA_RT_FORMAT_RGB32},
        {VA_FOURCC_RGB565, DRM_FORMAT_RGB565, VA_RT_FORMAT_RGB16},
        {VA_FOURCC_RGBA, DRM_FORMAT_ABGR8888, VA_RT_FORMAT_RGB32},
        {VA_FOURCC_RGBX, DRM_FORMAT_XBGR8888, VA_RT_FORMAT_RGB32}};
    vav4l2_set_ioctl_hook(import_ioctl);
    for (unsigned i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
        VASurfaceAttrib a = {.type = VASurfaceAttribPixelFormat, .value = {.type = VAGenericValueTypeInteger, .value.i = formats[i].va}};
        VASurfaceID sid; VAImage in, out; VAImageFormat f = {.fourcc = formats[i].va}; unsigned char *ip, *op;
        if (v.vaCreateSurfaces2(&d, formats[i].rt, 4, 4, &sid, 1, &a, 1) || v.vaCreateImage(&d, &f, 4, 4, &in) ||
            v.vaCreateImage(&d, &f, 4, 4, &out)) { expect(0, "export-format-allocation"); break; }
        v.vaMapBuffer(&d, in.buf, (void **)&ip); v.vaMapBuffer(&d, out.buf, (void **)&op);
        if (i == 0) {
            VADRMPRIMESurfaceDescriptor initial = {0};
            expect(v.vaExportSurfaceHandle(&d, sid, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, VA_EXPORT_SURFACE_WRITE_ONLY, &initial) == VA_STATUS_SUCCESS,
                "fresh-surface-can-be-exported-for-external-initialization");
            struct vav4l2_surface *fresh = ((struct vav4l2_driver_data *)d.pDriverData)->surfaces[sid];
            struct fake_bo *storage = (struct fake_bo *)fresh->gbm_bo;
            if (storage) memset(storage->pixels, 0x2a, storage->map_size);
            expect(v.vaGetImage(&d, sid, 0, 0, 4, 4, out.image_id) == VA_STATUS_SUCCESS && op[0] == 0x2a,
                "new-external-image-is-readable-without-a-prior-va-upload"); close(initial.objects[0].fd);
            fresh->cap_fourcc = V4L2_PIX_FMT_NV15; fresh->last_writer = VAV4L2_WRITER_MEMFD;
            VADRMPRIMESurfaceDescriptor wrong, untouched; memset(&wrong, 0xa5, sizeof(wrong)); untouched = wrong;
            expect(v.vaExportSurfaceHandle(&d, sid, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, 0, &wrong) == VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT &&
                !memcmp(&wrong, &untouched, sizeof(wrong)), "packed-nv15-capture-is-never-exported-as-nv12-bytes");
            fresh->cap_fourcc = V4L2_PIX_FMT_NV12; fresh->last_writer = VAV4L2_WRITER_CPU;
        }
        if (f.fourcc == VA_FOURCC_P010) {
            struct vav4l2_surface *decoded = ((struct vav4l2_driver_data *)d.pDriverData)->surfaces[sid];
            unsigned char capture[30]; memset(capture, 0x55, sizeof(capture));
            struct vav4l2_context decode = {.driver_data = d.pDriverData, .v4l2_fd = device_fd, .capture_bufs_allocd = 1,
                .capture_buf_ptr = {capture}, .capture_buf_size = 30, .cap_sizeimage = 30, .cap_stride = 5,
                .cap_width = 4, .cap_height = 4, .cap_pixelformat = V4L2_PIX_FMT_NV15};
            VADRMPRIMESurfaceDescriptor first = {0};
            expect(!vav4l2_surface_pull_capture(&decode, decoded, 0) &&
                v.vaExportSurfaceHandle(&d, sid, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, VA_EXPORT_SURFACE_READ_WRITE, &first) == VA_STATUS_SUCCESS &&
                v.vaGetImage(&d, sid, 0, 0, 4, 4, out.image_id) == VA_STATUS_SUCCESS && op[0] == 0x40 &&
                vav4l2_validate_reference(&decode, sid) == VA_STATUS_SUCCESS, "first-p010-export-and-readback-retain-native-dpb-reference");
            close(first.objects[0].fd); decoded->buf_index = -1; decoded->cap_view = NULL; decoded->capture_ctx = NULL;
        }
        for (unsigned n = 0; n < in.data_size; n++) ip[n] = n * 17 + i;
        expect(v.vaPutImage(&d, sid, in.image_id, 0, 0, 4, 4, 0, 0, 4, 4) == VA_STATUS_SUCCESS, "export-format-upload");
        struct vav4l2_prime_layout l; expect(!vav4l2_prime_layout(f.fourcc, 4, 4, &l), "export-format-layout");
        VADRMPRIMESurfaceDescriptor e = {0};
        expect(v.vaExportSurfaceHandle(&d, sid, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
            VA_EXPORT_SURFACE_READ_WRITE | VA_EXPORT_SURFACE_COMPOSED_LAYERS, &e) == VA_STATUS_SUCCESS &&
            e.fourcc == f.fourcc && e.layers[0].drm_format == formats[i].drm && e.layers[0].num_planes == in.num_planes,
            "all-fifteen-native-exports-use-the-correct-drm-format");
        if (f.fourcc == VA_FOURCC_422H || f.fourcc == VA_FOURCC_YV16)
            expect(!(e.layers[0].pitch[1] & 63) && !(e.layers[0].pitch[2] & 63), "planar422-native-export-pads-each-chroma-row-to-sixty-four-bytes");
        struct vav4l2_surface *s = ((struct vav4l2_driver_data *)d.pDriverData)->surfaces[sid];
        struct fake_bo *bo = (struct fake_bo *)s->gbm_bo;
        for (unsigned p = 0; p < in.num_planes; p++) for (unsigned y = 0; y < l.rows[p]; y++)
            expect(!memcmp(bo->pixels + e.layers[0].offset[p] + (size_t)y * e.layers[0].pitch[p],
                ip + in.offsets[p] + (size_t)y * in.pitches[p], l.row[p]), "native-export-has-original-plane-bytes");
        if (f.fourcc == VA_FOURCC_YV12 || f.fourcc == VA_FOURCC_YV16) {
            expect(e.layers[0].offset[2] < e.layers[0].offset[1], "native-YVU-descriptor-preserves-logical-order-over-YUV-storage");
            for (unsigned p = 1; p < 3; p++) for (unsigned y = 0; y < l.rows[p]; y++)
                memset(bo->pixels + e.layers[0].offset[p] + y * e.layers[0].pitch[p], p == 1 ? 0xd2 : 0x44, l.row[p]);
            s->last_writer = VAV4L2_WRITER_BO;
            expect(v.vaGetImage(&d, sid, 0, 0, 4, 4, out.image_id) == VA_STATUS_SUCCESS && op[out.offsets[1]] == 0xd2 && op[out.offsets[2]] == 0x44,
                "native-YVU-CPU-readback-uses-public-Cr-Cb-order");
            VADRMPRIMESurfaceDescriptor separated = {0};
            expect(vav4l2_surface_fill_prime_gbm(s, VA_EXPORT_SURFACE_SEPARATE_LAYERS, &separated) == VA_STATUS_SUCCESS &&
                separated.num_layers == 3 && separated.layers[1].offset[0] == e.layers[0].offset[1] && separated.layers[2].offset[0] == e.layers[0].offset[2],
                "separate-YVU-layers-retain-the-same-logical-offsets"); close(separated.objects[0].fd);
        }
        memset(stale_pixels, 0x19, sizeof(stale_pixels)); stale_gbm_read = 1;
        memset(bo->pixels, 0x76, bo->map_size);
        s->last_writer = VAV4L2_WRITER_BO;
        int before_maps = maps;
        expect(vav4l2_gbm_surface_sync(s) == 0 && maps == before_maps && bo->pixels[0] == 0x76,
            "device-written-bo-is-never-overwritten-from-a-stale-cpu-snapshot");
        if (f.fourcc == VA_FOURCC_NV12) expect(vav4l2_gbm_read_cpu(s) == 0 && ((unsigned char *)s->cpu_ptr)[0] == 0x76,
            "narrow-nv12-bo-readback-maps-visible-byte-width-not-padded-cpu-stride");
        expect(v.vaGetImage(&d, sid, 0, 0, 4, 4, out.image_id) == VA_STATUS_SUCCESS, "writable-export-is-refreshed-before-va-read");
        for (unsigned p = 0; p < out.num_planes; p++) for (unsigned y = 0; y < l.rows[p]; y++) for (unsigned x = 0; x < l.row[p]; x++)
            expect(op[out.offsets[p] + (size_t)y * out.pitches[p] + x] == 0x76, "external-write-is-visible-in-every-format");
        stale_gbm_read = 0;
        int before_exports = exported;
        VADRMPRIMESurfaceDescriptor shared = {0};
        expect(vav4l2_surface_fill_prime_gbm(s, VA_EXPORT_SURFACE_COMPOSED_LAYERS, &shared) == VA_STATUS_SUCCESS &&
            exported == before_exports && (fcntl(shared.objects[0].fd, F_GETFD) & FD_CLOEXEC),
            "shared-native-storage-is-duplicated-without-reexporting-a-gbm-transfer");
        struct stat original, duplicate;
        expect(fstat(e.objects[0].fd, &original) == 0 && fstat(shared.objects[0].fd, &duplicate) == 0 &&
            original.st_dev == duplicate.st_dev && original.st_ino == duplicate.st_ino,
            "shared-descriptor-keeps-the-published-allocation-identity");
        if (shared.num_objects) close(shared.objects[0].fd);
        if (f.fourcc == VA_FOURCC_P010 || f.fourcc == VA_FOURCC_Y210 || f.fourcc == VA_FOURCC_YUY2) {
            unsigned char capture[64]; memset(capture, 0x55, sizeof(capture));
            uint32_t cf = f.fourcc == VA_FOURCC_P010 ? V4L2_PIX_FMT_NV15 : f.fourcc == VA_FOURCC_Y210 ? V4L2_PIX_FMT_NV20 : V4L2_PIX_FMT_NV16;
            unsigned stride = cf == V4L2_PIX_FMT_NV16 ? 4 : 5, size = vav4l2_capture_plane_size(cf, stride, 4);
            struct vav4l2_context decode = {.driver_data = d.pDriverData, .v4l2_fd = device_fd, .capture_bufs_allocd = 1,
                .capture_buf_ptr = {capture}, .capture_buf_size = size, .cap_sizeimage = size, .cap_stride = stride,
                .cap_width = 4, .cap_height = 4, .cap_pixelformat = cf};
            expect(vav4l2_surface_pull_capture(&decode, s, 0) == 0 && bo->pixels[0] == (cf == V4L2_PIX_FMT_NV16 ? 0x55 : 0x40),
                "retained-non-nv12-export-is-updated-after-decoding");
            expect(v.vaGetImage(&d, sid, 0, 0, 4, 4, out.image_id) == VA_STATUS_SUCCESS &&
                op[0] == (cf == V4L2_PIX_FMT_NV16 ? 0x55 : 0x40) && vav4l2_validate_reference(&decode, sid) == VA_STATUS_SUCCESS,
                "non-nv12-display-read-keeps-native-dpb-reference");
            s->buf_index = -1; s->cap_view = NULL; s->capture_ctx = NULL;
        }
        VADRMPRIMESurfaceDescriptor separate = {0};
        expect(v.vaExportSurfaceHandle(&d, sid, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, VA_EXPORT_SURFACE_SEPARATE_LAYERS, &separate) == VA_STATUS_SUCCESS &&
            separate.num_layers == l.planes, "all-formats-separate-layer-export");
        close(separate.objects[0].fd); v.vaUnmapBuffer(&d, in.buf); v.vaUnmapBuffer(&d, out.buf);
        v.vaDestroyImage(&d, in.image_id); v.vaDestroyImage(&d, out.image_id); v.vaDestroySurfaces(&d, &sid, 1);
        expect(fcntl(e.objects[0].fd, F_GETFD) >= 0, "new-format-client-export-outlives-surface"); close(e.objects[0].fd);
    }
    v.vaTerminate(&d); vav4l2_set_ioctl_hook(NULL);
}
static void shared_allocator_checks(void)
{
    struct vav4l2_surface s = {.width = 4, .height = 4, .format = VA_FOURCC_BGRX, .memfd_fd = -1,
        .import_fd = -1, .gbm_external_fd = -1, .cpu_stride = 64, .cpu_size = 256, .last_writer = VAV4L2_WRITER_CPU};
    if (vav4l2_surface_ensure_cpu(&s)) { expect(0, "shared-native-cpu-fixture"); return; }
    memset(s.cpu_ptr, 0x42, s.cpu_size); vav4l2_set_ioctl_hook(import_ioctl); use_shared_storage = 1;
    expect(vav4l2_gbm_surface_ensure(&s) == 0 && s.gbm_writable && s.gbm_external_fd >= 0 && !s.gbm_imported,
        "native-surface-imports-owned-coherent-storage-before-first-upload");
    int held = s.gbm_external_fd, retained = held >= 0 ? dup(held) : -1;
    unsigned char bytes[256]; memset(bytes, 0x69, sizeof(bytes));
    expect(held >= 0 && pwrite(held, bytes, sizeof(bytes), 0) == sizeof(bytes), "external-writer-updates-native-coherent-object");
    stale_gbm_read = 1; memset(stale_pixels, 0x19, sizeof(stale_pixels));
    expect(vav4l2_gbm_read_cpu(&s) == 0 && ((unsigned char *)s.cpu_ptr)[0] == 0x69 && !sync_active,
        "native-coherent-readback-never-uses-a-stale-gbm-transfer");
    stale_gbm_read = 0; use_shared_storage = 0;
    vav4l2_gbm_surface_destroy(&s); free(s.cpu_ptr);
    expect(fcntl(held, F_GETFD) < 0 && retained >= 0 && fcntl(retained, F_GETFD) >= 0,
        "native-allocation-cleanup-closes-owned-fd-and-retains-client-duplicates");
    if (retained >= 0) close(retained);
    use_shared_storage = 1; fail_import = import_calls + 1; coherent_calls = 0;
    struct vav4l2_surface retry = {.width = 4, .height = 4, .format = VA_FOURCC_NV12, .memfd_fd = -1, .gbm_external_fd = -1};
    expect(!vav4l2_gbm_surface_ensure(&retry) && coherent_calls == 1 && retry.gbm_writable && retry.gbm_external_fd == last_coherent,
        "rejected-prepublication-import-tries-independent-coherent-storage");
    held = retry.gbm_external_fd; retained = dup(held); vav4l2_gbm_surface_destroy(&retry);
    expect(fcntl(held, F_GETFD) < 0 && retained >= 0 && fcntl(retained, F_GETFD) >= 0, "coherent-retry-preserves-client-FD-ownership");
    if (retained >= 0) close(retained);
    fail_import = import_calls + 1; coherent_calls = 0; coherent_fail = 1;
    expect(!vav4l2_gbm_surface_ensure(&retry) && coherent_calls == 1 && !retry.gbm_writable, "all-shared-providers-rejected-keep-native-fallback");
    vav4l2_gbm_surface_destroy(&retry); coherent_fail = 0;
    setenv("VAV4L2_SHARED_STORAGE", "coherent", 1); fail_import = import_calls + 1; coherent_calls = 0;
    expect(!vav4l2_gbm_surface_ensure(&retry) && coherent_calls == 1 && !retry.gbm_writable, "explicit-coherent-policy-does-not-retry-same-provider");
    vav4l2_gbm_surface_destroy(&retry); unsetenv("VAV4L2_SHARED_STORAGE"); use_shared_storage = fail_import = 0;
    vav4l2_set_ioctl_hook(NULL);
}
static int fresh_output_fd = -1, fresh_output_syncs, fresh_input_fd = -1;
static int fresh_output_ioctl(int fd, unsigned long request, void *argument)
{
    (void)fd;
    if (request == VIDIOC_EXPBUF) {
        ((struct v4l2_exportbuffer *)argument)->fd = fcntl(fresh_input_fd, F_DUPFD_CLOEXEC, 0); return 0;
    }
    if (request == DMA_BUF_IOCTL_SYNC) {
        expect((((struct dma_buf_sync *)argument)->flags & DMA_BUF_SYNC_RW) == DMA_BUF_SYNC_READ, "fresh-vpp-only-synchronizes-initial-bo-export");
        fresh_output_syncs++; return 0;
    }
    if (request == VIDIOC_S_FMT) {
        struct v4l2_format *f = argument;
        unsigned width = f->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE ? 8 : 4;
        expect(f->fmt.pix_mp.width == width && f->fmt.pix_mp.height == width && f->fmt.pix_mp.pixelformat == V4L2_PIX_FMT_NV12, "native-vpp-negotiates-distinct-input-and-output-sizes");
        f->fmt.pix_mp.plane_fmt[0].sizeimage = f->fmt.pix_mp.plane_fmt[0].bytesperline * (f->fmt.pix_mp.height + f->fmt.pix_mp.height / 2);
    }
    if (request == VIDIOC_QBUF) {
        struct v4l2_buffer *b = argument;
        expect(b->memory == V4L2_MEMORY_DMABUF, "fresh-output-uses-dma-queues");
        if (b->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) fresh_output_fd = b->m.planes[0].m.fd;
        else {
            unsigned char bytes[96];
            expect(b->m.planes[0].bytesused == sizeof(bytes) && pread(b->m.planes[0].m.fd, bytes, sizeof(bytes), 0) == sizeof(bytes) &&
                pwrite(fresh_output_fd, bytes, 24, 0) == 24, "uniform-frame-downscale-through-device-queues");
        }
    }
    if (request == VIDIOC_DQBUF) ((struct v4l2_buffer *)argument)->m.planes[0].bytesused = 24;
    return 0;
}
static void fresh_output_checks(void)
{
    struct vav4l2_driver_data dd = {.vpp_devices_scanned = 1, .num_vpp_devices = 1, .gbm = {.fd = -1, .drm_fd = -1}};
    struct vav4l2_vpp_device *node = &dd.vpp_devices[0];
    strcpy(node->path, "/dev/null"); strcpy(node->identity.driver, "fixture-vpp");
    node->input[0] = node->output[0] = V4L2_PIX_FMT_NV12; node->num_input = node->num_output = 1;
    struct vav4l2_surface src = {.driver_data = &dd, .surface_id = 1, .width = 4, .height = 4, .format = VA_FOURCC_NV12, .buf_index = -1};
    struct vav4l2_surface dst = src; dst.surface_id = 2; dd.surfaces[1] = &src; src.width = src.height = 8;
    fresh_input_fd = memfd_create("native-vpp-input", MFD_CLOEXEC);
    unsigned char pixels[96]; memset(pixels, 0x59, sizeof(pixels));
    if (fresh_input_fd < 0 || ftruncate(fresh_input_fd, 4096) || pwrite(fresh_input_fd, pixels, sizeof(pixels), 0) != sizeof(pixels)) { expect(0, "fresh-vpp-input-fixture"); return; }
    struct vav4l2_context decoder = {.v4l2_fd = fresh_input_fd, .capture_bufs_allocd = 1, .cap_sizeimage = 96};
    src.has_pic = 1; src.last_writer = VAV4L2_WRITER_MEMFD; src.buf_index = 0; src.stride = 8; src.aligned_h = 8;
    src.cap_fourcc = V4L2_PIX_FMT_NV12; src.capture_ctx = &decoder;
    struct vav4l2_context ctx = {.driver_data = &dd, .current_surface = &dst, .device_path = strdup(node->path), .v4l2_fd = open(node->path, O_RDWR | O_CLOEXEC)};
    VAProcPipelineParameterBuffer pipe = {.surface = 1};
    struct vav4l2_buffer parameter = {.type = VAProcPipelineParameterBufferType, .size = sizeof(pipe), .data = &pipe};
    struct vav4l2_buffer *buffers[] = {&parameter}; int previous_maps = maps; fresh_output_syncs = 0;
    setenv("VAV4L2_VPP_DMABUF", "required", 1); vav4l2_set_ioctl_hook(fresh_output_ioctl);
    expect(vav4l2_vpp_run(&ctx, buffers, 1) == VA_STATUS_SUCCESS && dst.gbm_bo && dst.has_pic && !dst.cpu_ptr && maps == previous_maps && fresh_output_syncs == 2,
        "fresh-vpp-output-allocates-without-upload-or-readback");
    if (dst.gbm_bo) expect(!memcmp(((struct fake_bo *)dst.gbm_bo)->pixels, pixels, 24), "fresh-vpp-output-pixels");
    for (unsigned frame = 0; frame < 3; frame++) {
        memset(pixels, 0x70 + frame, sizeof(pixels));
        expect(pwrite(fresh_input_fd, pixels, sizeof(pixels), 0) == sizeof(pixels) && vav4l2_vpp_run(&ctx, buffers, 1) == VA_STATUS_SUCCESS &&
            dst.gbm_bo && !memcmp(((struct fake_bo *)dst.gbm_bo)->pixels, pixels, 24) && !src.cpu_ptr && !dst.cpu_ptr && maps == previous_maps,
            "native-vpp-reuses-output-backing-and-transfers-each-current-frame");
    }
    vav4l2_m2m_teardown(&ctx, &ctx.vpp_q); close(ctx.v4l2_fd); free(ctx.device_path);
    vav4l2_gbm_surface_destroy(&dst);
    dst = (struct vav4l2_surface){.driver_data = &dd, .surface_id = 2, .width = 4, .height = 4, .format = VA_FOURCC_NV12, .buf_index = -1};
    src = dst; src.surface_id = 1; ctx.v4l2_fd = open(node->path, O_RDWR | O_CLOEXEC); ctx.device_path = strdup(node->path);
    VADRMPRIMESurfaceDescriptor desc = {.fourcc = VA_FOURCC_NV12, .width = 4, .height = 4, .num_objects = 1, .num_layers = 1};
    desc.objects[0].fd = fresh_input_fd; desc.objects[0].size = 4096; desc.layers[0].drm_format = DRM_FORMAT_NV12;
    desc.layers[0].num_planes = 2; desc.layers[0].pitch[0] = desc.layers[0].pitch[1] = 4; desc.layers[0].offset[1] = 16;
    expect(vav4l2_prime_import(&src, &desc) == VA_STATUS_SUCCESS, "imported-vpp-input-fixture");
    expect(vav4l2_vpp_run(&ctx, buffers, 1) != VA_STATUS_SUCCESS && !dst.gbm_bo && !dst.has_pic,
        "imported-input-does-not-promote-a-fresh-output-to-direct-transport");
    close(ctx.v4l2_fd); free(ctx.device_path); vav4l2_prime_destroy(&src); vav4l2_gbm_driver_cleanup(&dd); close(fresh_input_fd); fresh_input_fd = -1;
    vav4l2_set_ioctl_hook(NULL); unsetenv("VAV4L2_VPP_DMABUF");
}
int main(int argc, char **argv)
{
    display_device_checks();
    destroys = device_destroys = 0;
    struct VADriverVTable va = {0}, vb = {0};
    struct VADriverContext a = {.vtable = &va}, b = {.vtable = &vb};
    setenv("VAV4L2_PROBE_NOCACHE", "1", 1); setenv("VAV4L2_RENDER_NODE", "/dev/null", 1);
    setenv("VAV4L2_SHARED_STORAGE", "auto", 1);
    setenv("VAV4L2_EXPBUF_EXPORT", argc > 1 ? "1" : "0", 1);
    if (VAV4L2_DRIVER_INIT(&a) || VAV4L2_DRIVER_INIT(&b)) return 1;
    VASurfaceID sa, sb;
    if (va.vaCreateSurfaces(&a, 3, 3, VA_RT_FORMAT_YUV420, 1, &sa) ||
        vb.vaCreateSurfaces(&b, 3, 3, VA_RT_FORMAT_YUV420, 1, &sb)) return 1;
    struct vav4l2_surface *s = ((struct vav4l2_driver_data *)a.pDriverData)->surfaces[sa];
    struct vav4l2_surface *other = ((struct vav4l2_driver_data *)b.pDriverData)->surfaces[sb];
    if (vav4l2_surface_ensure_cpu(s)) return 1;
    memset(s->cpu_ptr, 0x77, s->cpu_size); s->has_pic = 1; s->last_writer = VAV4L2_WRITER_CPU;
    fail_map = 1;
    expect(vav4l2_gbm_surface_ensure(s) < 0 && !s->gbm_bo && destroys == 1, "initial-upload-failure-releases-new-bo");
    fail_map = 0;
    expect(vav4l2_gbm_surface_ensure(s) == 0 && s->gbm_bo, "initial-upload-can-retry");
    struct fake_bo *bo = (struct fake_bo *)s->gbm_bo;
    if (!bo) return 1;
    expect(bo->width == 4 && bo->height == 5 && bo->pixels[0] == 0x77 && bo->pixels[12] == 0x77 &&
           bo->pixels[19] == 0x77, "odd-nv12-width-allocates-complete-chroma-pairs");
    ((unsigned char *)s->cpu_ptr)[0] = 0x66; fail_map = 1;
    expect(vav4l2_gbm_surface_upload(s, s->cpu_ptr, s->cpu_stride, s->height) < 0 && s->gbm_dirty,
           "failed-refresh-marks-existing-bo-dirty");
    VADRMPRIMESurfaceDescriptor desc, unchanged; memset(&desc, 0xa5, sizeof(desc)); unchanged = desc;
    int old_exported = exported;
    expect(va.vaExportSurfaceHandle(&a, sa, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, 0, &desc) == VA_STATUS_ERROR_OPERATION_FAILED &&
           exported == old_exported && !memcmp(&desc, &unchanged, sizeof(desc)), "dirty-failed-export-never-returns-stale-pixels-or-fd");
    fail_map = 0;
    VAStatus st = va.vaExportSurfaceHandle(&a, sa, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, 0, &desc);
    expect(st == VA_STATUS_SUCCESS && !s->gbm_dirty && bo->pixels[0] == 0x66, "export-retries-current-cpu-content");
    int retained = st == VA_STATUS_SUCCESS ? desc.objects[0].fd : -1;
    expect(st == VA_STATUS_SUCCESS && desc.objects[0].size == (size_t)bo->pitch * bo->height + 64 &&
        (fcntl(retained, F_GETFD) & FD_CLOEXEC), "gbm-export-reports-complete-object-and-cloexec");
    bad_modifier = 1; old_exported = exported;
    expect(va.vaExportSurfaceHandle(&a, sa, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, 0, &unchanged) == VA_STATUS_ERROR_OPERATION_FAILED &&
        exported == old_exported, "gbm-export-rejects-unimplemented-modifier-before-fd-transfer");
    bad_modifier = 0; short_export = 1; unchanged = desc;
    expect(va.vaExportSurfaceHandle(&a, sa, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, 0, &unchanged) == VA_STATUS_ERROR_OPERATION_FAILED &&
        !memcmp(&desc, &unchanged, sizeof(desc)) && fcntl(last_export, F_GETFD) < 0, "gbm-short-object-failure-closes-only-new-fd-and-keeps-descriptor");
    short_export = 0;
    int old_maps = maps;
    expect(vav4l2_gbm_surface_ensure(s) == 0 && maps == old_maps, "clean-export-keeps-copy-free-fast-path");
    VAImage image; VAImageFormat format = {.fourcc = VA_FOURCC_NV12};
    if (va.vaCreateImage(&a, &format, 3, 3, &image)) return 1;
    void *pixels; if (va.vaMapBuffer(&a, image.buf, &pixels)) return 1;
    memset(pixels, 0x55, image.data_size); va.vaUnmapBuffer(&a, image.buf);
    fail_map = 1;
    expect(va.vaPutImage(&a, sa, image.image_id, 0, 0, 3, 3, 0, 0, 3, 3) == VA_STATUS_ERROR_OPERATION_FAILED &&
           s->gbm_dirty, "put-image-propagates-display-upload-failure");
    fail_map = 0; expect(vav4l2_gbm_surface_ensure(s) == 0 && bo->pixels[0] == 0x55, "put-image-content-remains-retryable");
    unsigned char capture[32] = {0};
    struct vav4l2_context decode = {.v4l2_fd = dup(device_fd), .capture_bufs_allocd = 1,
        .capture_buf_ptr = {capture}, .cap_stride = 4, .cap_height = 4, .cap_pixelformat = V4L2_PIX_FMT_NV12,
        .capture_buf_size = 24, .cap_sizeimage = 24};
    fail_map = 1;
    expect(vav4l2_surface_pull_capture(&decode, s, 0) < 0 && s->buf_index == -1 && !s->capture_ctx,
           "decoded-frame-cannot-succeed-with-an-unchanged-exported-display-bo");
    fail_map = 0; decode.cap_pixelformat = V4L2_PIX_FMT_NV16; decode.capture_buf_size = decode.cap_sizeimage = 32;
    expect(vav4l2_surface_pull_capture(&decode, s, 0) < 0 && s->gbm_dirty && bo->pixels[0] == 0x55,
        "non-nv12-capture-is-not-reinterpreted-as-an-existing-nv12-display-object");
    decode.cap_pixelformat = V4L2_PIX_FMT_NV12; decode.capture_buf_size = decode.cap_sizeimage = 24;
    memset(capture, 0x3c, sizeof(capture));
    expect(vav4l2_surface_pull_capture(&decode, s, 0) == 0 && !s->gbm_dirty && bo->pixels[0] == 0x3c,
        "decoded-frame-updates-existing-display-bo-even-with-native-expbuf-enabled");
    unsigned char retained_pixel = 0;
    expect(retained >= 0 && pread(retained, &retained_pixel, 1, 0) == 1 && retained_pixel == 0x3c,
        "previous-client-fd-observes-new-decoded-pixels-without-reexport");
    s->buf_index = -1; s->cap_view = NULL; s->capture_ctx = NULL;
    close(decode.v4l2_fd);
    VASurfaceID vpp_ids[2];
    if (vb.vaCreateSurfaces(&b, 4, 4, VA_RT_FORMAT_YUV420, 2, vpp_ids)) return 1;
    struct vav4l2_driver_data *dd = b.pDriverData;
    struct vav4l2_surface *src = dd->surfaces[vpp_ids[0]], *dst = dd->surfaces[vpp_ids[1]];
    if (vav4l2_surface_ensure_cpu(src) || vav4l2_gbm_surface_ensure(dst)) return 1;
    src->has_pic = 1; src->last_writer = VAV4L2_WRITER_CPU;
    struct vav4l2_context vpp = {.driver_data = dd, .current_surface = dst, .v4l2_fd = dup(device_fd)};
    struct vav4l2_m2m_state *q = &vpp.vpp_q;
    q->valid = 1; q->out_planes = 1; q->out_len[0] = q->cap_len = 24;
    q->out_map[0] = mmap(NULL, 24, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    q->cap_map = mmap(NULL, 24, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (q->out_map[0] == MAP_FAILED || q->cap_map == MAP_FAILED) return 1;
    memset(q->cap_map, 0x91, 24);
    q->ofmt.fmt.pix_mp.plane_fmt[0].bytesperline = q->cfmt.fmt.pix_mp.plane_fmt[0].bytesperline = 4;
    uint32_t key[] = {4, 4, VA_FOURCC_NV12, 4, 4, VA_FOURCC_NV12, 0, 0}; memcpy(q->key, key, sizeof(key));
    VAProcPipelineParameterBuffer pipe = {.surface = vpp_ids[0]};
    struct vav4l2_buffer params = {.type = VAProcPipelineParameterBufferType, .size = sizeof(pipe), .data = &pipe};
    struct vav4l2_buffer *pending[] = {&params}; fail_map = 1; vav4l2_set_ioctl_hook(queue_ioctl);
    expect(vav4l2_vpp_run(&vpp, pending, 1) == VA_STATUS_ERROR_OPERATION_FAILED && !q->valid && dst->gbm_dirty,
           "vpp-propagates-display-upload-failure-and-releases-queues");
    vav4l2_m2m_teardown(&vpp, q); vav4l2_set_ioctl_hook(NULL); close(vpp.v4l2_fd); fail_map = 0;
    va.vaDestroyImage(&a, image.image_id);
    expect(vav4l2_gbm_surface_upload(NULL, pixels, 4, 3) < 0 &&
           vav4l2_gbm_surface_upload(s, s->cpu_ptr, 2, 3) < 0 &&
           vav4l2_gbm_surface_upload(s, s->cpu_ptr, s->cpu_stride, 2) < 0, "upload-rejects-null-and-short-plane-geometry");
    expect(vav4l2_gbm_surface_ensure(other) == 0, "second-driver-owns-independent-backend");
    int fd = device_fd;
    int first_fd = ((struct vav4l2_driver_data *)a.pDriverData)->gbm.fd;
    va.vaTerminate(&a);
    retained_pixel = 0;
    expect(retained >= 0 && pread(retained, &retained_pixel, 1, 0) == 1 && retained_pixel == 0x3c,
        "client-fd-survives-surface-and-driver-destruction");
    if (retained >= 0) close(retained);
    expect(device_destroys == 1 && fcntl(first_fd, F_GETFD) < 0 && fcntl(fd, F_GETFD) >= 0 && other->gbm_bo, "first-terminate-releases-own-backend-and-keeps-other-display");
    vb.vaTerminate(&b);
    expect(device_destroys == 2 && fcntl(fd, F_GETFD) < 0, "last-terminate-releases-backend-and-render-fd");
    struct VADriverVTable vc = {0}; struct VADriverContext c = {.vtable = &vc};
    if (VAV4L2_DRIVER_INIT(&c)) return 1;
    VASurfaceID sc; if (vc.vaCreateSurfaces(&c, 4, 4, VA_RT_FORMAT_YUV420, 1, &sc)) return 1;
    expect(vav4l2_gbm_surface_ensure(((struct vav4l2_driver_data *)c.pDriverData)->surfaces[sc]) == 0,
           "backend-can-initialize-again-after-last-terminate");
    vc.vaTerminate(&c); expect(device_destroys == 3, "reinitialized-backend-is-also-released");
    fresh_output_checks();
    import_checks();
    shared_allocator_checks();
    export_formats();
    if (argc == 1) {
        pid_t child = fork(); int status = 0;
        if (child == 0) { execl(argv[0], argv[0], "--expbuf", (char *)NULL); _exit(127); }
        expect(child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status),
            "gbm-coherence-also-passes-with-native-expbuf-enabled");
    }
    return failures != 0;
}
