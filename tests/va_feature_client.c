/* Hardware feature checks through the public libva API; no internal driver symbols. */
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver_identity.h"
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/dma-buf.h>
#include <gbm.h>
#include <drm_fourcc.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#include <va/va_vpp.h>
#include <va/va_enc_jpeg.h>
static VADisplay display;
static FILE *results;
static const char *report_path;
static unsigned passed, failed;
static char detail[192];
static int status(VAStatus st, const char *operation)
{
    if (st != VA_STATUS_SUCCESS) {
        snprintf(detail, sizeof(detail), "%s: %s (%d)", operation, vaErrorStr(st), st);
        fprintf(stderr, "%s\n", detail);
    }
    return st == VA_STATUS_SUCCESS;
}
static void result(const char *name, int ok)
{
    if (ok) passed++; else failed++;
    fprintf(results, "%s\t%s\t%s\n", ok ? "PASS" : "FAIL", name, ok || !detail[0] ? "-" : detail); fflush(results);
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    detail[0] = 0;
}
static int image(unsigned fourcc, unsigned w, unsigned h, VAImage *out)
{
    int n = vaMaxNumImageFormats(display);
    if (n <= 0) return 0;
    VAImageFormat *formats = calloc((unsigned)n, sizeof(*formats));
    if (!formats) return 0;
    VAStatus st = vaQueryImageFormats(display, formats, &n);
    int ok = 0;
    if (status(st, "vaQueryImageFormats")) {
        for (int i = 0; i < n; i++) if (formats[i].fourcc == fourcc) {
            ok = status(vaCreateImage(display, &formats[i], w, h, out), "vaCreateImage"); break;
        }
    }
    free(formats); return ok;
}
static unsigned bpp(unsigned fcc)
{
    return fcc == VA_FOURCC_P010 || fcc == VA_FOURCC_YUY2 || fcc == VA_FOURCC_UYVY || fcc == VA_FOURCC_VYUY || fcc == VA_FOURCC_YVYU ? 2 :
        fcc == VA_FOURCC_Y210 || fcc == VA_FOURCC_BGRX || fcc == VA_FOURCC_BGRA || fcc == VA_FOURCC_ARGB ? 4 : 1;
}
static int surface_size(unsigned rt, unsigned fcc, unsigned width, unsigned height, VASurfaceID *sid)
{
    VASurfaceAttrib a = {.type = VASurfaceAttribPixelFormat, .flags = VA_SURFACE_ATTRIB_SETTABLE,
        .value = {.type = VAGenericValueTypeInteger, .value.i = fcc}};
    return status(vaCreateSurfaces(display, rt, width, height, sid, 1, &a, 1), "vaCreateSurfaces");
}
static int surface(unsigned rt, unsigned fcc, VASurfaceID *sid)
{
    return surface_size(rt, fcc, 64, 64, sid);
}
static int transfer(unsigned rt, unsigned fcc)
{
    VASurfaceID sid = VA_INVALID_ID;
    VAImage in = {.image_id = VA_INVALID_ID}, out = {.image_id = VA_INVALID_ID};
    uint8_t *mapped = NULL, *expected = NULL;
    int ok = 0;
    if (!surface(rt, fcc, &sid) || !image(fcc, 64, 64, &in) || !image(fcc, 32, 32, &out)) goto done;
    if (!status(vaMapBuffer(display, in.buf, (void **)&mapped), "vaMapBuffer upload")) goto done;
    for (unsigned i = 0; i < in.data_size; i++) mapped[i] = (i * 17 + 37) % 251;
    expected = malloc(in.data_size);
    if (expected) memcpy(expected, mapped, in.data_size);
    if (!status(vaUnmapBuffer(display, in.buf), "vaUnmapBuffer upload") || !expected) goto done;
    if (!status(vaPutImage(display, sid, in.image_id, 0, 0, 64, 64, 0, 0, 64, 64), "vaPutImage") ||
        !status(vaGetImage(display, sid, 16, 16, 32, 32, out.image_id), "vaGetImage crop") ||
        !status(vaMapBuffer(display, out.buf, (void **)&mapped), "vaMapBuffer readback")) goto done;
    ok = 1;
    for (unsigned p = 0; p < out.num_planes; p++) {
        unsigned row_bytes = p && out.num_planes == 3 ? 16 : 32 * bpp(fcc);
        unsigned full_chroma = fcc == VA_FOURCC_422H || fcc == VA_FOURCC_YV16;
        unsigned sx = p && out.num_planes == 3 ? 8 : 16 * bpp(fcc), sy = p && !full_chroma ? 8 : 16;
        unsigned rows = p && !full_chroma ? 16 : 32;
        for (unsigned y = 0; y < rows; y++)
            if (memcmp(expected + in.offsets[p] + (size_t)(sy + y) * in.pitches[p] + sx,
                       mapped + out.offsets[p] + (size_t)y * out.pitches[p], row_bytes)) ok = 0;
    }
    if (!status(vaUnmapBuffer(display, out.buf), "vaUnmapBuffer readback")) ok = 0;
done:
    free(expected);
    if (in.image_id != VA_INVALID_ID) vaDestroyImage(display, in.image_id);
    if (out.image_id != VA_INVALID_ID) vaDestroyImage(display, out.image_id);
    if (sid != VA_INVALID_ID) vaDestroySurfaces(display, &sid, 1);
    return ok;
}
#define VPP_SIZE 128
static uint8_t sample(unsigned plane, unsigned x, unsigned y)
{
    return plane == 0 ? 16 + (x + y * 3) % 220 : plane == 1 ? 64 + x % 32 : 128 + y % 32;
}
static int upload_nv12(VASurfaceID sid, VAImage *img, int background)
{
    uint8_t *p;
    if (!status(vaMapBuffer(display, img->buf, (void **)&p), "map NV12 source")) return 0;
    for (unsigned y = 0; y < img->height; y++) for (unsigned x = 0; x < img->width; x++)
        p[img->offsets[0] + (size_t)y * img->pitches[0] + x] = background ? 0x55 : sample(0, x, y);
    for (unsigned y = 0; y < img->height / 2; y++) for (unsigned x = 0; x < img->width / 2; x++) {
        p[img->offsets[1] + (size_t)y * img->pitches[1] + x * 2] = background ? 0x66 : sample(1, x, y);
        p[img->offsets[1] + (size_t)y * img->pitches[1] + x * 2 + 1] = background ? 0x66 : sample(2, x, y);
    }
    return status(vaUnmapBuffer(display, img->buf), "unmap NV12 source") &&
           status(vaPutImage(display, sid, img->image_id, 0, 0, img->width, img->height, 0, 0, img->width, img->height), "upload NV12 source");
}
static int process(VAContextID ctx, VASurfaceID dst, VAProcPipelineParameterBuffer *pipe)
{
    VABufferID buffer = VA_INVALID_ID;
    if (!status(vaCreateBuffer(display, ctx, VAProcPipelineParameterBufferType, sizeof(*pipe), 1, pipe, &buffer), "VPP params")) return 0;
    int ok = status(vaBeginPicture(display, ctx, dst), "VPP BeginPicture");
    if (ok) {
        ok = status(vaRenderPicture(display, ctx, &buffer, 1), "VPP RenderPicture");
        VAStatus end = vaEndPicture(display, ctx);
        ok = status(end, "VPP EndPicture") && ok;
    }
    vaDestroyBuffer(display, buffer);
    return ok && status(vaSyncSurface(display, dst), "VPP SyncSurface");
}
static int prime_sync(int fd, uint64_t flags)
{
    struct dma_buf_sync sync = {.flags = flags}; int ret;
    do { ret = ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync); } while (ret < 0 && (errno == EINTR || errno == EAGAIN));
    if (ret < 0) perror("DMA_BUF_IOCTL_SYNC");
    return ret == 0;
}
static uint8_t prime_sample(unsigned frame, unsigned plane, unsigned x, unsigned y)
{
    return plane == 0 ? 16 + (x * 3 + y * 5 + frame * 17) % 200 :
        plane == 1 ? 48 + (x + y * 3 + frame * 7) % 64 : 144 + (x * 3 + y + frame * 11) % 64;
}
static void prime_fill(uint8_t *data, unsigned pitch, unsigned chroma, unsigned frame)
{
    for (unsigned y = 0; y < 64; y++) for (unsigned x = 0; x < 64; x++) data[y * pitch + x] = prime_sample(frame, 0, x, y);
    for (unsigned y = 0; y < 32; y++) for (unsigned x = 0; x < 32; x++) {
        data[chroma + y * pitch + 2 * x] = prime_sample(frame, 1, x, y);
        data[chroma + y * pitch + 2 * x + 1] = prime_sample(frame, 2, x, y);
    }
}
static int prime_pixels(const uint8_t *data, unsigned y_pitch, unsigned uv_pitch, unsigned y_offset, unsigned uv_offset,
                         unsigned frame, int partial)
{
    for (unsigned plane = 0; plane < 3; plane++) for (unsigned y = 0; y < (plane ? 32u : 64u); y++)
        for (unsigned x = 0; x < (plane ? 32u : 64u); x++) {
            unsigned lo = plane ? 8 : 16, hi = plane ? 24 : 48;
            unsigned f = partial && x >= lo && x < hi && y >= lo && y < hi ? 7 : frame;
            size_t offset = plane ? uv_offset + (size_t)y * uv_pitch + 2 * x + plane - 1 : y_offset + (size_t)y * y_pitch + x;
            if (data[offset] != prime_sample(f, plane, x, y)) return 0;
        }
    return 1;
}
static int prime_bo(struct gbm_bo *bo, int fd, unsigned frame, int write, int partial)
{
    if (!prime_sync(fd, DMA_BUF_SYNC_START | (write ? DMA_BUF_SYNC_WRITE : DMA_BUF_SYNC_READ))) return 0;
    uint32_t pitch = 0; void *map = NULL;
    uint8_t *data = gbm_bo_map(bo, 0, 0, 64, 96, write ? GBM_BO_TRANSFER_WRITE : GBM_BO_TRANSFER_READ, &pitch, &map);
    int ok = data && pitch >= 64;
    if (ok && write) prime_fill(data, pitch, pitch * 64, frame);
    if (ok && !write) ok = prime_pixels(data, pitch, pitch, 0, pitch * 64, frame, partial);
    if (data) gbm_bo_unmap(bo, map);
    return prime_sync(fd, DMA_BUF_SYNC_END | (write ? DMA_BUF_SYNC_WRITE : DMA_BUF_SYNC_READ)) && ok;
}
static int prime_image(VASurfaceID sid, VAImage *out, unsigned frame)
{
    uint8_t *pixels;
    if (!status(vaGetImage(display, sid, 0, 0, 64, 64, out->image_id), "import GetImage") ||
        !status(vaMapBuffer(display, out->buf, (void **)&pixels), "import map image")) return 0;
    int ok = prime_pixels(pixels, out->pitches[0], out->pitches[1], out->offsets[0], out->offsets[1], frame, 0);
    return status(vaUnmapBuffer(display, out->buf), "import unmap image") && ok;
}
static int prime_upload(VASurfaceID sid, VAImage *in, unsigned frame, int partial)
{
    uint8_t *pixels;
    if (!status(vaMapBuffer(display, in->buf, (void **)&pixels), "import map upload")) return 0;
    prime_fill(pixels + in->offsets[0], in->pitches[0], in->offsets[1] - in->offsets[0], frame);
    if (!status(vaUnmapBuffer(display, in->buf), "import unmap upload")) return 0;
    unsigned pos = partial ? 16 : 0, size = partial ? 32 : 64;
    return status(vaPutImage(display, sid, in->image_id, pos, pos, size, size, pos, pos, size, size), "import PutImage");
}
static int prime_fd_pixels(const VADRMPRIMESurfaceDescriptor *desc, unsigned frame)
{
    if (desc->fourcc != VA_FOURCC_NV12 || desc->num_objects != 1 || desc->num_layers != 1 || desc->layers[0].num_planes != 2 ||
        desc->layers[0].object_index[0] || desc->layers[0].object_index[1] || desc->layers[0].pitch[0] < 64 || desc->layers[0].pitch[1] < 64 ||
        (uint64_t)desc->layers[0].offset[0] + (uint64_t)desc->layers[0].pitch[0] * 64 > desc->objects[0].size ||
        (uint64_t)desc->layers[0].offset[1] + (uint64_t)desc->layers[0].pitch[1] * 32 > desc->objects[0].size) return 0;
    int fd = desc->objects[0].fd;
    if (!prime_sync(fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ)) return 0;
    void *data = mmap(NULL, desc->objects[0].size, PROT_READ, MAP_SHARED, fd, 0);
    int ok = data != MAP_FAILED;
    if (ok) {
        ok = prime_pixels(data, desc->layers[0].pitch[0], desc->layers[0].pitch[1], desc->layers[0].offset[0], desc->layers[0].offset[1], frame, 0);
        munmap(data, desc->objects[0].size);
    }
    return prime_sync(fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ) && ok;
}
static void dmabuf_import_checks(int render_fd)
{
    struct gbm_device *dev = gbm_create_device(render_fd);
    struct gbm_bo *bo = dev ? gbm_bo_create(dev, 64, 96, GBM_FORMAT_R8, GBM_BO_USE_LINEAR) : NULL;
    int owner = bo ? gbm_bo_get_fd_for_plane(bo, 0) : -1, client = -1;
    VASurfaceID imported = VA_INVALID_ID, native = VA_INVALID_ID;
    VAConfigID cfg = VA_INVALID_ID; VAContextID ctx = VA_INVALID_ID;
    VAImage in = {.image_id = VA_INVALID_ID}, out = {.image_id = VA_INVALID_ID};
    VADRMPRIMESurfaceDescriptor desc = {.fourcc = VA_FOURCC_NV12, .width = 64, .height = 64, .num_objects = 1, .num_layers = 1};
    VADRMPRIMESurfaceDescriptor exported = {.objects = {{.fd = -1}}};
    off_t size = owner >= 0 ? lseek(owner, 0, SEEK_END) : -1;
    int ok = bo && owner >= 0 && size > 0 && (uint64_t)size <= UINT32_MAX && lseek(owner, 0, SEEK_SET) == 0;
    if (ok) {
        client = fcntl(owner, F_DUPFD_CLOEXEC, 0);
        desc.objects[0].fd = client; desc.objects[0].size = size; desc.objects[0].drm_format_modifier = gbm_bo_get_modifier(bo);
        desc.layers[0].drm_format = DRM_FORMAT_NV12; desc.layers[0].num_planes = 2;
        desc.layers[0].pitch[0] = desc.layers[0].pitch[1] = gbm_bo_get_stride_for_plane(bo, 0);
        desc.layers[0].offset[1] = desc.layers[0].pitch[0] * 64;
        VASurfaceAttrib attrs[] = {
            {.type = VASurfaceAttribMemoryType, .flags = VA_SURFACE_ATTRIB_SETTABLE,
             .value = {.type = VAGenericValueTypeInteger, .value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2}},
            {.type = VASurfaceAttribExternalBufferDescriptor, .flags = VA_SURFACE_ATTRIB_SETTABLE,
             .value = {.type = VAGenericValueTypePointer, .value.p = &desc}}};
        ok = client >= 0 && prime_bo(bo, owner, 0, 1, 0) &&
            status(vaCreateSurfaces(display, VA_RT_FORMAT_YUV420, 64, 64, &imported, 1, attrs, 2), "import linear NV12") &&
            image(VA_FOURCC_NV12, 64, 64, &in) && image(VA_FOURCC_NV12, 64, 64, &out);
    }
    if (client >= 0) { close(client); client = -1; }
    result("dmabuf-import-nv12", ok && prime_image(imported, &out, 0));
    int fresh = ok;
    for (unsigned frame = 1; frame <= 3 && fresh; frame++) fresh = prime_bo(bo, owner, frame, 1, 0) && prime_image(imported, &out, frame);
    result("dmabuf-import-external-refresh", fresh);
    result("dmabuf-import-image-writeback", fresh && prime_upload(imported, &in, 7, 1) && prime_bo(bo, owner, 3, 0, 1));
    VAConfigAttrib rt = {.type = VAConfigAttribRTFormat, .value = VA_RT_FORMAT_YUV420};
    int vpp = ok && status(vaCreateConfig(display, VAProfileNone, VAEntrypointVideoProc, &rt, 1, &cfg), "import VPP config") &&
        surface(VA_RT_FORMAT_YUV420, VA_FOURCC_NV12, &native) &&
        status(vaCreateContext(display, cfg, 64, 64, VA_PROGRESSIVE, &native, 1, &ctx), "import VPP context");
    VAProcPipelineParameterBuffer pipe = {.surface = imported};
    result("dmabuf-import-vpp-input", vpp && prime_bo(bo, owner, 4, 1, 0) && process(ctx, native, &pipe) && prime_image(native, &out, 4));
    pipe.surface = native;
    int output = vpp && prime_upload(native, &in, 5, 0) && process(ctx, imported, &pipe) && prime_bo(bo, owner, 5, 0, 0);
    result("dmabuf-import-vpp-output", output);
    int lifetime = output && status(vaExportSurfaceHandle(display, imported, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
        VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_COMPOSED_LAYERS, &exported), "import reexport");
    if (bo) { gbm_bo_destroy(bo); bo = NULL; }
    if (owner >= 0) { close(owner); owner = -1; }
    if (lifetime) lifetime = prime_image(imported, &out, 5);
    if (imported != VA_INVALID_ID) {
        lifetime = status(vaDestroySurfaces(display, &imported, 1), "destroy imported surface") && lifetime; imported = VA_INVALID_ID;
    }
    result("dmabuf-import-lifetime", lifetime && prime_fd_pixels(&exported, 5));
    if (exported.objects[0].fd >= 0) close(exported.objects[0].fd);
    if (ctx != VA_INVALID_ID) vaDestroyContext(display, ctx);
    if (native != VA_INVALID_ID) vaDestroySurfaces(display, &native, 1);
    if (cfg != VA_INVALID_ID) vaDestroyConfig(display, cfg);
    if (in.image_id != VA_INVALID_ID) vaDestroyImage(display, in.image_id);
    if (out.image_id != VA_INVALID_ID) vaDestroyImage(display, out.image_id);
    if (dev) gbm_device_destroy(dev);
}
struct dmabuf_format { unsigned rt, va, drm, planes, bytes, chroma_rows; const char *name; };
static const struct dmabuf_format dmabuf_formats[] = {
    {VA_RT_FORMAT_YUV420, VA_FOURCC_NV12, DRM_FORMAT_NV12, 2, 1, 32, "NV12"},
    {VA_RT_FORMAT_YUV420, VA_FOURCC_NV21, DRM_FORMAT_NV21, 2, 1, 32, "NV21"},
    {VA_RT_FORMAT_YUV420, VA_FOURCC_I420, DRM_FORMAT_YUV420, 3, 1, 32, "I420"},
    {VA_RT_FORMAT_YUV420, VA_FOURCC_YV12, DRM_FORMAT_YVU420, 3, 1, 32, "YV12"},
    {VA_RT_FORMAT_YUV420_10, VA_FOURCC_P010, DRM_FORMAT_P010, 2, 2, 32, "P010"},
    {VA_RT_FORMAT_YUV422, VA_FOURCC_YUY2, DRM_FORMAT_YUYV, 1, 2, 0, "YUY2"},
    {VA_RT_FORMAT_YUV422, VA_FOURCC_UYVY, DRM_FORMAT_UYVY, 1, 2, 0, "UYVY"},
    {VA_RT_FORMAT_YUV422, VA_FOURCC_VYUY, DRM_FORMAT_VYUY, 1, 2, 0, "VYUY"},
    {VA_RT_FORMAT_YUV422, VA_FOURCC_YVYU, DRM_FORMAT_YVYU, 1, 2, 0, "YVYU"},
    {VA_RT_FORMAT_YUV422, VA_FOURCC_422H, DRM_FORMAT_YUV422, 3, 1, 64, "422H"},
    {VA_RT_FORMAT_YUV422, VA_FOURCC_YV16, DRM_FORMAT_YVU422, 3, 1, 64, "YV16"},
    {VA_RT_FORMAT_YUV422_10, VA_FOURCC_Y210, DRM_FORMAT_Y210, 1, 4, 0, "Y210"},
    {VA_RT_FORMAT_RGB32, VA_FOURCC_ARGB, DRM_FORMAT_BGRA8888, 1, 4, 0, "ARGB"},
    {VA_RT_FORMAT_RGB32, VA_FOURCC_BGRA, DRM_FORMAT_ARGB8888, 1, 4, 0, "BGRA"},
    {VA_RT_FORMAT_RGB32, VA_FOURCC_BGRX, DRM_FORMAT_XRGB8888, 1, 4, 0, "BGRX"}};
