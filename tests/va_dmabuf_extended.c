/* Reuse the public API setup helpers, without invoking legacy feature/JPEG
 * checks. No driver-private symbols or processing fallback are used here. */
#define main legacy_feature_main
#include "va_feature_client.c"
#undef main
#include <poll.h>
#include <time.h>

static int extended_error(const char *stage)
{
    snprintf(detail, sizeof(detail), "%s: %s", stage, strerror(errno)); perror(stage); return 0;
}

static int extended_sync(int fd, uint64_t flags)
{
    struct timespec start, now, pause = {.tv_nsec = 1000000};
    if (clock_gettime(CLOCK_MONOTONIC, &start)) return extended_error("extended clock");
    if (!(flags & DMA_BUF_SYNC_END)) {
        struct pollfd p = {.fd = fd, .events = flags & DMA_BUF_SYNC_WRITE ? POLLOUT : POLLIN};
        int ret;
        do {
            if (clock_gettime(CLOCK_MONOTONIC, &now)) return extended_error("extended clock");
            int64_t left = 2000000000 - ((int64_t)(now.tv_sec - start.tv_sec) * 1000000000 + now.tv_nsec - start.tv_nsec);
            if (left <= 0) { errno = ETIMEDOUT; return extended_error("extended reservation wait"); }
            ret = poll(&p, 1, (left + 999999) / 1000000);
        } while (ret < 0 && errno == EINTR);
        if (ret <= 0 || !(p.revents & p.events) || p.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            if (ret >= 0) errno = ret ? EIO : ETIMEDOUT;
            return extended_error("extended reservation wait");
        }
    }
    struct dma_buf_sync sync = {.flags = flags};
    while (ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) < 0) {
        if ((errno != EINTR && errno != EAGAIN) || clock_gettime(CLOCK_MONOTONIC, &now) ||
            (int64_t)(now.tv_sec - start.tv_sec) * 1000000000 + now.tv_nsec - start.tv_nsec >= 2000000000) return extended_error("extended DMA sync");
        nanosleep(&pause, NULL);
    }
    return 1;
}
static struct dmabuf_format extended_format(unsigned va)
{
    for (unsigned i = 0; i < sizeof(dmabuf_formats) / sizeof(dmabuf_formats[0]); i++) if (dmabuf_formats[i].va == va) {
        struct dmabuf_format f = dmabuf_formats[i];
        if (va == VA_FOURCC_422H || va == VA_FOURCC_YV16) f.chroma_rows = 128;
        return f;
    }
    return (struct dmabuf_format){0};
}
/* Plane values describe Y/Cb/Cr semantics, independently of physical order. */
static uint8_t extended_byte(const struct dmabuf_format *f, unsigned p, unsigned y, unsigned x, unsigned frame)
{
    if (f->rt == VA_RT_FORMAT_RGB32) {
        unsigned component = f->va == VA_FOURCC_ARGB ? x % 4 : x % 4 == 3 ? 0 : 3 - x % 4;
        return component ? 19 + (component - 1) * 53 + y * 3 + (x / 4) * 7 + frame * 23 : 255;
    }
    unsigned component = p && (f->va == VA_FOURCC_YV12 || f->va == VA_FOURCC_YV16) ? 3 - p : p;
    return 19 + component * 71 + y * 3 + x * 7 + frame * 23;
}
static uint8_t extended_value(const struct dmabuf_format *f, unsigned p, unsigned y, unsigned x, unsigned frame,
                              const VARectangle *region, unsigned background)
{
    if (region) {
        unsigned sx = p && f->planes == 3 ? 2 : 1, sy = p && f->chroma_rows * 2 == dmabuf_dimension(f) ? 2 : 1;
        if (x < (unsigned)region->x * f->bytes / sx || x >= ((unsigned)region->x + region->width) * f->bytes / sx ||
            y < (unsigned)region->y / sy || y >= ((unsigned)region->y + region->height) / sy) frame = background;
    }
    return extended_byte(f, p, y, x, frame);
}
static int extended_pixels(uint8_t *data, unsigned size, const unsigned *offset, const unsigned *pitch,
                           const struct dmabuf_format *f, unsigned frame, int write, const VARectangle *region, unsigned background, int padding)
{
    int ok = 1; uint8_t *visible = !write && region && padding ? calloc(size, 1) : NULL;
    if (!write && region && padding && !visible) return 0;
    if (write) memset(data, 0xcd, size);
    for (unsigned p = 0; p < f->planes; p++) {
        unsigned row = dmabuf_row(f, p), rows = dmabuf_rows(f, p);
        if (pitch[p] < row || (uint64_t)offset[p] + (uint64_t)(rows - 1) * pitch[p] + row > size) { ok = 0; break; }
        for (unsigned y = 0; y < rows; y++) for (unsigned x = 0; x < row; x++) {
            unsigned at = offset[p] + y * pitch[p] + x;
            uint8_t expected = extended_value(f, p, y, x, frame, region, background);
            if (visible) visible[at] = 1;
            if (write) data[at] = expected;
            else if (data[at] != expected) {
                if (ok) snprintf(detail, sizeof(detail), "%s frame=%u plane=%u x=%u y=%u wanted=%u got=%u", f->name, frame, p, x, y, expected, data[at]);
                ok = 0;
            }
        }
    }
    if (visible) for (unsigned at = 0; at < size; at++) if (!visible[at] && data[at] != 0xcd) {
        if (ok) snprintf(detail, sizeof(detail), "%s compose padding offset=%u wanted=205 got=%u", f->name, at, data[at]);
        ok = 0;
    }
    free(visible); return ok;
}
static int extended_object(const VADRMPRIMESurfaceDescriptor *d, const struct dmabuf_format *f, unsigned frame, int write,
                           const VARectangle *region, unsigned background)
{
    if (d->fourcc != f->va || d->num_objects != 1 || (d->num_layers != 1 && d->num_layers != f->planes)) return 0;
    unsigned offset[3], pitch[3];
    for (unsigned p = 0; p < f->planes; p++) {
        unsigned layer = d->num_layers == 1 ? 0 : p, plane = d->num_layers == 1 ? p : 0;
        if (d->layers[layer].object_index[plane]) return 0;
        offset[p] = d->layers[layer].offset[plane]; pitch[p] = d->layers[layer].pitch[plane];
    }
    int fd = d->objects[0].fd; uint64_t access = write ? DMA_BUF_SYNC_WRITE : DMA_BUF_SYNC_READ;
    if (!extended_sync(fd, DMA_BUF_SYNC_START | access)) return 0;
    uint8_t *map = mmap(NULL, d->objects[0].size, PROT_READ | (write ? PROT_WRITE : 0), MAP_SHARED, fd, 0);
    int ok = map != MAP_FAILED;
    if (ok) {
        ok = extended_pixels(map, d->objects[0].size, offset, pitch, f, frame, write, region, background, 1);
        if (munmap(map, d->objects[0].size)) ok = extended_error("extended object unmap");
    } else extended_error("extended object map");
    return extended_sync(fd, DMA_BUF_SYNC_END | access) && ok;
}
static int extended_image(VASurfaceID sid, VAImage *im, const struct dmabuf_format *f, unsigned frame, int write,
                          const VARectangle *region, unsigned background)
{
    uint8_t *map = NULL;
    if (!write && !status(vaGetImage(display, sid, 0, 0, im->width, im->height, im->image_id), "extended GetImage")) return 0;
    if (!status(vaMapBuffer(display, im->buf, (void **)&map), "extended image map")) return 0;
    /* Image API padding is independent of the exported object's padding. */
    int ok = extended_pixels(map, im->data_size, im->offsets, im->pitches, f, frame, write, region, background, 0);
    ok = status(vaUnmapBuffer(display, im->buf), "extended image unmap") && ok;
    if (ok && write) ok = status(vaPutImage(display, sid, im->image_id, 0, 0, im->width, im->height, 0, 0, im->width, im->height), "extended PutImage");
    return ok;
}
static int extended_export(VASurfaceID sid, VADRMPRIMESurfaceDescriptor *d, unsigned layers)
{
    return status(vaExportSurfaceHandle(display, sid, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, VA_EXPORT_SURFACE_READ_WRITE | layers, d), "extended export");
}
static int extended_import(const struct dmabuf_format *f, VADRMPRIMESurfaceDescriptor *d, VASurfaceID *sid)
{
    VASurfaceAttrib a[] = {
        {.type = VASurfaceAttribMemoryType, .flags = VA_SURFACE_ATTRIB_SETTABLE, .value = {.type = VAGenericValueTypeInteger, .value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2}},
        {.type = VASurfaceAttribExternalBufferDescriptor, .flags = VA_SURFACE_ATTRIB_SETTABLE, .value = {.type = VAGenericValueTypePointer, .value.p = d}}};
    return status(vaCreateSurfaces(display, f->rt, d->width, d->height, sid, 1, a, 2), "extended import");
}
static int extended_context(VAConfigID cfg, unsigned size, VAContextID *ctx)
{
    return status(vaCreateContext(display, cfg, size, size, VA_PROGRESSIVE, NULL, 0, ctx), "extended context");
}
static VAStatus extended_submit(VAContextID ctx, VASurfaceID dst, VAProcPipelineParameterBuffer *pipe)
{
    VABufferID id = VA_INVALID_ID;
    VAStatus st = vaCreateBuffer(display, ctx, VAProcPipelineParameterBufferType, sizeof(*pipe), 1, pipe, &id);
    if (st == VA_STATUS_SUCCESS) st = vaBeginPicture(display, ctx, dst);
    if (st == VA_STATUS_SUCCESS) { st = vaRenderPicture(display, ctx, &id, 1); VAStatus end = vaEndPicture(display, ctx); if (!st) st = end; }
    if (id != VA_INVALID_ID) vaDestroyBuffer(display, id);
    return st;
}
static int extended_alpha(VADRMPRIMESurfaceDescriptor *d, const struct dmabuf_format *f, unsigned value)
{
    int fd = d->objects[0].fd;
    if (!extended_sync(fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE)) return 0;
    uint8_t *map = mmap(NULL, d->objects[0].size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    int ok = map != MAP_FAILED;
    if (ok) { map[d->layers[0].offset[0] + (f->va == VA_FOURCC_ARGB ? 0 : 3)] = value; munmap(map, d->objects[0].size); }
    return extended_sync(fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE) && ok;
}
static void extended_checks(unsigned fourcc)
{
    struct dmabuf_format f = extended_format(fourcc); unsigned size = dmabuf_dimension(&f);
    VASurfaceID src = VA_INVALID_ID, dst = VA_INVALID_ID, alias = VA_INVALID_ID;
    VAConfigID cfg = VA_INVALID_ID; VAContextID ctx = VA_INVALID_ID;
    VAImage im = {.image_id = VA_INVALID_ID}; VADRMPRIMESurfaceDescriptor d[2] = {{0}}, separate = {0};
    VAConfigAttrib rt = {.type = VAConfigAttribRTFormat, .value = f.rt};
    int setup = surface_size(f.rt, f.va, size, size, &src) && surface_size(f.rt, f.va, size, size, &dst) && image(f.va, size, size, &im) &&
        extended_image(src, &im, &f, 0, 1, NULL, 0) && extended_image(dst, &im, &f, 113, 1, NULL, 0) &&
        extended_export(src, &d[0], VA_EXPORT_SURFACE_COMPOSED_LAYERS) && extended_export(dst, &d[1], VA_EXPORT_SURFACE_COMPOSED_LAYERS) &&
        extended_import(&f, &d[1], &alias) && status(vaCreateConfig(display, VAProfileNone, VAEntrypointVideoProc, &rt, 1, &cfg), "extended config") &&
        extended_context(cfg, size, &ctx);
    int layout = setup && extended_export(src, &separate, VA_EXPORT_SURFACE_SEPARATE_LAYERS) && extended_object(&separate, &f, 0, 0, NULL, 0);
    if (layout && (f.va == VA_FOURCC_YV12 || f.va == VA_FOURCC_YV16))
        layout = d[0].layers[0].offset[2] == d[0].layers[0].pitch[0] * size && d[0].layers[0].offset[1] > d[0].layers[0].offset[2];
    char tag[128]; snprintf(tag, sizeof(tag), "dmabuf-extended-layout-%s", f.name); result(tag, layout); dmabuf_close(&separate);
    VAProcPipelineParameterBuffer pipe = {.surface = src}; int warm = setup;
    for (unsigned frame = 0; frame < 32 && warm; frame++) warm = extended_object(&d[0], &f, frame, 1, NULL, 0) &&
        extended_object(&d[1], &f, 113, 1, NULL, 0) && process(ctx, alias, &pipe) && extended_object(&d[0], &f, frame, 0, NULL, 0) &&
        extended_object(&d[1], &f, frame, 0, NULL, 0) && extended_image(alias, &im, &f, frame, 0, NULL, 0) && extended_image(dst, &im, &f, frame, 0, NULL, 0);
    if (dst != VA_INVALID_ID) { warm = status(vaDestroySurfaces(display, &dst, 1), "extended exporter destroy") && warm; dst = VA_INVALID_ID; }
    warm = warm && extended_object(&d[0], &f, 6, 1, NULL, 0) && extended_object(&d[1], &f, 113, 1, NULL, 0) &&
        process(ctx, alias, &pipe) && extended_object(&d[1], &f, 6, 0, NULL, 0);
    pipe.surface = alias;
    warm = warm && extended_object(&d[0], &f, 7, 1, NULL, 0) && process(ctx, src, &pipe) && extended_object(&d[0], &f, 6, 0, NULL, 0);
    snprintf(tag, sizeof(tag), "dmabuf-extended-warm-%s", f.name); result(tag, warm); pipe.surface = src;
    int cold = setup;
    for (unsigned frame = 37; frame < 45 && cold; frame++) {
        VAContextID fresh = VA_INVALID_ID;
        cold = extended_context(cfg, size, &fresh) && extended_object(&d[0], &f, frame, 1, NULL, 0) && extended_object(&d[1], &f, 113, 1, NULL, 0) &&
            process(fresh, alias, &pipe) && extended_object(&d[0], &f, frame, 0, NULL, 0) && extended_object(&d[1], &f, frame, 0, NULL, 0) &&
            extended_image(alias, &im, &f, frame, 0, NULL, 0);
        if (fresh != VA_INVALID_ID) vaDestroyContext(display, fresh);
    }
    snprintf(tag, sizeof(tag), "dmabuf-extended-cold-%s", f.name); result(tag, cold);
    VARectangle region = {12, 12, 40, 40}; pipe.surface_region = pipe.output_region = &region;
    int compose = setup && extended_object(&d[0], &f, 55, 1, NULL, 0) && extended_object(&d[1], &f, 113, 1, NULL, 0) &&
        process(ctx, alias, &pipe) && extended_object(&d[0], &f, 55, 0, NULL, 0) && extended_object(&d[1], &f, 55, 0, &region, 113) &&
        extended_image(alias, &im, &f, 55, 0, &region, 113);
    snprintf(tag, sizeof(tag), "dmabuf-extended-compose-%s", f.name); result(tag, compose); pipe.surface_region = pipe.output_region = NULL;
    if (f.rt == VA_RT_FORMAT_RGB32) {
        int alpha = setup && extended_object(&d[0], &f, 45, 1, NULL, 0) && extended_object(&d[1], &f, 113, 1, NULL, 0) && extended_alpha(&d[0], &f, 127);
        setenv("VAV4L2_VPP_DMABUF", "auto", 1);
        alpha = alpha && extended_submit(ctx, alias, &pipe) == VA_STATUS_ERROR_UNIMPLEMENTED && extended_object(&d[1], &f, 113, 0, NULL, 0);
        setenv("VAV4L2_VPP_DMABUF", "required", 1);
        alpha = alpha && extended_alpha(&d[0], &f, 255) && process(ctx, alias, &pipe) && extended_object(&d[1], &f, 45, 0, NULL, 0);
        snprintf(tag, sizeof(tag), "dmabuf-extended-alpha-%s", f.name); result(tag, alpha);
    }
    if (ctx != VA_INVALID_ID) vaDestroyContext(display, ctx);
    if (cfg != VA_INVALID_ID) vaDestroyConfig(display, cfg);
    if (im.image_id != VA_INVALID_ID) vaDestroyImage(display, im.image_id);
    if (src != VA_INVALID_ID) vaDestroySurfaces(display, &src, 1);
    if (alias != VA_INVALID_ID) vaDestroySurfaces(display, &alias, 1);
    dmabuf_close(&d[0]); dmabuf_close(&d[1]);
}
static void extended_cross(unsigned source, unsigned target)
{
    struct dmabuf_format f[2] = {extended_format(source), extended_format(target)};
    VASurfaceID sid[2] = {VA_INVALID_ID, VA_INVALID_ID}; VADRMPRIMESurfaceDescriptor d[2] = {{0}};
    VAImage im[2] = {{.image_id = VA_INVALID_ID}, {.image_id = VA_INVALID_ID}};
    VAConfigID cfg = VA_INVALID_ID; VAContextID ctx = VA_INVALID_ID;
    VAConfigAttrib rt = {.type = VAConfigAttribRTFormat, .value = f[0].rt}; int ok = 1;
    for (unsigned i = 0; i < 2 && ok; i++) ok = surface_size(f[i].rt, f[i].va, dmabuf_dimension(&f[i]), dmabuf_dimension(&f[i]), &sid[i]) &&
        image(f[i].va, dmabuf_dimension(&f[i]), dmabuf_dimension(&f[i]), &im[i]) && extended_image(sid[i], &im[i], &f[i], 0, 1, NULL, 0) &&
        extended_export(sid[i], &d[i], VA_EXPORT_SURFACE_COMPOSED_LAYERS);
    ok = ok && status(vaCreateConfig(display, VAProfileNone, VAEntrypointVideoProc, &rt, 1, &cfg), "extended cross config") &&
        extended_context(cfg, dmabuf_dimension(&f[0]), &ctx);
    VAProcPipelineParameterBuffer pipe = {.surface = sid[0]};
    for (unsigned frame = 61; frame < 69 && ok; frame++) ok = extended_object(&d[0], &f[0], frame, 1, NULL, 0) && extended_object(&d[1], &f[1], 113, 1, NULL, 0) &&
        process(ctx, sid[1], &pipe) && extended_object(&d[0], &f[0], frame, 0, NULL, 0) && extended_object(&d[1], &f[1], frame, 0, NULL, 0) &&
        extended_image(sid[1], &im[1], &f[1], frame, 0, NULL, 0);
    char tag[128]; snprintf(tag, sizeof(tag), "dmabuf-extended-cross-%s-to-%s", f[0].name, f[1].name); result(tag, ok);
    if (ctx != VA_INVALID_ID) vaDestroyContext(display, ctx);
    if (cfg != VA_INVALID_ID) vaDestroyConfig(display, cfg);
    for (unsigned i = 0; i < 2; i++) { if (im[i].image_id != VA_INVALID_ID) vaDestroyImage(display, im[i].image_id); if (sid[i] != VA_INVALID_ID) vaDestroySurfaces(display, &sid[i], 1); dmabuf_close(&d[i]); }
}
int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: %s RENDER-NODE RESULTS.tsv\n", argv[0]); return 2; }
    results = fopen(argv[2], "w"); if (!results) { perror(argv[2]); return 2; }
    int fd = open(argv[1], O_RDWR | O_CLOEXEC), major, minor;
    if (fd < 0) { perror(argv[1]); fclose(results); return 2; }
    display = vaGetDisplayDRM(fd);
    if (!display || !status(vaInitialize(display, &major, &minor), "extended initialize")) { close(fd); fclose(results); return 2; }
    const char *vendor = vaQueryVendorString(display);
    if (!vav4l2_test_driver_identity(vendor)) { fprintf(stderr, "Unexpected VA driver\n"); vaTerminate(display); close(fd); fclose(results); return 2; }
    setenv("VAV4L2_VPP_DMABUF", "required", 1);
    const unsigned formats[] = {VA_FOURCC_YV12, VA_FOURCC_422H, VA_FOURCC_YV16, VA_FOURCC_ARGB, VA_FOURCC_BGRA};
    for (unsigned i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) extended_checks(formats[i]);
    const unsigned pairs[][2] = {{VA_FOURCC_YV12, VA_FOURCC_I420}, {VA_FOURCC_I420, VA_FOURCC_YV12}, {VA_FOURCC_422H, VA_FOURCC_YV16},
        {VA_FOURCC_YV16, VA_FOURCC_422H}, {VA_FOURCC_ARGB, VA_FOURCC_BGRA}, {VA_FOURCC_BGRA, VA_FOURCC_ARGB}};
    for (unsigned i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) extended_cross(pairs[i][0], pairs[i][1]);
    vaTerminate(display); close(fd); fprintf(results, "COMPLETE\t1\n");
    if (fclose(results)) return 2;
    printf("EXTENDED_DMA PASS=%u FAIL=%u COMPLETE=1 EXIT=%u\n", passed, failed, failed ? 1 : 0);
    return failed ? 1 : 0;
}
