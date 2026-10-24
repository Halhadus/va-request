/* Public LINEAR row-layout interoperability; processing uses explicit staging. */
#define main legacy_feature_main
#include "va_feature_client.c"
#undef main

static unsigned row_layer(const VADRMPRIMESurfaceDescriptor *d, unsigned p) { return d->num_layers == 1 ? 0 : p; }
static unsigned row_plane(const VADRMPRIMESurfaceDescriptor *d, unsigned p) { return d->num_layers == 1 ? p : 0; }
static unsigned char row_value(const struct dmabuf_format *f, unsigned p, unsigned y, unsigned x, unsigned frame, int partial)
{
    unsigned sx = p && f->planes == 3 ? 2 : 1, sy = p && f->chroma_rows * 2 == 64 ? 2 : 1;
    if (partial && (x < 16 * f->bytes / sx || x >= 48 * f->bytes / sx || y < 16 / sy || y >= 48 / sy)) frame = 3;
    return dmabuf_byte(f, p, y, x, frame);
}
static int row_object(const VADRMPRIMESurfaceDescriptor *d, const struct dmabuf_format *f, unsigned frame, int write, int partial)
{
    if (d->num_objects != 1) return 0;
    int fd = d->objects[0].fd; unsigned size = d->objects[0].size;
    uint64_t access = write ? DMA_BUF_SYNC_WRITE : DMA_BUF_SYNC_READ;
    if (!prime_sync(fd, DMA_BUF_SYNC_START | access)) return 0;
    unsigned char *map = mmap(NULL, size, PROT_READ | (write ? PROT_WRITE : 0), MAP_SHARED, fd, 0);
    unsigned char *visible = write ? NULL : calloc(size, 1);
    int ok = map != MAP_FAILED && (write || visible);
    if (ok) {
        if (write) memset(map, 0xcd, size);
        for (unsigned p = 0; p < f->planes; p++) {
            unsigned layer = row_layer(d, p), plane = row_plane(d, p), offset = d->layers[layer].offset[plane], pitch = d->layers[layer].pitch[plane];
            if (pitch < dmabuf_row(f, p) || (uint64_t)offset + (uint64_t)(dmabuf_rows(f, p) - 1) * pitch + dmabuf_row(f, p) > size) { ok = 0; break; }
            for (unsigned y = 0; y < dmabuf_rows(f, p); y++) for (unsigned x = 0; x < dmabuf_row(f, p); x++) {
                unsigned at = offset + y * pitch + x; unsigned char wanted = row_value(f, p, y, x, frame, partial);
                if (write) map[at] = wanted;
                else { visible[at] = 1; if (map[at] != wanted) { if (ok) snprintf(detail, sizeof(detail), "%s plane=%u x=%u y=%u wanted=%u got=%u", f->name, p, x, y, wanted, map[at]); ok = 0; } }
            }
        }
        if (!write) for (unsigned at = 0; at < size; at++) if (!visible[at] && map[at] != 0xcd) {
            if (ok) snprintf(detail, sizeof(detail), "%s row padding byte=%u modified", f->name, at);
            ok = 0;
        }
    }
    free(visible);
    if (map != MAP_FAILED && munmap(map, size)) { perror("row object unmap"); ok = 0; }
    return prime_sync(fd, DMA_BUF_SYNC_END | access) && ok;
}
static int row_image(VASurfaceID sid, VAImage *im, const struct dmabuf_format *f, unsigned frame, int write, int partial)
{
    unsigned char *map = NULL;
    if (!write && !status(vaGetImage(display, sid, 0, 0, 64, 64, im->image_id), "row GetImage")) return 0;
    if (!status(vaMapBuffer(display, im->buf, (void **)&map), "row image map")) return 0;
    int ok = 1;
    for (unsigned p = 0; p < f->planes; p++) for (unsigned y = 0; y < dmabuf_rows(f, p); y++) for (unsigned x = 0; x < dmabuf_row(f, p); x++) {
        unsigned char *at = map + im->offsets[p] + y * im->pitches[p] + x, wanted = row_value(f, p, y, x, frame, partial);
        if (write) *at = wanted; else if (*at != wanted) {
            if (ok) snprintf(detail, sizeof(detail), "%s GetImage plane=%u x=%u y=%u wanted=%u got=%u", f->name, p, x, y, wanted, *at);
            ok = 0;
        }
    }
    ok = status(vaUnmapBuffer(display, im->buf), "row image unmap") && ok;
    if (write && ok) ok = partial ? status(vaPutImage(display, sid, im->image_id, 16, 16, 32, 32, 16, 16, 32, 32), "row partial PutImage") :
        status(vaPutImage(display, sid, im->image_id, 0, 0, 64, 64, 0, 0, 64, 64), "row full PutImage");
    return ok;
}
static void row_check(const struct dmabuf_format *f, unsigned variant)
{
    VASurfaceID holder = VA_INVALID_ID, sid = VA_INVALID_ID, target = VA_INVALID_ID;
    VAConfigID cfg = VA_INVALID_ID; VAContextID ctx = VA_INVALID_ID; VAImage im = {.image_id = VA_INVALID_ID};
    VADRMPRIMESurfaceDescriptor backing = {0}, d = {.fourcc = f->va, .width = 64, .height = 64, .num_objects = 1,
        .num_layers = variant & 1 ? f->planes : 1}, retained = {0}; d.objects[0].fd = -1;
    /* A public BGRX allocation supplies enough LINEAR bytes for every layout;
     * its image interpretation is replaced only in the importing descriptor. */
    int ok = surface_size(VA_RT_FORMAT_RGB32, VA_FOURCC_BGRX, 64, 64, &holder) &&
        status(vaExportSurfaceHandle(display, holder, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, VA_EXPORT_SURFACE_READ_WRITE | VA_EXPORT_SURFACE_COMPOSED_LAYERS, &backing), "row backing export");
    if (ok) {
        d.objects[0] = backing.objects[0]; d.objects[0].fd = fcntl(backing.objects[0].fd, F_DUPFD_CLOEXEC, 0); ok = d.objects[0].fd >= 0;
        unsigned pitch = 0; for (unsigned p = 0; p < f->planes; p++) pitch += dmabuf_row(f, p);
        for (unsigned p = 0; p < f->planes; p++) {
            unsigned layer = row_layer(&d, p), plane = row_plane(&d, p), offset = 0;
            for (unsigned i = 0; i < f->planes; i++) if ((variant & 2) ? i > p : i < p) offset += dmabuf_row(f, i);
            d.layers[layer].drm_format = d.num_layers == 1 ? f->drm : f->va == VA_FOURCC_P010 ? (p ? DRM_FORMAT_GR1616 : DRM_FORMAT_R16) : p && f->planes == 2 ? DRM_FORMAT_GR88 : DRM_FORMAT_R8;
            d.layers[layer].num_planes = d.num_layers == 1 ? f->planes : 1;
            d.layers[layer].offset[plane] = offset; d.layers[layer].pitch[plane] = pitch;
        }
    }
    VASurfaceAttrib attrs[] = {
        {.type = VASurfaceAttribMemoryType, .flags = VA_SURFACE_ATTRIB_SETTABLE, .value = {.type = VAGenericValueTypeInteger, .value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2}},
        {.type = VASurfaceAttribExternalBufferDescriptor, .flags = VA_SURFACE_ATTRIB_SETTABLE, .value = {.type = VAGenericValueTypePointer, .value.p = &d}}};
    ok = ok && row_object(&d, f, 1, 1, 0) && status(vaCreateSurfaces(display, f->rt, 64, 64, &sid, 1, attrs, 2), "row layout import") && image(f->va, 64, 64, &im);
    if (holder != VA_INVALID_ID) { ok = status(vaDestroySurfaces(display, &holder, 1), "row exporter destroy") && ok; holder = VA_INVALID_ID; }
    dmabuf_close(&backing);
    ok = ok && row_image(sid, &im, f, 1, 0, 0) && row_object(&d, f, 2, 1, 0) && row_image(sid, &im, f, 2, 0, 0);
    if (ok && f->va != VA_FOURCC_P010) {
        VAConfigAttrib rt = {.type = VAConfigAttribRTFormat, .value = f->rt}; VAProcPipelineParameterBuffer pipe = {.surface = sid};
        ok = surface_size(f->rt, f->va, 64, 64, &target) && status(vaCreateConfig(display, VAProfileNone, VAEntrypointVideoProc, &rt, 1, &cfg), "row VPP config") &&
            status(vaCreateContext(display, cfg, 64, 64, VA_PROGRESSIVE, NULL, 0, &ctx), "row VPP context") && process(ctx, target, &pipe) && dmabuf_image(target, &im, f, 2, 0) && row_object(&d, f, 2, 0, 0);
    }
    ok = ok && row_image(sid, &im, f, 3, 1, 0) && row_object(&d, f, 3, 0, 0) && row_image(sid, &im, f, 4, 1, 1) && row_object(&d, f, 4, 0, 1);
    if (ok) {
        VADRMPRIMESurfaceDescriptor bad = d; unsigned layer = row_layer(&bad, 1), plane = row_plane(&bad, 1); VASurfaceID untouched = 1234;
        bad.layers[layer].offset[plane] = d.layers[0].offset[0] + dmabuf_row(f, 0) - 1;
        attrs[1].value.value.p = &bad;
        VAStatus st = vaCreateSurfaces(display, f->rt, 64, 64, &untouched, 1, attrs, 2);
        ok = st == VA_STATUS_ERROR_INVALID_PARAMETER && untouched == 1234;
        if (st == VA_STATUS_SUCCESS) vaDestroySurfaces(display, &untouched, 1);
    }
    ok = ok && status(vaExportSurfaceHandle(display, sid, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, VA_EXPORT_SURFACE_READ_WRITE | VA_EXPORT_SURFACE_COMPOSED_LAYERS, &retained), "row reexport");
    if (ok) for (unsigned p = 0; p < f->planes; p++) {
        unsigned layer = row_layer(&d, p), plane = row_plane(&d, p);
        ok = ok && retained.layers[0].offset[p] == d.layers[layer].offset[plane] && retained.layers[0].pitch[p] == d.layers[layer].pitch[plane];
    }
    dmabuf_close(&d);
    ok = ok && row_image(sid, &im, f, 4, 0, 1);
    if (ctx != VA_INVALID_ID) vaDestroyContext(display, ctx);
    if (cfg != VA_INVALID_ID) vaDestroyConfig(display, cfg);
    if (target != VA_INVALID_ID) vaDestroySurfaces(display, &target, 1);
    if (sid != VA_INVALID_ID) vaDestroySurfaces(display, &sid, 1);
    ok = ok && row_object(&retained, f, 4, 0, 1);
    dmabuf_close(&retained); if (im.image_id != VA_INVALID_ID) vaDestroyImage(display, im.image_id);
    char tag[128]; snprintf(tag, sizeof(tag), "dmabuf-rows-%s-%s-%s", f->name, variant & 1 ? "separate" : "composed", variant & 2 ? "reverse" : "forward"); result(tag, ok);
}
int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: %s RENDER-NODE RESULTS.tsv\n", argv[0]); return 2; }
    results = fopen(argv[2], "w"); if (!results) { perror(argv[2]); return 2; }
    int fd = open(argv[1], O_RDWR | O_CLOEXEC), major, minor;
    if (fd < 0) { perror(argv[1]); fclose(results); return 2; }
    display = vaGetDisplayDRM(fd);
    if (!display || !status(vaInitialize(display, &major, &minor), "row initialize")) { close(fd); fclose(results); return 2; }
    const char *vendor = vaQueryVendorString(display);
    if (!vav4l2_test_driver_identity(vendor)) { fprintf(stderr, "Unexpected VA driver\n"); vaTerminate(display); close(fd); fclose(results); return 2; }
    setenv("VAV4L2_VPP_DMABUF", "auto", 1);
    for (unsigned i = 0; i < sizeof(dmabuf_formats) / sizeof(dmabuf_formats[0]); i++) if (dmabuf_formats[i].planes > 1)
        for (unsigned variant = 0; variant < 4; variant++) row_check(&dmabuf_formats[i], variant);
    vaTerminate(display); close(fd); fprintf(results, "COMPLETE\t1\n"); if (fclose(results)) return 2;
    printf("DMA_ROWS PASS=%u FAIL=%u COMPLETE=1 EXIT=%u\n", passed, failed, failed ? 1 : 0); return failed ? 1 : 0;
}