static uint8_t dmabuf_byte(const struct dmabuf_format *f, unsigned plane, unsigned y, unsigned x, unsigned frame)
{
    if ((f->va == VA_FOURCC_BGRX || frame == 5 || frame == 6) && f->rt == VA_RT_FORMAT_RGB32 && x % 4 == (f->va == VA_FOURCC_ARGB ? 0u : 3u)) return 255;
    return 19 + plane * 71 + y * 3 + x * 7 + frame * 23;
}
/* Packed422 VPP uses RGA3, whose inspected minimum is128x128. Other DMA
 * formats keep64x64; enumeration does not imply64x64 VPP support. */
static unsigned dmabuf_dimension(const struct dmabuf_format *f)
{
    if (f->planes > 1) return (f->va == VA_FOURCC_422H || f->va == VA_FOURCC_YV16) ? f->chroma_rows : 2 * f->chroma_rows;
    return f->rt == VA_RT_FORMAT_YUV422 ? VPP_SIZE : 64;
}
static unsigned dmabuf_row(const struct dmabuf_format *f, unsigned p) { return p && f->planes == 3 ? dmabuf_dimension(f) / 2 : dmabuf_dimension(f) * f->bytes; }
static unsigned dmabuf_rows(const struct dmabuf_format *f, unsigned p) { return p ? f->chroma_rows : dmabuf_dimension(f); }
static int dmabuf_image(VASurfaceID sid, VAImage *im, const struct dmabuf_format *f, unsigned frame, int write)
{
    uint8_t *pixels = NULL;
    if (!write && !status(vaGetImage(display, sid, 0, 0, im->width, im->height, im->image_id), "DMA-BUF image download")) return 0;
    if (!status(vaMapBuffer(display, im->buf, (void **)&pixels), "DMA-BUF image map")) return 0;
    int ok = 1;
    for (unsigned p = 0; p < f->planes; p++) for (unsigned y = 0; y < dmabuf_rows(f, p); y++) for (unsigned x = 0; x < dmabuf_row(f, p); x++) {
        uint8_t *at = pixels + im->offsets[p] + (size_t)y * im->pitches[p] + x;
        if (write) *at = dmabuf_byte(f, p, y, x, frame);
        else if (*at != dmabuf_byte(f, p, y, x, frame)) {
            if (ok) snprintf(detail, sizeof(detail), "%s image frame=%u plane=%u x=%u y=%u wanted=%u got=%u", f->name, frame, p, x, y, dmabuf_byte(f, p, y, x, frame), *at);
            ok = 0;
        }
    }
    ok = status(vaUnmapBuffer(display, im->buf), "DMA-BUF image unmap") && ok;
    if (ok && write) ok = status(vaPutImage(display, sid, im->image_id, 0, 0, im->width, im->height, 0, 0, im->width, im->height), "DMA-BUF image upload");
    return ok;
}
static int dmabuf_object(const VADRMPRIMESurfaceDescriptor *d, const struct dmabuf_format *f, unsigned frame, int write, int partial)
{
    int ok = 1;
    if (d->fourcc != f->va || d->num_objects > 3 || !d->num_objects || (d->num_layers != 1 && d->num_layers != f->planes)) return 0;
    for (unsigned o = 0; o < d->num_objects; o++) {
        int fd = d->objects[o].fd;
        if (!prime_sync(fd, DMA_BUF_SYNC_START | (write ? DMA_BUF_SYNC_WRITE : DMA_BUF_SYNC_READ))) return 0;
        uint8_t *pixels = mmap(NULL, d->objects[o].size, PROT_READ | (write ? PROT_WRITE : 0), MAP_SHARED, fd, 0);
        if (pixels == MAP_FAILED) ok = 0;
        else {
            if (write) memset(pixels, 0xcd, d->objects[o].size);
            for (unsigned p = 0; p < f->planes; p++) {
                unsigned layer = d->num_layers == 1 ? 0 : p, plane = d->num_layers == 1 ? p : 0;
                if (d->layers[layer].object_index[plane] != o) continue;
                unsigned pitch = d->layers[layer].pitch[plane], offset = d->layers[layer].offset[plane];
                unsigned row = dmabuf_row(f, p), rows = dmabuf_rows(f, p);
                if (pitch < row || (uint64_t)offset + (uint64_t)pitch * (rows - 1) + row > d->objects[o].size) { ok = 0; continue; }
                for (unsigned y = 0; y < rows; y++) for (unsigned x = 0; x < row; x++) {
                    unsigned use = partial && y >= rows / 4 && y < rows * 3 / 4 && x >= row / 4 && x < row * 3 / 4 ? 3 : frame;
                    uint8_t *at = pixels + offset + (size_t)y * pitch + x;
                    if (write) *at = dmabuf_byte(f, p, y, x, frame);
                    else if (*at != dmabuf_byte(f, p, y, x, use)) {
                        if (ok) snprintf(detail, sizeof(detail), "%s object=%u frame=%u plane=%u x=%u y=%u wanted=%u got=%u", f->name, o, use, p, x, y, dmabuf_byte(f, p, y, x, use), *at);
                        ok = 0;
                    }
                }
            }
            if (partial && !write) for (unsigned at = 0; at < d->objects[o].size; at++) {
                int visible = 0;
                for (unsigned p = 0; p < f->planes; p++) {
                    unsigned layer = d->num_layers == 1 ? 0 : p, plane = d->num_layers == 1 ? p : 0;
                    unsigned offset = d->layers[layer].offset[plane], pitch = d->layers[layer].pitch[plane];
                    if (d->layers[layer].object_index[plane] == o && at >= offset && pitch &&
                        (at - offset) / pitch < dmabuf_rows(f, p) && (at - offset) % pitch < dmabuf_row(f, p)) visible = 1;
                }
                if (!visible && pixels[at] != 0xcd) {
                    if (ok) snprintf(detail, sizeof(detail), "%s object=%u padding offset=%u wanted=205 got=%u", f->name, o, at, pixels[at]);
                    ok = 0;
                }
            }
            munmap(pixels, d->objects[o].size);
        }
        ok = prime_sync(fd, DMA_BUF_SYNC_END | (write ? DMA_BUF_SYNC_WRITE : DMA_BUF_SYNC_READ)) && ok;
    }
    return ok;
}
static void dmabuf_close(VADRMPRIMESurfaceDescriptor *d)
{
    for (unsigned i = 0; i < d->num_objects; i++) if (d->objects[i].fd >= 0) { close(d->objects[i].fd); d->objects[i].fd = -1; }
}
static void dmabuf_layout_checks(int render_fd)
{
    struct gbm_device *dev = gbm_create_device(render_fd);
    for (unsigned n = 0; n < sizeof(dmabuf_formats) / sizeof(dmabuf_formats[0]); n++) {
        const struct dmabuf_format *f = &dmabuf_formats[n];
        unsigned size = dmabuf_dimension(f);
        for (unsigned variant = 0; variant < 4; variant++) {
            VADRMPRIMESurfaceDescriptor d = {.fourcc = f->va, .width = size, .height = size,
                .num_objects = variant & 1 ? f->planes : 1, .num_layers = variant & 2 ? f->planes : 1};
            struct gbm_bo *bos[3] = {0}; int ok = dev != NULL;
            for (unsigned o = 0; o < d.num_objects; o++) d.objects[o].fd = -1;
            for (unsigned o = 0; o < d.num_objects && ok; o++) {
                bos[o] = gbm_bo_create(dev, 512, 256, GBM_FORMAT_R8, GBM_BO_USE_LINEAR);
                int fd = bos[o] ? gbm_bo_get_fd_for_plane(bos[o], 0) : -1;
                off_t size = fd >= 0 ? lseek(fd, 0, SEEK_END) : -1;
                d.objects[o].fd = fd;
                ok = size > 0 && (uint64_t)size <= UINT32_MAX && lseek(fd, 0, SEEK_SET) == 0;
                if (ok) { d.objects[o].size = size; d.objects[o].drm_format_modifier = gbm_bo_get_modifier(bos[o]); }
            }
            for (unsigned p = 0; p < f->planes; p++) {
                unsigned layer = d.num_layers == 1 ? 0 : p, plane = d.num_layers == 1 ? p : 0;
                d.layers[layer].drm_format = d.num_layers == 1 ? f->drm : f->planes == 1 ? f->drm :
                    f->va == VA_FOURCC_P010 ? (p ? DRM_FORMAT_GR1616 : DRM_FORMAT_R16) : p && f->planes == 2 ? DRM_FORMAT_GR88 : DRM_FORMAT_R8;
                d.layers[layer].num_planes = d.num_layers == 1 ? f->planes : 1;
                d.layers[layer].object_index[plane] = d.num_objects == 1 ? 0 : p;
                d.layers[layer].offset[plane] = 64 + (d.num_objects == 1 ? p * 32768 : 0);
                d.layers[layer].pitch[plane] = dmabuf_row(f, p) + 64 + p * 16;
            }
            VASurfaceID sid = VA_INVALID_ID; VAImage im = {.image_id = VA_INVALID_ID};
            VASurfaceAttrib attrs[] = {
                {.type = VASurfaceAttribPixelFormat, .flags = VA_SURFACE_ATTRIB_SETTABLE, .value = {.type = VAGenericValueTypeInteger, .value.i = f->va}},
                {.type = VASurfaceAttribMemoryType, .flags = VA_SURFACE_ATTRIB_SETTABLE, .value = {.type = VAGenericValueTypeInteger, .value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2}},
                {.type = VASurfaceAttribExternalBufferDescriptor, .flags = VA_SURFACE_ATTRIB_SETTABLE, .value = {.type = VAGenericValueTypePointer, .value.p = &d}}};
            ok = ok && dmabuf_object(&d, f, 0, 1, 0) && status(vaCreateSurfaces(display, f->rt, size, size, &sid, 1, attrs, 3), "DMA-BUF layout import") && image(f->va, size, size, &im);
            ok = ok && dmabuf_image(sid, &im, f, 0, 0) && dmabuf_object(&d, f, 1, 1, 0) && dmabuf_image(sid, &im, f, 1, 0);
            ok = ok && dmabuf_image(sid, &im, f, 3, 1) && dmabuf_object(&d, f, 3, 0, 0);
            /* Preserve the latest external background through a partial write. */
            ok = ok && dmabuf_object(&d, f, 1, 1, 0) && status(vaPutImage(display, sid, im.image_id, size / 4, size / 4, size / 2, size / 2, size / 4, size / 4, size / 2, size / 2), "DMA-BUF partial upload") && dmabuf_object(&d, f, 1, 0, 1);
            VADRMPRIMESurfaceDescriptor e = {0};
            ok = ok && status(vaExportSurfaceHandle(display, sid, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                VA_EXPORT_SURFACE_READ_WRITE | (variant & 2 ? VA_EXPORT_SURFACE_COMPOSED_LAYERS : VA_EXPORT_SURFACE_SEPARATE_LAYERS), &e), "DMA-BUF layout reexport");
            if (variant == 3 && f->va != VA_FOURCC_P010 && f->va != VA_FOURCC_Y210) {
                VAConfigID cfg = VA_INVALID_ID; VAContextID ctx = VA_INVALID_ID; VASurfaceID native = VA_INVALID_ID;
                VAConfigAttrib rt = {.type = VAConfigAttribRTFormat, .value = f->rt};
                ok = ok && status(vaCreateConfig(display, VAProfileNone, VAEntrypointVideoProc, &rt, 1, &cfg), "multi-object VPP config") &&
                    surface_size(f->rt, f->va, size, size, &native) && status(vaCreateContext(display, cfg, size, size, VA_PROGRESSIVE, &native, 1, &ctx), "multi-object VPP context");
                VAProcPipelineParameterBuffer pipe = {.surface = sid};
                ok = ok && dmabuf_object(&d, f, 5, 1, 0) && process(ctx, native, &pipe) && dmabuf_image(native, &im, f, 5, 0);
                pipe.surface = native;
                ok = ok && dmabuf_image(native, &im, f, 6, 1) && process(ctx, sid, &pipe) && dmabuf_object(&d, f, 6, 0, 0);
                if (ctx != VA_INVALID_ID) vaDestroyContext(display, ctx);
                if (native != VA_INVALID_ID) vaDestroySurfaces(display, &native, 1);
                if (cfg != VA_INVALID_ID) vaDestroyConfig(display, cfg);
            }
            dmabuf_close(&d); for (unsigned o = 0; o < 3; o++) if (bos[o]) gbm_bo_destroy(bos[o]);
            ok = ok && dmabuf_object(&e, f, 4, 1, 0) && dmabuf_image(sid, &im, f, 4, 0);
            if (sid != VA_INVALID_ID) { ok = status(vaDestroySurfaces(display, &sid, 1), "DMA-BUF layout destroy") && ok; sid = VA_INVALID_ID; }
            ok = ok && dmabuf_object(&e, f, 4, 0, 0);
            char tag[100]; snprintf(tag, sizeof(tag), "dmabuf-layout-%s-%u", f->name, variant); result(tag, ok);
            dmabuf_close(&e); if (im.image_id != VA_INVALID_ID) vaDestroyImage(display, im.image_id);
        }
        VASurfaceID sid = VA_INVALID_ID; VAImage im = {.image_id = VA_INVALID_ID}; VADRMPRIMESurfaceDescriptor e = {0};
        int ok = surface_size(f->rt, f->va, size, size, &sid) && image(f->va, size, size, &im) && dmabuf_image(sid, &im, f, 1, 1) &&
            status(vaExportSurfaceHandle(display, sid, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                VA_EXPORT_SURFACE_READ_WRITE | VA_EXPORT_SURFACE_COMPOSED_LAYERS, &e), "DMA-BUF native format export");
        ok = ok && e.fourcc == f->va && e.layers[0].drm_format == f->drm && dmabuf_object(&e, f, 1, 0, 0) &&
            dmabuf_object(&e, f, 2, 1, 0) && dmabuf_image(sid, &im, f, 2, 0);
        if (sid != VA_INVALID_ID) vaDestroySurfaces(display, &sid, 1);
        ok = ok && dmabuf_object(&e, f, 2, 0, 0);
        char tag[100]; snprintf(tag, sizeof(tag), "dmabuf-export-writeback-%s", f->name); result(tag, ok);
        dmabuf_close(&e); if (im.image_id != VA_INVALID_ID) vaDestroyImage(display, im.image_id);
    }
    if (dev) gbm_device_destroy(dev);
}
/* Each case requires real DMABUF queues; a staging fallback is a failure.
 * Native export -> external producer -> imported consumer also checks aliases
 * across distinct VA IDs and object lifetime after the exporting VA ID dies. */
static int dmabuf_step(int ok, const char *stage, unsigned frame)
{
    if (!ok) { char previous[sizeof(detail)]; memcpy(previous, detail, sizeof(previous)); previous[sizeof(previous) - 1] = 0;
        snprintf(detail, sizeof(detail), "%s frame=%u: %.135s", stage, frame, previous); }
    return ok;
}
static void dmabuf_exporter_info(const VADRMPRIMESurfaceDescriptor *d, const char *role)
{
    for (unsigned o = 0; o < d->num_objects; o++) {
        char path[80], line[256]; snprintf(path, sizeof(path), "/proc/self/fdinfo/%d", d->objects[o].fd);
        FILE *file = fopen(path, "r"); if (!file) continue;
        while (fgets(line, sizeof(line), file)) if (!strncmp(line, "exp_name:", 9))
            printf("DMA-BUF %s format=%08x object=%u size=%u %s", role, d->fourcc, o, d->objects[o].size, line);
        fclose(file);
    }
}
static void dmabuf_direct_checks(void)
{
    const unsigned formats[] = {0, 1, 2, 5, 6, 7, 8, 14};
    const char *old = getenv("VAV4L2_VPP_DMABUF"); char *saved = old ? strdup(old) : NULL;
    setenv("VAV4L2_VPP_DMABUF", "required", 1);
    for (unsigned i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
        const struct dmabuf_format *f = &dmabuf_formats[formats[i]];
        unsigned size = dmabuf_dimension(f);
        VASurfaceID src = VA_INVALID_ID, dst = VA_INVALID_ID, imported = VA_INVALID_ID;
        VAConfigID cfg = VA_INVALID_ID; VAContextID ctx = VA_INVALID_ID;
        VAImage im = {.image_id = VA_INVALID_ID}; VADRMPRIMESurfaceDescriptor d[2] = {{0}};
        VAConfigAttrib rt = {.type = VAConfigAttribRTFormat, .value = f->rt};
        int ok = surface_size(f->rt, f->va, size, size, &src) && surface_size(f->rt, f->va, size, size, &dst) &&
            image(f->va, size, size, &im) && dmabuf_image(src, &im, f, 0, 1) && dmabuf_image(dst, &im, f, 113, 1) &&
            status(vaExportSurfaceHandle(display, src, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                VA_EXPORT_SURFACE_READ_WRITE | VA_EXPORT_SURFACE_COMPOSED_LAYERS, &d[0]), "direct input export") &&
            status(vaExportSurfaceHandle(display, dst, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                VA_EXPORT_SURFACE_READ_WRITE | VA_EXPORT_SURFACE_COMPOSED_LAYERS, &d[1]), "direct output export");
        if (ok) { dmabuf_exporter_info(&d[0], "source"); dmabuf_exporter_info(&d[1], "destination"); }
        VASurfaceAttrib attrs[] = {
            {.type = VASurfaceAttribMemoryType, .flags = VA_SURFACE_ATTRIB_SETTABLE, .value = {.type = VAGenericValueTypeInteger, .value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2}},
            {.type = VASurfaceAttribExternalBufferDescriptor, .flags = VA_SURFACE_ATTRIB_SETTABLE, .value = {.type = VAGenericValueTypePointer, .value.p = &d[1]}}};
        ok = ok && status(vaCreateSurfaces(display, f->rt, size, size, &imported, 1, attrs, 2), "direct output reimport") &&
            status(vaCreateConfig(display, VAProfileNone, VAEntrypointVideoProc, &rt, 1, &cfg), "direct VPP config") &&
            status(vaCreateContext(display, cfg, size, size, VA_PROGRESSIVE, NULL, 0, &ctx), "direct VPP context");
        int ready = ok;
        VAProcPipelineParameterBuffer pipe = {.surface = src};
        for (unsigned frame = 0; frame < 32 && ok; frame++) {
            ok = dmabuf_step(dmabuf_object(&d[0], f, frame, 1, 0), "producer-write", frame) &&
                dmabuf_step(dmabuf_object(&d[0], f, frame, 0, 0), "producer-raw-FD", frame) &&
                dmabuf_step(process(ctx, imported, &pipe), "VPP-job", frame) &&
                dmabuf_step(dmabuf_object(&d[0], f, frame, 0, 0), "producer-after-VPP", frame) &&
                dmabuf_step(dmabuf_object(&d[1], f, frame, 0, 0), "destination-raw-FD", frame) &&
                dmabuf_step(dmabuf_image(imported, &im, f, frame, 0), "imported-GetImage", frame) &&
                dmabuf_step(dmabuf_image(dst, &im, f, frame, 0), "exporter-GetImage", frame);
        }
        if (i == 0 && ok) {
            /* The inspected RGA2 crop/compose minimum is34, not32. */
            VARectangle region = {12, 12, 40, 40}; pipe.surface_region = pipe.output_region = &region;
            ok = dmabuf_object(&d[0], f, 6, 1, 0) && dmabuf_object(&d[1], f, 5, 1, 0) && process(ctx, imported, &pipe) &&
                status(vaGetImage(display, imported, 0, 0, size, size, im.image_id), "direct crop-compose download");
            uint8_t *pixels = NULL;
            if (ok && status(vaMapBuffer(display, im.buf, (void **)&pixels), "direct crop-compose map")) {
                for (unsigned p = 0; p < f->planes; p++) for (unsigned y = 0; y < dmabuf_rows(f, p); y++) for (unsigned x = 0; x < dmabuf_row(f, p); x++) {
                    unsigned divisor = p ? 2 : 1, top = region.y / divisor, bottom = (region.y + region.height) / divisor;
                    unsigned frame = x >= (unsigned)region.x && x < (unsigned)region.x + region.width && y >= top && y < bottom ? 6 : 5;
                    if (pixels[im.offsets[p] + (size_t)y * im.pitches[p] + x] != dmabuf_byte(f, p, y, x, frame)) {
                        if (ok) snprintf(detail, sizeof(detail), "direct crop-compose plane=%u x=%u y=%u background/region mismatch", p, x, y);
                        ok = 0;
                    }
                }
                ok = status(vaUnmapBuffer(display, im.buf), "direct crop-compose unmap") && ok;
            } else ok = 0;
            pipe.surface_region = pipe.output_region = NULL;
        }
        /* Native exporter can disappear while the imported alias remains. */
        if (dst != VA_INVALID_ID) { ok = status(vaDestroySurfaces(display, &dst, 1), "direct exporter destroy") && ok; dst = VA_INVALID_ID; }
        ok = ok && dmabuf_object(&d[0], f, 6, 1, 0) && process(ctx, imported, &pipe) && dmabuf_image(imported, &im, f, 6, 0);
        pipe.surface = imported;
        /* A no-op must fail even when the source previously held frame6. */
        ok = ok && dmabuf_object(&d[0], f, 7, 1, 0) && process(ctx, src, &pipe) && dmabuf_image(src, &im, f, 6, 0);
        char tag[100]; snprintf(tag, sizeof(tag), "dmabuf-direct-stress-%s", f->name); result(tag, ok);
        int cold_ok = ready; pipe = (VAProcPipelineParameterBuffer){.surface = src};
        for (unsigned cold = 0; cold < 8 && cold_ok; cold++) {
            if (ctx != VA_INVALID_ID) {
                cold_ok = dmabuf_step(status(vaDestroyContext(display, ctx), "cold context destroy"), "cold-context-destroy", cold);
                if (cold_ok) ctx = VA_INVALID_ID;
            }
            unsigned frame = 37 + cold;
            cold_ok = cold_ok && dmabuf_step(status(vaCreateContext(display, cfg, size, size, VA_PROGRESSIVE, NULL, 0, &ctx), "cold VPP context"), "cold-context-create", cold) &&
                dmabuf_step(dmabuf_object(&d[0], f, frame, 1, 0), "cold-producer-write", cold) &&
                dmabuf_step(dmabuf_object(&d[1], f, 113, 1, 0), "cold-destination-poison", cold) &&
                dmabuf_step(process(ctx, imported, &pipe), "cold-VPP-job", cold) &&
                dmabuf_step(dmabuf_object(&d[0], f, frame, 0, 0), "cold-source-after-VPP", cold) &&
                dmabuf_step(dmabuf_object(&d[1], f, frame, 0, 0), "cold-destination-raw-FD", cold) &&
                dmabuf_step(dmabuf_image(imported, &im, f, frame, 0), "cold-imported-GetImage", cold);
        }
        snprintf(tag, sizeof(tag), "dmabuf-cold-stress-%s", f->name); result(tag, cold_ok);
        if (ctx != VA_INVALID_ID) vaDestroyContext(display, ctx);
        if (cfg != VA_INVALID_ID) vaDestroyConfig(display, cfg);
        if (src != VA_INVALID_ID) vaDestroySurfaces(display, &src, 1);
        if (dst != VA_INVALID_ID) vaDestroySurfaces(display, &dst, 1);
        if (imported != VA_INVALID_ID) vaDestroySurfaces(display, &imported, 1);
        if (im.image_id != VA_INVALID_ID) vaDestroyImage(display, im.image_id);
        dmabuf_close(&d[0]); dmabuf_close(&d[1]);
    }
    if (saved) { setenv("VAV4L2_VPP_DMABUF", saved, 1); free(saved); } else unsetenv("VAV4L2_VPP_DMABUF");
}
static void dmabuf_multiplanar_checks(int render_fd)
{
    struct gbm_device *dev = gbm_create_device(render_fd);
    const char *old = getenv("VAV4L2_VPP_DMABUF"); char *saved = old ? strdup(old) : NULL;
    setenv("VAV4L2_VPP_DMABUF", "required", 1);
    for (unsigned n = 0; n < 2; n++) {
        struct dmabuf_format f = dmabuf_formats[n]; f.chroma_rows = 64;
        VADRMPRIMESurfaceDescriptor d[2] = {{0}}, retained[2] = {{0}};
        struct gbm_bo *bo[2][2] = {{0}}; VASurfaceID sid[2] = {VA_INVALID_ID, VA_INVALID_ID};
        VAImage im = {.image_id = VA_INVALID_ID}; VAConfigID cfg = VA_INVALID_ID; VAContextID ctx = VA_INVALID_ID;
        int ok = dev != NULL;
        for (unsigned i = 0; i < 2; i++) {
            d[i].fourcc = f.va; d[i].width = d[i].height = 128; d[i].num_objects = 2; d[i].num_layers = 1;
            d[i].layers[0].drm_format = f.drm; d[i].layers[0].num_planes = 2;
            for (unsigned p = 0; p < 2; p++) d[i].objects[p].fd = -1;
            for (unsigned p = 0; p < 2 && ok; p++) {
                bo[i][p] = gbm_bo_create(dev, 128, p ? 64 : 128, GBM_FORMAT_R8, GBM_BO_USE_LINEAR);
                int fd = bo[i][p] ? gbm_bo_get_fd_for_plane(bo[i][p], 0) : -1;
                off_t size = fd >= 0 ? lseek(fd, 0, SEEK_END) : -1;
                d[i].objects[p].fd = fd;
                ok = size > 0 && (uint64_t)size <= UINT32_MAX && lseek(fd, 0, SEEK_SET) == 0;
                if (ok) {
                    d[i].objects[p].size = size; d[i].objects[p].drm_format_modifier = gbm_bo_get_modifier(bo[i][p]);
                    d[i].layers[0].object_index[p] = p; d[i].layers[0].pitch[p] = gbm_bo_get_stride_for_plane(bo[i][p], 0);
                }
            }
        }
        VASurfaceAttrib attrs[] = {
            {.type = VASurfaceAttribMemoryType, .flags = VA_SURFACE_ATTRIB_SETTABLE, .value = {.type = VAGenericValueTypeInteger, .value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2}},
            {.type = VASurfaceAttribExternalBufferDescriptor, .flags = VA_SURFACE_ATTRIB_SETTABLE, .value = {.type = VAGenericValueTypePointer, .value.p = d}}};
        ok = ok && status(vaCreateSurfaces(display, f.rt, 128, 128, sid, 2, attrs, 2), "two-object DMA surfaces") && image(f.va, 128, 128, &im);
        for (unsigned i = 0; i < 2 && ok; i++) ok = status(vaExportSurfaceHandle(display, sid[i], VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
            VA_EXPORT_SURFACE_READ_WRITE | VA_EXPORT_SURFACE_COMPOSED_LAYERS, &retained[i]), "two-object DMA reexport");
        for (unsigned i = 0; i < 2; i++) {
            dmabuf_close(&d[i]); for (unsigned p = 0; p < 2; p++) if (bo[i][p]) gbm_bo_destroy(bo[i][p]);
        }
        VAConfigAttrib rt = {.type = VAConfigAttribRTFormat, .value = f.rt};
        ok = ok && status(vaCreateConfig(display, VAProfileNone, VAEntrypointVideoProc, &rt, 1, &cfg), "two-object VPP config") &&
            status(vaCreateContext(display, cfg, 128, 128, VA_PROGRESSIVE, NULL, 0, &ctx), "two-object VPP context");
        VAProcPipelineParameterBuffer pipe = {.surface = sid[0]};
        for (unsigned frame = 0; frame < 34 && ok; frame++) ok = dmabuf_object(&retained[0], &f, frame, 1, 0) &&
            process(ctx, sid[1], &pipe) && dmabuf_image(sid[1], &im, &f, frame, 0);
        char tag[100]; snprintf(tag, sizeof(tag), "dmabuf-multiplanar-stress-%s", f.name); result(tag, ok);
        if (ctx != VA_INVALID_ID) vaDestroyContext(display, ctx);
        if (cfg != VA_INVALID_ID) vaDestroyConfig(display, cfg);
        for (unsigned i = 0; i < 2; i++) { if (sid[i] != VA_INVALID_ID) vaDestroySurfaces(display, &sid[i], 1); dmabuf_close(&retained[i]); }
        if (im.image_id != VA_INVALID_ID) vaDestroyImage(display, im.image_id);
    }
    if (saved) { setenv("VAV4L2_VPP_DMABUF", saved, 1); free(saved); } else unsetenv("VAV4L2_VPP_DMABUF");
    if (dev) gbm_device_destroy(dev);
}
static void save_image_failure(const VAImage *img, const void *pixels)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s.failure-%u.bin", report_path, passed + failed + 1);
    FILE *file = fopen(path, "wb");
    if (!file) { perror("failure image"); return; }
    if (fwrite(pixels, 1, img->data_size, file) != img->data_size) perror("write failure image");
    if (fclose(file)) perror("close failure image");
    snprintf(path, sizeof(path), "%s.failure-%u.json", report_path, passed + failed + 1);
    file = fopen(path, "w");
    if (!file) { perror("failure image layout"); return; }
    fprintf(file, "{\"fourcc\":%u,\"width\":%u,\"height\":%u,\"size\":%u,\"planes\":%u,\"pitches\":[%u,%u,%u],\"offsets\":[%u,%u,%u]}\n",
        img->format.fourcc, img->width, img->height, img->data_size, img->num_planes,
        img->pitches[0], img->pitches[1], img->pitches[2], img->offsets[0], img->offsets[1], img->offsets[2]);
    if (fclose(file)) perror("close failure image layout");
}
static int process_composition(VAContextID ctx, VASurfaceID dst, VASurfaceID src, int array)
{
    VARectangle input = {0, 0, VPP_SIZE / 2, VPP_SIZE / 2};
    VARectangle output[] = {{0, 0, VPP_SIZE / 2, VPP_SIZE / 2},
        {VPP_SIZE / 2, VPP_SIZE / 2, VPP_SIZE / 2, VPP_SIZE / 2}};
    VAProcPipelineParameterBuffer pipes[] = {{.surface = src, .surface_region = &input,
        .output_region = &output[0], .output_background_color = 0xff000000},
        {.surface = src, .surface_region = &input, .output_region = &output[1]}};
    VABufferID buffers[] = {VA_INVALID_ID, VA_INVALID_ID};
    int n = array ? 1 : 2, ok = 1;
    for (int i = 0; i < n && ok; i++)
        ok = status(vaCreateBuffer(display, ctx, VAProcPipelineParameterBufferType, sizeof(pipes[0]),
            array ? 2 : 1, array ? pipes : &pipes[i], &buffers[i]), "VPP composition buffer");
    if (ok && status(vaBeginPicture(display, ctx, dst), "VPP composition BeginPicture")) {
        for (int i = 0; i < n && ok; i++) ok = status(vaRenderPicture(display, ctx, &buffers[i], 1), "VPP composition RenderPicture");
        ok = status(vaEndPicture(display, ctx), "VPP composition EndPicture") && ok;
    } else ok = 0;
    for (int i = 0; i < n; i++) if (buffers[i] != VA_INVALID_ID) vaDestroyBuffer(display, buffers[i]);
    return ok && status(vaSyncSurface(display, dst), "VPP composition SyncSurface");
}
static int pixels(VASurfaceID sid, VAImage *img, unsigned rotation, unsigned mirror, int region)
{
    uint8_t *p;
    if (!status(vaGetImage(display, sid, 0, 0, img->width, img->height, img->image_id), "VPP GetImage") ||
        !status(vaMapBuffer(display, img->buf, (void **)&p), "VPP MapBuffer")) return 0;
    unsigned differences = 0, first[5] = {0};
    for (unsigned plane = 0; plane < 3; plane++) {
        unsigned size = plane ? img->width / 2 : img->width;
        for (unsigned y = 0; y < size; y++) for (unsigned x = 0; x < size; x++) {
            unsigned sx = x, sy = y, w = size / 2, offset = size / 4;
            uint8_t expected;
            if (region == 3 && ((x < w) != (y < w))) {
                expected = plane ? 128 : 16;
            } else if (region == 1 && (x < offset || x >= offset + w || y < offset || y >= offset + w)) {
                expected = plane ? 0x66 : 0x55;
            } else if (region == 2 && (x < w || y >= w)) {
                expected = plane ? 128 : 16;
            } else {
                if (region == 2) sx -= w;
                if (region == 3) { sx %= w; sy %= w; }
                if (!region) {
                    unsigned mx = mirror & VA_MIRROR_HORIZONTAL ? size - 1 - x : x;
                    unsigned my = mirror & VA_MIRROR_VERTICAL ? size - 1 - y : y;
                    if (rotation == VA_ROTATION_90) { sx = my; sy = size - 1 - mx; }
                    else if (rotation == VA_ROTATION_180) { sx = size - 1 - mx; sy = size - 1 - my; }
                    else if (rotation == VA_ROTATION_270) { sx = size - 1 - my; sy = mx; }
                    else { sx = mx; sy = my; }
                }
                expected = sample(plane, sx, sy);
            }
            uint8_t actual = plane ? p[img->offsets[1] + (size_t)y * img->pitches[1] + x * 2 + plane - 1] :
                                     p[img->offsets[0] + (size_t)y * img->pitches[0] + x];
            if (actual != expected) {
                if (!differences) { first[0] = plane; first[1] = x; first[2] = y; first[3] = expected; first[4] = actual; }
                differences++;
            }
        }
    }
    if (differences) {
        save_image_failure(img, p);
        snprintf(detail, sizeof(detail), "VPP differences=%u first plane=%u x=%u y=%u expected=%u actual=%u artifact=failure-%u",
                 differences, first[0], first[1], first[2], first[3], first[4], passed + failed + 1);
        fprintf(stderr, "%s\n", detail);
    }
    return status(vaUnmapBuffer(display, img->buf), "VPP UnmapBuffer") && !differences;
}
static int flat_upload(VASurfaceID sid, VAImage *img, int rgb)
{
    uint8_t *p;
    if (!status(vaMapBuffer(display, img->buf, (void **)&p), "map flat source")) return 0;
    for (unsigned y = 0; y < img->height; y++) for (unsigned x = 0; x < img->width; x++) {
        uint8_t *pixel = p + img->offsets[0] + (size_t)y * img->pitches[0] + x * (rgb ? 4 : 1);
        if (rgb) { pixel[0] = 0; pixel[1] = 0; pixel[2] = 255; pixel[3] = 255; }
        else *pixel = 81;
    }
    if (!rgb) for (unsigned y = 0; y < img->height / 2; y++) for (unsigned x = 0; x < img->width / 2; x++) {
        p[img->offsets[1] + (size_t)y * img->pitches[1] + x * 2] = 90;
        p[img->offsets[1] + (size_t)y * img->pitches[1] + x * 2 + 1] = 240;
    }
    return status(vaUnmapBuffer(display, img->buf), "unmap flat source") &&
        status(vaPutImage(display, sid, img->image_id, 0, 0, img->width, img->height, 0, 0, img->width, img->height), "upload flat source");
}
static int flat_pixels(VASurfaceID sid, VAImage *img, int rgb, int bt709)
{
    uint8_t *p;
    if (!status(vaGetImage(display, sid, 0, 0, img->width, img->height, img->image_id), "CSC GetImage") ||
        !status(vaMapBuffer(display, img->buf, (void **)&p), "CSC MapBuffer")) return 0;
    unsigned bad = 0; int first_expected = 0, first_actual = 0;
    for (unsigned y = 0; y < img->height; y++) for (unsigned x = 0; x < img->width; x++) {
        uint8_t *pixel = p + img->offsets[0] + (size_t)y * img->pitches[0] + x * (rgb ? 4 : 1);
        int expected[3] = {rgb ? 0 : bt709 ? 63 : 81, rgb ? bt709 ? 24 : 0 : 0, 255};
        for (unsigned c = 0; c < (rgb ? 3u : 1u); c++) if (abs((int)pixel[c] - expected[c]) > 3) { if (!bad) { first_expected = expected[c]; first_actual = pixel[c]; } bad++; }
    }
    if (!rgb) for (unsigned y = 0; y < img->height / 2; y++) for (unsigned x = 0; x < img->width / 2; x++) {
        uint8_t *pixel = p + img->offsets[1] + (size_t)y * img->pitches[1] + x * 2;
        if (abs((int)pixel[0] - (bt709 ? 102 : 90)) > 3 || abs((int)pixel[1] - 240) > 3) {
            if (!bad) { first_expected = abs((int)pixel[0] - (bt709 ? 102 : 90)) > 3 ? (bt709 ? 102 : 90) : 240;
                first_actual = abs((int)pixel[0] - (bt709 ? 102 : 90)) > 3 ? pixel[0] : pixel[1]; } bad++;
        }
    }
    if (bad) { save_image_failure(img, p); snprintf(detail, sizeof(detail), "CSC bad=%u tolerance=3 first expected=%d actual=%d artifact=failure-%u",
        bad, first_expected, first_actual, passed + failed + 1); fprintf(stderr, "%s\n", detail); }
    return status(vaUnmapBuffer(display, img->buf), "CSC UnmapBuffer") && !bad;
}

static int vpp_dimension_contract(VAConfigID config, VAContextID ctx)
{
    VAProcPipelineCaps caps;
    VAConfigAttrib maximum[] = {{.type = VAConfigAttribMaxPictureWidth}, {.type = VAConfigAttribMaxPictureHeight}};
    unsigned count = 0, found = 0;
    if (!status(vaQueryVideoProcPipelineCaps(display, ctx, NULL, 0, &caps), "VPP pipeline caps") ||
        !status(vaGetConfigAttributes(display, VAProfileNone, VAEntrypointVideoProc, maximum, 2), "VPP max dimensions") ||
        !status(vaQuerySurfaceAttributes(display, config, NULL, &count), "VPP surface attribute count")) return 0;
    VASurfaceAttrib *attributes = calloc(count, sizeof(*attributes));
    if (!attributes) return 0;
    int ok = status(vaQuerySurfaceAttributes(display, config, attributes, &count), "VPP surface attributes");
    unsigned limits[] = {caps.min_input_width < caps.min_output_width ? caps.min_input_width : caps.min_output_width,
        caps.min_input_height < caps.min_output_height ? caps.min_input_height : caps.min_output_height,
        caps.max_input_width > caps.max_output_width ? caps.max_input_width : caps.max_output_width,
        caps.max_input_height > caps.max_output_height ? caps.max_input_height : caps.max_output_height};
    for (unsigned i = 2; i < 4; i++) if (limits[i] > UINT16_MAX) limits[i] = UINT16_MAX;
    const VASurfaceAttribType types[] = {VASurfaceAttribMinWidth, VASurfaceAttribMinHeight, VASurfaceAttribMaxWidth, VASurfaceAttribMaxHeight};
    for (unsigned i = 0; i < count; i++) for (unsigned j = 0; j < 4; j++) if (attributes[i].type == types[j]) {
        found |= 1u << j;
        ok &= attributes[i].value.type == VAGenericValueTypeInteger && (unsigned)attributes[i].value.value.i == limits[j];
    }
    ok &= found == 15 && limits[0] && limits[1] && limits[0] <= limits[2] && limits[1] <= limits[3] &&
          maximum[0].value == limits[2] && maximum[1].value == limits[3];
    VAConfigID accepted = VA_INVALID_ID;
    if (ok) ok = status(vaCreateConfig(display, VAProfileNone, VAEntrypointVideoProc, maximum, 2, &accepted), "VPP advertised max config");
    if (accepted != VA_INVALID_ID) vaDestroyConfig(display, accepted);
    free(attributes);
    if (!ok && !detail[0]) snprintf(detail, sizeof(detail), "VPP config/surface/pipeline dimension contract differs");
    return ok;
}
static void vpp_checks(void)
{
    VAConfigID config = VA_INVALID_ID;
    VAContextID ctx = VA_INVALID_ID;
    VASurfaceID src = VA_INVALID_ID, dst = VA_INVALID_ID, planar = VA_INVALID_ID, rgb = VA_INVALID_ID;
    VAImage img = {.image_id = VA_INVALID_ID}, rgb_image = {.image_id = VA_INVALID_ID};
    VAConfigAttrib rt = {.type = VAConfigAttribRTFormat, .value = VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_RGB32};
    int setup = status(vaCreateConfig(display, VAProfileNone, VAEntrypointVideoProc, &rt, 1, &config), "VPP config") &&
        surface_size(VA_RT_FORMAT_YUV420, VA_FOURCC_NV12, VPP_SIZE, VPP_SIZE, &src) && surface_size(VA_RT_FORMAT_YUV420, VA_FOURCC_NV12, VPP_SIZE, VPP_SIZE, &dst) &&
        image(VA_FOURCC_NV12, VPP_SIZE, VPP_SIZE, &img) && upload_nv12(src, &img, 0) && upload_nv12(dst, &img, 1) &&
        status(vaCreateContext(display, config, VPP_SIZE, VPP_SIZE, VA_PROGRESSIVE, &dst, 1, &ctx), "VPP context");
    if (!setup) { result("vpp-setup", 0); goto done; }
    result("vpp-dimension-capability-contract", vpp_dimension_contract(config, ctx));
    VARectangle input = {VPP_SIZE / 4, VPP_SIZE / 4, VPP_SIZE / 2, VPP_SIZE / 2}, output = {VPP_SIZE / 4, VPP_SIZE / 4, VPP_SIZE / 2, VPP_SIZE / 2};
    VAProcPipelineParameterBuffer pipe = {.surface = src, .surface_region = &input, .output_region = &output};
    result("vpp-crop-compose-transparent", process(ctx, dst, &pipe) && pixels(dst, &img, 0, 0, 1));
    input.x = input.y = 0; output.x = VPP_SIZE / 2; output.y = 0; pipe.output_background_color = 0xff000000;
    result("vpp-warm-crop-compose-black", process(ctx, dst, &pipe) && pixels(dst, &img, 0, 0, 2));
    pipe.surface_region = pipe.output_region = NULL;
    result("vpp-reset-full-frame", process(ctx, dst, &pipe) && pixels(dst, &img, 0, 0, 0));
    result("vpp-compose-separate-render-calls", process_composition(ctx, dst, src, 0) && pixels(dst, &img, 0, 0, 3));
    result("vpp-compose-pipeline-array", process_composition(ctx, dst, src, 1) && pixels(dst, &img, 0, 0, 3));
    for (unsigned rotation = VA_ROTATION_NONE; rotation <= VA_ROTATION_270; rotation++) {
        for (unsigned mirror = 0; mirror <= (VA_MIRROR_HORIZONTAL | VA_MIRROR_VERTICAL); mirror++) {
            char name[80]; snprintf(name, sizeof(name), "vpp-rotation-%u-mirror-%u", rotation, mirror);
            pipe.rotation_state = rotation; pipe.mirror_state = mirror;
            result(name, process(ctx, dst, &pipe) && pixels(dst, &img, rotation, mirror, 0));
        }
    }
    pipe.rotation_state = VA_ROTATION_NONE; pipe.mirror_state = VA_MIRROR_NONE;
    int planar_ok = surface_size(VA_RT_FORMAT_YUV420, VA_FOURCC_I420, VPP_SIZE, VPP_SIZE, &planar) && upload_nv12(src, &img, 0);
    result("vpp-nv12-to-i420", planar_ok && process(ctx, planar, &pipe) && pixels(planar, &img, 0, 0, 0));
    pipe.surface = planar;
    result("vpp-i420-to-nv12", planar_ok && process(ctx, dst, &pipe) && pixels(dst, &img, 0, 0, 0));
    static const struct { unsigned fcc; const char *name; } extra[] = {
        {VA_FOURCC_NV21, "nv21"}, {VA_FOURCC_YV12, "yv12"},
    };
    for (unsigned i = 0; i < 2; i++) {
        VASurfaceID converted = VA_INVALID_ID;
        char name[80];
        int ready = surface_size(VA_RT_FORMAT_YUV420, extra[i].fcc, VPP_SIZE, VPP_SIZE, &converted) && upload_nv12(src, &img, 0);
        pipe.surface = src;
        snprintf(name, sizeof(name), "vpp-nv12-to-%s", extra[i].name);
        result(name, ready && process(ctx, converted, &pipe) && pixels(converted, &img, 0, 0, 0));
        /* Supply an independent canonical upload for the reverse direction;
         * it must not inherit a failed forward conversion's chroma order. */
        int uploaded = ready && upload_nv12(converted, &img, 0);
        pipe.surface = converted;
        snprintf(name, sizeof(name), "vpp-%s-to-nv12", extra[i].name);
        result(name, uploaded && process(ctx, dst, &pipe) && pixels(dst, &img, 0, 0, 0));
        if (converted != VA_INVALID_ID) vaDestroySurfaces(display, &converted, 1);
    }
    int rgb_ok = surface_size(VA_RT_FORMAT_RGB32, VA_FOURCC_BGRX, VPP_SIZE, VPP_SIZE, &rgb) && image(VA_FOURCC_BGRX, VPP_SIZE, VPP_SIZE, &rgb_image);
    for (unsigned bt709 = 0; bt709 < 2; bt709++) {
        char name[80];
        VAProcColorStandardType standard = bt709 ? VAProcColorStandardBT709 : VAProcColorStandardBT601;
        pipe.surface = src; pipe.surface_color_standard = standard; pipe.output_color_standard = VAProcColorStandardNone;
        pipe.input_color_properties.color_range = VA_SOURCE_RANGE_REDUCED;
        pipe.output_color_properties.color_range = VA_SOURCE_RANGE_FULL;
        snprintf(name, sizeof(name), "vpp-nv12-to-bgrx-bt%u", bt709 ? 709 : 601);
        result(name, rgb_ok && flat_upload(src, &img, 0) && process(ctx, rgb, &pipe) && flat_pixels(rgb, &rgb_image, 1, bt709));
        pipe.surface = rgb; pipe.surface_color_standard = VAProcColorStandardNone; pipe.output_color_standard = standard;
        pipe.input_color_properties.color_range = VA_SOURCE_RANGE_FULL;
        pipe.output_color_properties.color_range = VA_SOURCE_RANGE_REDUCED;
        snprintf(name, sizeof(name), "vpp-bgrx-to-nv12-bt%u", bt709 ? 709 : 601);
        result(name, rgb_ok && flat_upload(rgb, &rgb_image, 1) && process(ctx, dst, &pipe) && flat_pixels(dst, &img, 0, bt709));
    }

done:
    if (ctx != VA_INVALID_ID) vaDestroyContext(display, ctx);
    if (config != VA_INVALID_ID) vaDestroyConfig(display, config);
    if (img.image_id != VA_INVALID_ID) vaDestroyImage(display, img.image_id);
    if (rgb_image.image_id != VA_INVALID_ID) vaDestroyImage(display, rgb_image.image_id);
    if (rgb != VA_INVALID_ID) vaDestroySurfaces(display, &rgb, 1);
    if (planar != VA_INVALID_ID) vaDestroySurfaces(display, &planar, 1);
    if (src != VA_INVALID_ID) vaDestroySurfaces(display, &src, 1);
    if (dst != VA_INVALID_ID) vaDestroySurfaces(display, &dst, 1);
}
static void packed422_pair(unsigned fcc, unsigned x, unsigned y, uint8_t values[4])
{
    uint8_t y0 = sample(0, x * 2, y), y1 = sample(0, x * 2 + 1, y);
    uint8_t u = sample(1, x, y), v = sample(2, x, y);
    if (fcc == VA_FOURCC_UYVY) { values[0] = u; values[1] = y0; values[2] = v; values[3] = y1; }
    else if (fcc == VA_FOURCC_VYUY) { values[0] = v; values[1] = y0; values[2] = u; values[3] = y1; }
    else if (fcc == VA_FOURCC_YVYU) { values[0] = y0; values[1] = v; values[2] = y1; values[3] = u; }
    else { values[0] = y0; values[1] = u; values[2] = y1; values[3] = v; }
}
static uint8_t *planar422_component(const VAImage *img, uint8_t *data, unsigned plane, unsigned x, unsigned y)
{
    unsigned index = img->format.fourcc == VA_FOURCC_YV16 && plane ? 3 - plane : plane;
    return data + img->offsets[index] + (size_t)y * img->pitches[index] + x;
}
static int packed422_upload(VASurfaceID sid, VAImage *img)
{
    uint8_t *p;
    if (!status(vaMapBuffer(display, img->buf, (void **)&p), "map packed422 source")) return 0;
    memset(p, 0xa5, img->data_size);
    if (img->format.fourcc == VA_FOURCC_422H || img->format.fourcc == VA_FOURCC_YV16) {
        for (unsigned plane = 0; plane < 3; plane++) for (unsigned y = 0; y < img->height; y++)
            for (unsigned x = 0; x < (plane ? img->width / 2u : img->width); x++)
                *planar422_component(img, p, plane, x, y) = sample(plane, x, y);
    } else {
        for (unsigned y = 0; y < img->height; y++) for (unsigned x = 0; x < img->width / 2; x++)
            packed422_pair(img->format.fourcc, x, y, p + img->offsets[0] + (size_t)y * img->pitches[0] + x * 4);
    }
    return status(vaUnmapBuffer(display, img->buf), "unmap packed422 source") &&
           status(vaPutImage(display, sid, img->image_id, 0, 0, img->width, img->height, 0, 0, img->width, img->height), "upload packed422 source");
}
static int packed422_pixels(VASurfaceID sid, VAImage *img)
{
    uint8_t *p;
    if (!status(vaGetImage(display, sid, 0, 0, img->width, img->height, img->image_id), "packed422 GetImage") ||
        !status(vaMapBuffer(display, img->buf, (void **)&p), "map packed422 output")) return 0;
    unsigned bad = 0;
    if (img->format.fourcc == VA_FOURCC_422H || img->format.fourcc == VA_FOURCC_YV16) {
        for (unsigned plane = 0; plane < 3; plane++) for (unsigned y = 0; y < img->height; y++)
            for (unsigned x = 0; x < (plane ? img->width / 2u : img->width); x++)
                bad += *planar422_component(img, p, plane, x, y) != sample(plane, x, y);
    } else {
        for (unsigned y = 0; y < img->height; y++) for (unsigned x = 0; x < img->width / 2; x++) {
            uint8_t values[4]; packed422_pair(img->format.fourcc, x, y, values);
            bad += memcmp(p + img->offsets[0] + (size_t)y * img->pitches[0] + x * 4, values, 4) != 0;
        }
    }
    if (bad) {
        save_image_failure(img, p);
        snprintf(detail, sizeof(detail), "packed422 pairs differ=%u artifact=failure-%u", bad, passed + failed + 1);
    }
    return status(vaUnmapBuffer(display, img->buf), "unmap packed422 output") && !bad;
}
static void packed422_checks(void)
{
    const unsigned fcc[] = {VA_FOURCC_YUY2, VA_FOURCC_UYVY, VA_FOURCC_VYUY, VA_FOURCC_YVYU, VA_FOURCC_422H, VA_FOURCC_YV16};
    const char *names[] = {"yuy2", "uyvy", "vyuy", "yvyu", "422h", "yv16"};
    VASurfaceID src[6], dst[6]; VAImage imgs[6];
    memset(imgs, 0, sizeof(imgs));
    for (unsigned i = 0; i < 6; i++) { src[i] = dst[i] = VA_INVALID_ID; imgs[i].image_id = VA_INVALID_ID; }
    VAConfigID config = VA_INVALID_ID; VAContextID ctx = VA_INVALID_ID;
    VAConfigAttrib rt = {.type = VAConfigAttribRTFormat, .value = VA_RT_FORMAT_YUV422};
    int ready = status(vaCreateConfig(display, VAProfileNone, VAEntrypointVideoProc, &rt, 1, &config), "packed422 VPP config");
    for (unsigned i = 0; i < 6 && ready; i++)
        ready = surface_size(rt.value, fcc[i], VPP_SIZE, VPP_SIZE, &src[i]) &&
                surface_size(rt.value, fcc[i], VPP_SIZE, VPP_SIZE, &dst[i]) && image(fcc[i], VPP_SIZE, VPP_SIZE, &imgs[i]);
    if (ready) ready = status(vaCreateContext(display, config, VPP_SIZE, VPP_SIZE, VA_PROGRESSIVE, dst, 6, &ctx), "packed422 VPP context");
    for (unsigned from = 0; from < 6; from++) for (unsigned to = 0; to < 6; to++) {
        char name[80];
        if (from == to) snprintf(name, sizeof(name), "vpp-%s-identity", names[from]);
        else snprintf(name, sizeof(name), "vpp-%s-to-%s", names[from], names[to]);
        VAProcPipelineParameterBuffer pipe = {.surface = src[from]};
        result(name, ready && packed422_upload(src[from], &imgs[from]) &&
               process(ctx, dst[to], &pipe) && packed422_pixels(dst[to], &imgs[to]));
    }
    if (ctx != VA_INVALID_ID) vaDestroyContext(display, ctx);
    if (config != VA_INVALID_ID) vaDestroyConfig(display, config);
    for (unsigned i = 0; i < 6; i++) {
        if (imgs[i].image_id != VA_INVALID_ID) vaDestroyImage(display, imgs[i].image_id);
        if (src[i] != VA_INVALID_ID) vaDestroySurfaces(display, &src[i], 1);
        if (dst[i] != VA_INVALID_ID) vaDestroySurfaces(display, &dst[i], 1);
    }
}
static unsigned jpeg_luma_dc(const uint8_t *data, size_t size, unsigned width, unsigned height)
{
    if (size < 4 || data[0] != 255 || data[1] != 0xd8) return 0;
    size_t off = 2;
    unsigned dc = 0, geometry = 0;
    while (off + 4 <= size && data[off] == 255) {
        unsigned marker = data[off + 1], length = ((unsigned)data[off + 2] << 8) | data[off + 3];
        if (length < 2 || length > size - off - 2) return 0;
        if (marker == 0xda) return geometry ? dc : 0;
        if (marker == 0xc0 && length >= 8)
            geometry = (((unsigned)data[off + 5] << 8) | data[off + 6]) == height &&
                       (((unsigned)data[off + 7] << 8) | data[off + 8]) == width;
        if (marker == 0xdb) {
            size_t p = off + 4, end = off + 2 + length;
            while (p < end) {
                unsigned table = data[p++], bytes = table >> 4 ? 128 : 64;
                if (bytes > end - p) return 0;
                if (!(table & 15) && !(table >> 4)) dc = data[p];
                p += bytes;
            }
        }
        off += 2 + length;
    }
    return 0;
}
static void jpeg_checks(void)
{
    VAConfigID config = VA_INVALID_ID;
    VAContextID ctx = VA_INVALID_ID;
    VASurfaceID src = VA_INVALID_ID;
    VABufferID coded = VA_INVALID_ID;
    VAImage img = {.image_id = VA_INVALID_ID};
    int setup = status(vaCreateConfig(display, VAProfileJPEGBaseline, VAEntrypointEncPicture, NULL, 0, &config), "JPEG config") &&
        surface_size(VA_RT_FORMAT_YUV420, VA_FOURCC_NV12, 320, 240, &src) && image(VA_FOURCC_NV12, 320, 240, &img) && upload_nv12(src, &img, 0) &&
        status(vaCreateContext(display, config, 320, 240, VA_PROGRESSIVE, &src, 1, &ctx), "JPEG context") &&
        status(vaCreateBuffer(display, ctx, VAEncCodedBufferType, 262144, 1, NULL, &coded), "JPEG coded buffer");
    if (!setup) { result("jpeg-native-setup", 0); goto done; }
    unsigned quality[] = {5, 50, 95}, dc[3] = {0};
    for (unsigned i = 0; i < 3; i++) {
        VAEncPictureParameterBufferJPEG pic = {.reconstructed_picture = VA_INVALID_SURFACE,
            .picture_width = 320, .picture_height = 240, .coded_buf = coded,
            .pic_flags.bits = {.huffman = 1, .interleaved = 1}, .sample_bit_depth = 8,
            .num_scan = 1, .num_components = 3, .component_id = {1, 2, 3},
            .quantiser_table_selector = {0, 1, 1}, .quality = quality[i]};
        VABufferID params = VA_INVALID_ID;
        int ok = status(vaCreateBuffer(display, ctx, VAEncPictureParameterBufferType, sizeof(pic), 1, &pic, &params), "JPEG picture buffer");
        if (ok && status(vaBeginPicture(display, ctx, src), "JPEG BeginPicture")) {
            ok = status(vaRenderPicture(display, ctx, &params, 1), "JPEG RenderPicture");
            ok = status(vaEndPicture(display, ctx), "JPEG EndPicture") && ok;
        } else ok = 0;
        if (params != VA_INVALID_ID) vaDestroyBuffer(display, params);
        if (ok) {
            VACodedBufferSegment *segment = NULL;
            ok = status(vaSyncBuffer(display, coded, 0), "JPEG SyncBuffer") && status(vaSyncSurface2(display, src, 0), "JPEG SyncSurface2") &&
                status(vaMapBuffer(display, coded, (void **)&segment), "JPEG MapBuffer");
            if (ok) {
                dc[i] = segment && !segment->status && !segment->next ? jpeg_luma_dc(segment->buf, segment->size, 320, 240) : 0;
                ok = dc[i] != 0;
                char path[1024];
                int length = snprintf(path, sizeof(path), "%s.native-%u.jpg", report_path, quality[i]);
                FILE *output = length > 0 && (unsigned)length < sizeof(path) ? fopen(path, "wb") : NULL;
                if (output && segment && segment->buf) {
                    if (fwrite(segment->buf, 1, segment->size, output) != segment->size) ok = 0;
                    if (fclose(output)) ok = 0;
                } else { if (output) fclose(output); ok = 0; }
                ok = status(vaUnmapBuffer(display, coded), "JPEG UnmapBuffer") && ok;
            }
        }
        char name[80]; snprintf(name, sizeof(name), "jpeg-native-quality-%u", quality[i]); result(name, ok);
    }
    if (!(dc[0] > dc[1] && dc[1] > dc[2] && dc[2]))
        snprintf(detail, sizeof(detail), "JPEG DQT luma DC=%u,%u,%u", dc[0], dc[1], dc[2]);
    result("jpeg-native-quality-changes-dqt", dc[0] > dc[1] && dc[1] > dc[2] && dc[2]);
done:
    if (coded != VA_INVALID_ID) vaDestroyBuffer(display, coded);
    if (ctx != VA_INVALID_ID) vaDestroyContext(display, ctx);
    if (config != VA_INVALID_ID) vaDestroyConfig(display, config);
    if (img.image_id != VA_INVALID_ID) vaDestroyImage(display, img.image_id);
    if (src != VA_INVALID_ID) vaDestroySurfaces(display, &src, 1);
}

static int jpeg_layout_upload(VASurfaceID surface, VAImage *img, unsigned frame)
{
    uint8_t *data;
    if (!status(vaMapBuffer(display, img->buf, (void **)&data), "JPEG layout upload map")) return 0;
    memset(data, 0xa5, img->data_size);
    for (unsigned y = 0; y < img->height; y++) for (unsigned x = 0; x < img->width; x++)
        data[img->offsets[0] + (size_t)y * img->pitches[0] + x] = 16 + (x * 3 + y * 5 + frame * 17) % 200;
    for (unsigned y = 0; y < img->height / 2; y++) for (unsigned x = 0; x < img->width / 2; x++) {
        unsigned u = 43 + (x * 7 + frame * 11) % 71, v = 153 + (y * 3 + frame * 13) % 61;
        if (img->num_planes == 3) {
            unsigned up = img->format.fourcc == VA_FOURCC_YV12 ? 2 : 1, vp = 3 - up;
            data[img->offsets[up] + (size_t)y * img->pitches[up] + x] = u;
            data[img->offsets[vp] + (size_t)y * img->pitches[vp] + x] = v;
        } else {
            uint8_t *p = data + img->offsets[1] + (size_t)y * img->pitches[1] + x * 2;
            p[0] = img->format.fourcc == VA_FOURCC_NV21 ? v : u;
            p[1] = img->format.fourcc == VA_FOURCC_NV21 ? u : v;
        }
    }
    return status(vaUnmapBuffer(display, img->buf), "JPEG layout upload unmap") &&
        status(vaPutImage(display, surface, img->image_id, 0, 0, img->width, img->height, 0, 0, img->width, img->height), "JPEG layout upload");
}
static int jpeg_layout_encode(VAContextID ctx, VASurfaceID src, VABufferID coded, unsigned frame, const char *kind)
{
    VAEncPictureParameterBufferJPEG pic = {.reconstructed_picture = VA_INVALID_SURFACE,
        .picture_width = 320, .picture_height = 240, .coded_buf = coded,
        .pic_flags.bits = {.huffman = 1, .interleaved = 1}, .sample_bit_depth = 8,
        .num_scan = 1, .num_components = 3, .component_id = {1, 2, 3},
        .quantiser_table_selector = {0, 1, 1}, .quality = 50};
    VABufferID params = VA_INVALID_ID;
    int ok = status(vaCreateBuffer(display, ctx, VAEncPictureParameterBufferType, sizeof(pic), 1, &pic, &params), "JPEG layout picture");
    if (ok && status(vaBeginPicture(display, ctx, src), "JPEG layout BeginPicture")) {
        ok = status(vaRenderPicture(display, ctx, &params, 1), "JPEG layout RenderPicture");
        ok = status(vaEndPicture(display, ctx), "JPEG layout EndPicture") && ok;
    } else ok = 0;
    if (params != VA_INVALID_ID) vaDestroyBuffer(display, params);
    VACodedBufferSegment *segment = NULL;
    if (!ok || !status(vaSyncBuffer(display, coded, 0), "JPEG layout SyncBuffer") ||
        !status(vaMapBuffer(display, coded, (void **)&segment), "JPEG layout coded map")) return 0;
    ok = segment && !segment->status && !segment->next && jpeg_luma_dc(segment->buf, segment->size, 320, 240);
    char path[1024]; int n = snprintf(path, sizeof(path), "%s.layout-%u-%s.jpg", report_path, frame, kind);
    FILE *file = ok && n > 0 && (unsigned)n < sizeof(path) ? fopen(path, "wb") : NULL;
    if (file) {
        if (fwrite(segment->buf, 1, segment->size, file) != segment->size) ok = 0;
        if (fclose(file)) ok = 0;
    } else ok = 0;
    return status(vaUnmapBuffer(display, coded), "JPEG layout coded unmap") && ok;
}
static void jpeg_layout_checks(void)
{
    VAConfigID config = VA_INVALID_ID; VAContextID ctx = VA_INVALID_ID; VABufferID coded = VA_INVALID_ID;
    VASurfaceID surfaces[4]; VAImage images[4]; memset(images, 0, sizeof(images));
    const unsigned formats[] = {VA_FOURCC_NV12, VA_FOURCC_I420, VA_FOURCC_YV12, VA_FOURCC_NV21};
    const char *names[] = {"NV12", "I420", "YV12", "NV21"};
    const unsigned order[] = {0, 1, 2, 3, 3, 2, 1, 0};
    int reference_ok[8] = {0};
    for (unsigned i = 0; i < 4; i++) { surfaces[i] = VA_INVALID_ID; images[i].image_id = VA_INVALID_ID; }
    int setup = status(vaCreateConfig(display, VAProfileJPEGBaseline, VAEntrypointEncPicture, NULL, 0, &config), "JPEG420 config");
    for (unsigned i = 0; i < 4 && setup; i++)
        setup = surface_size(VA_RT_FORMAT_YUV420, formats[i], 320, 240, &surfaces[i]) && image(formats[i], 320, 240, &images[i]);
    if (setup) setup = status(vaCreateContext(display, config, 320, 240, VA_PROGRESSIVE, surfaces, 4, &ctx), "JPEG420 context") &&
        status(vaCreateBuffer(display, ctx, VAEncCodedBufferType, 262144, 1, NULL, &coded), "JPEG420 coded buffer");
    for (unsigned i = 0; i < 8 && setup; i++)
        reference_ok[i] = jpeg_layout_upload(surfaces[0], &images[0], i) && jpeg_layout_encode(ctx, surfaces[0], coded, i, "ref");
    for (unsigned i = 0; i < 8; i++) {
        char name[80]; snprintf(name, sizeof(name), "jpeg-420-layout-%u-%s", i, names[order[i]]);
        result(name, setup && reference_ok[i] && jpeg_layout_upload(surfaces[order[i]], &images[order[i]], i) &&
               jpeg_layout_encode(ctx, surfaces[order[i]], coded, i, "test"));
    }
    if (coded != VA_INVALID_ID) vaDestroyBuffer(display, coded);
    if (ctx != VA_INVALID_ID) vaDestroyContext(display, ctx);
    if (config != VA_INVALID_ID) vaDestroyConfig(display, config);
    for (unsigned i = 0; i < 4; i++) {
        if (images[i].image_id != VA_INVALID_ID) vaDestroyImage(display, images[i].image_id);
        if (surfaces[i] != VA_INVALID_ID) vaDestroySurfaces(display, &surfaces[i], 1);
    }
}

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: %s RENDER_NODE RESULTS_TSV\n", argv[0]); return 2; }
    report_path = argv[2];
    results = fopen(report_path, "w");
    if (!results) { perror(argv[2]); return 2; }
    int fd = open(argv[1], O_RDWR | O_CLOEXEC), major, minor;
    if (fd < 0) { perror(argv[1]); fclose(results); return 2; }
    display = vaGetDisplayDRM(fd);
    if (!display || !status(vaInitialize(display, &major, &minor), "vaInitialize")) { close(fd); fclose(results); return 2; }
    const char *vendor = vaQueryVendorString(display);
    if (!vav4l2_test_driver_identity(vendor)) {
        fprintf(stderr, "Unexpected VA driver: %s\n", vendor ? vendor : "unknown");
        vaTerminate(display); close(fd); fclose(results); return 2;
    }
    static const struct { unsigned rt, fcc; const char *name; } formats[] = {
        {VA_RT_FORMAT_YUV420, VA_FOURCC_NV12, "NV12"}, {VA_RT_FORMAT_YUV420, VA_FOURCC_I420, "I420"},
        {VA_RT_FORMAT_YUV420, VA_FOURCC_NV21, "NV21"}, {VA_RT_FORMAT_YUV420, VA_FOURCC_YV12, "YV12"},
        {VA_RT_FORMAT_YUV420_10, VA_FOURCC_P010, "P010"}, {VA_RT_FORMAT_YUV422, VA_FOURCC_YUY2, "YUY2"}, {VA_RT_FORMAT_YUV422, VA_FOURCC_UYVY, "UYVY"},
        {VA_RT_FORMAT_YUV422, VA_FOURCC_VYUY, "VYUY"}, {VA_RT_FORMAT_YUV422, VA_FOURCC_YVYU, "YVYU"},
        {VA_RT_FORMAT_YUV422, VA_FOURCC_422H, "422H"}, {VA_RT_FORMAT_YUV422, VA_FOURCC_YV16, "YV16"},
        {VA_RT_FORMAT_YUV422_10, VA_FOURCC_Y210, "Y210"}, {VA_RT_FORMAT_RGB32, VA_FOURCC_BGRX, "BGRX"},
        {VA_RT_FORMAT_RGB32, VA_FOURCC_BGRA, "BGRA"}, {VA_RT_FORMAT_RGB32, VA_FOURCC_ARGB, "ARGB"},
    };
    for (unsigned i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
        char name[80]; snprintf(name, sizeof(name), "image-transfer-%s", formats[i].name);
        result(name, transfer(formats[i].rt, formats[i].fcc));
    }
    vpp_checks();
    packed422_checks();
    dmabuf_import_checks(fd);
    dmabuf_layout_checks(fd);
    dmabuf_direct_checks();
    dmabuf_multiplanar_checks(fd);
    jpeg_checks();
    jpeg_layout_checks();
    vaTerminate(display); close(fd);
    fprintf(results, "COMPLETE\t1\n"); fclose(results);
    printf("PASS=%u FAIL=%u COMPLETE=1\n", passed, failed);
    return failed ? 1 : 0;
}
