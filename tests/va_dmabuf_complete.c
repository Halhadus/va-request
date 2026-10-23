/* Public API closure gate. Existing matrices remain independent and unchanged. */
#define main legacy_feature_main
#include "va_feature_client.c"
#undef main
#include <dirent.h>
#include <stddef.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <limits.h>
#include <time.h>

static const struct dmabuf_format additions[] = {
    {VA_RT_FORMAT_RGB16, VA_FOURCC_RGB565, DRM_FORMAT_RGB565, 1, 2, 0, "RGB565"},
    {VA_RT_FORMAT_RGB32, VA_FOURCC_RGBA, DRM_FORMAT_ABGR8888, 1, 4, 0, "RGBA"},
    {VA_RT_FORMAT_RGB32, VA_FOURCC_RGBX, DRM_FORMAT_XBGR8888, 1, 4, 0, "RGBX"}};

static unsigned complete_byte(const struct dmabuf_format *f, unsigned p, unsigned y, unsigned x, unsigned frame)
{
    if (f->rt == VA_RT_FORMAT_RGB32 && x % 4 == (f->va == VA_FOURCC_ARGB ? 0u : 3u)) return 255;
    return (23 + p * 71 + y * 3 + x * 7 + frame * 19) & 255;
}
static int complete_image(VASurfaceID sid, VAImage *im, const struct dmabuf_format *f, unsigned frame, int write)
{
    unsigned char *map = NULL;
    if (!write && !status(vaGetImage(display, sid, 0, 0, im->width, im->height, im->image_id), "complete GetImage")) return 0;
    if (!status(vaMapBuffer(display, im->buf, (void **)&map), "complete MapBuffer")) return 0;
    int ok = 1;
    for (unsigned p = 0; p < f->planes; p++) {
        unsigned row = im->width * f->bytes / (p && f->planes == 3 ? 2 : 1);
        unsigned rows = p && f->va != VA_FOURCC_422H && f->va != VA_FOURCC_YV16 ? im->height / 2 : im->height;
        for (unsigned y = 0; y < rows; y++) for (unsigned x = 0; x < row; x++) {
            unsigned at = im->offsets[p] + y * im->pitches[p] + x, value = complete_byte(f, p, y, x, frame);
            if (write) map[at] = value;
            else if (!((f->va == VA_FOURCC_RGBX || f->va == VA_FOURCC_BGRX) && x % 4 == 3) && map[at] != value) {
                if (ok) snprintf(detail, sizeof(detail), "%s plane=%u x=%u y=%u got=%u expected=%u", f->name, p, x, y, map[at], value);
                ok = 0;
            }
        }
    }
    ok = status(vaUnmapBuffer(display, im->buf), "complete UnmapBuffer") && ok;
    return ok && (!write || status(vaPutImage(display, sid, im->image_id, 0, 0, im->width, im->height, 0, 0, im->width, im->height), "complete PutImage"));
}
static void complete_v3(const VADRMPRIMESurfaceDescriptor *d, VADRMPRIME3SurfaceDescriptor *v3)
{
    memset(v3, 0, sizeof(*v3)); memcpy(v3, d, offsetof(VADRMPRIME3SurfaceDescriptor, flags));
}
static VAStatus complete_import(const struct dmabuf_format *f, VADRMPRIME3SurfaceDescriptor *d, unsigned count, VASurfaceID *ids)
{
    VASurfaceAttrib a[] = {
        {.type = VASurfaceAttribMemoryType, .flags = VA_SURFACE_ATTRIB_SETTABLE, .value = {.type = VAGenericValueTypeInteger, .value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_3}},
        {.type = VASurfaceAttribExternalBufferDescriptor, .flags = VA_SURFACE_ATTRIB_SETTABLE, .value = {.type = VAGenericValueTypePointer, .value.p = d}}};
    return vaCreateSurfaces(display, f->rt, d->width, d->height, ids, count, a, 2);
}
/* A process-wide count cannot distinguish a leaked duplicate from another
 * FD closing, or a cache worker's short-lived file from a retained surface FD.
 * Require the original descriptor identities to return within a bounded wait;
 * never adopt a new baseline or allow a persistent extra descriptor. */
struct complete_fd { int fd; dev_t dev; ino_t ino; };
struct complete_fds { struct complete_fd *entries; size_t count; };
static int complete_fd_order(const void *a, const void *b)
{
    int x = ((const struct complete_fd *)a)->fd, y = ((const struct complete_fd *)b)->fd;
    return (x > y) - (x < y);
}
static int complete_fd_snapshot(struct complete_fds *out)
{
    *out = (struct complete_fds){0};
    DIR *dir = opendir("/proc/self/fd"); if (!dir) return 0;
    int own = dirfd(dir), ok = 1; struct dirent *e;
    if (own < 0) ok = 0;
    while (ok) {
        errno = 0; e = readdir(dir); if (!e) { ok = !errno; break; }
        char *end; long fd = strtol(e->d_name, &end, 10);
        if (end == e->d_name || *end || fd < 0 || fd > INT_MAX || fd == own) continue;
        struct stat st;
        if (fstat((int)fd, &st)) { if (errno == EBADF) continue; ok = 0; break; }
        void *entries = realloc(out->entries, (out->count + 1) * sizeof(*out->entries));
        if (!entries) { ok = 0; break; }
        out->entries = entries; out->entries[out->count++] = (struct complete_fd){(int)fd, st.st_dev, st.st_ino};
    }
    if (closedir(dir)) ok = 0;
    if (!ok) { free(out->entries); *out = (struct complete_fds){0}; return 0; }
    if (out->count) qsort(out->entries, out->count, sizeof(*out->entries), complete_fd_order);
    return 1;
}
static int complete_fd_same(const struct complete_fd *a, const struct complete_fd *b)
{
    return a->fd == b->fd && a->dev == b->dev && a->ino == b->ino;
}
static int complete_fd_equal(const struct complete_fds *a, const struct complete_fds *b)
{
    if (a->count != b->count) return 0;
    for (size_t i = 0; i < a->count; i++) if (!complete_fd_same(&a->entries[i], &b->entries[i])) return 0;
    return 1;
}
static void complete_fd_delta(const struct complete_fds *from, const struct complete_fds *to, const char *kind)
{
    for (size_t i = 0; i < to->count; i++) {
        const struct complete_fd *e = &to->entries[i]; size_t j = 0;
        while (j < from->count && !complete_fd_same(e, &from->entries[j])) j++;
        if (j != from->count) continue;
        char path[64], target[512]; snprintf(path, sizeof(path), "/proc/self/fd/%d", e->fd);
        ssize_t n = readlink(path, target, sizeof(target) - 1); if (n < 0) n = 0; target[n] = 0;
        fprintf(results, "FD_DELTA\t%s\t%d\t%ju\t%ju\t%s\n", kind, e->fd, (uintmax_t)e->dev, (uintmax_t)e->ino, n ? target : "unavailable");
    }
}
static int complete_fd_check(const struct complete_fds *before, int *after)
{
    struct complete_fds now = {0};
    for (unsigned retry = 0; retry <= 50; retry++) {
        free(now.entries);
        if (!complete_fd_snapshot(&now)) { snprintf(detail, sizeof(detail), "FD inventory failed:%s", strerror(errno)); return 0; }
        *after = (int)now.count;
        if (complete_fd_equal(before, &now)) {
            if (retry) fprintf(results, "FD_SETTLED\t%u\t%zu\n", retry, now.count);
            free(now.entries); return 1;
        }
        if (retry < 50) { struct timespec pause = {.tv_nsec = 10000000}; if (nanosleep(&pause, NULL) && errno != EINTR) break; }
    }
    complete_fd_delta(before, &now, "added"); complete_fd_delta(&now, before, "missing");
    free(now.entries); return 0;
}
/* The parent completed all Image operations before fork. Read the inherited
 * allocation through its DMA-BUF mapping; dma_buf_fops provides no read/pread. */
static int complete_child_read(const VADRMPRIMESurfaceDescriptor *d, unsigned expected)
{
    unsigned object = d->layers[0].object_index[0], offset = d->layers[0].offset[0];
    if (object >= d->num_objects || offset >= d->objects[object].size) return 11;
    int fd = d->objects[object].fd, error = 0;
    if (!prime_sync(fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ)) return 12;
    unsigned char *map = mmap(NULL, d->objects[object].size, PROT_READ, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) error = 13;
    else {
        if (map[offset] != expected) error = 14;
        if (munmap(map, d->objects[object].size) && !error) error = 15;
    }
    if (!prime_sync(fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ) && !error) error = 16;
    return error;
}
static void complete_prime(const struct dmabuf_format *f, unsigned layers)
{
    unsigned size = dmabuf_dimension(f); char tag[128];
    const char *phase = "setup"; unsigned cycles = 0, rejections = 0; int after = -1;
    VASurfaceID owner = VA_INVALID_ID; VADRMPRIMESurfaceDescriptor d = {0};
    VAImage im = {.image_id = VA_INVALID_ID};
    int ok = surface_size(f->rt, f->va, size, size, &owner) && image(f->va, size, size, &im) && complete_image(owner, &im, f, 0, 1) &&
        status(vaExportSurfaceHandle(display, owner, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, VA_EXPORT_SURFACE_READ_WRITE | layers, &d), "complete export2");
    VADRMPRIME3SurfaceDescriptor v3[2]; complete_v3(&d, &v3[0]); complete_v3(&d, &v3[1]);
    if (owner != VA_INVALID_ID) { ok = status(vaDestroySurfaces(display, &owner, 1), "complete early exporter destroy") && ok; owner = VA_INVALID_ID; }
    struct complete_fds inventory = {0};
    int inventoried = complete_fd_snapshot(&inventory), before = inventoried ? (int)inventory.count : -1; ok &= inventoried;
    for (unsigned frame = 1; frame <= 32 && ok; frame++) {
        phase = "cycle";
        VASurfaceID ids[2] = {VA_INVALID_ID, VA_INVALID_ID}; VADRMPRIME3SurfaceDescriptor e = {0};
        ok = status(complete_import(f, v3, 2, ids), "complete PRIME3 batch import") && complete_image(ids[0], &im, f, frame - 1, 0) &&
            complete_image(ids[1], &im, f, frame, 1) && complete_image(ids[0], &im, f, frame, 0) &&
            status(vaExportSurfaceHandle(display, ids[0], VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_3, VA_EXPORT_SURFACE_READ_WRITE | layers, &e), "complete export3");
        if (ok) {
            ok = e.fourcc == f->va && !e.flags && e.num_objects == d.num_objects && e.num_layers == d.num_layers;
            for (unsigned i = 0; i < sizeof(e.reserved) / sizeof(e.reserved[0]); i++) ok &= !e.reserved[i];
            for (unsigned i = 0; i < e.num_objects; i++) ok &= !!(fcntl(e.objects[i].fd, F_GETFD) & FD_CLOEXEC);
        }
        for (unsigned i = 0; i < 2; i++) if (ids[i] != VA_INVALID_ID) ok = status(vaDestroySurfaces(display, &ids[i], 1), "complete destroy imported") && ok;
        for (unsigned i = 0; i < e.num_objects; i++) close(e.objects[i].fd);
        if (inventoried) ok = complete_fd_check(&inventory, &after) && ok;
        if (ok) cycles++;
    }
    /* The second descriptor must be checked before either ID is published. */
    for (unsigned fault = 0; fault < 3 && ok; fault++) {
        phase = "reject";
        VASurfaceID ids[2] = {1234, 1235};
        v3[1].flags = fault == 0 ? VA_SURFACE_EXTBUF_DESC_PROTECTED : fault == 1 ? 1 : 0; v3[1].reserved[0] = fault == 2;
        VAStatus st = complete_import(f, v3, 2, ids);
        ok = st == (fault == 2 ? VA_STATUS_ERROR_INVALID_PARAMETER : VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE) && ids[0] == 1234 && ids[1] == 1235;
        ok = complete_fd_check(&inventory, &after) && ok;
        if (ok) rejections++;
    }
    v3[1].flags = v3[1].reserved[0] = 0;
    if (ok) {
        phase = "child";
        pid_t child = fork();
        if (child == 0) _exit(complete_child_read(&d, complete_byte(f, 0, 0, 0, 32)));
        int code = 0; pid_t waited = -1;
        if (child > 0) do { waited = waitpid(child, &code, 0); } while (waited < 0 && errno == EINTR);
        ok = child > 0 && waited == child && WIFEXITED(code) && !WEXITSTATUS(code);
        if (!ok) snprintf(detail, sizeof(detail), "inherited FD child:pid=%d wait=%d status=%d exit=%d", (int)child, (int)waited, code, WIFEXITED(code) ? WEXITSTATUS(code) : -1);
    }
    if (!ok && !detail[0]) snprintf(detail, sizeof(detail), "PRIME3 phase=%s cycles=%u rejections=%u FD-before=%d FD-after=%d", phase, cycles, rejections, before, after);
    if (ok) fprintf(results, "PRIME_EVIDENCE\t%s\t%s\t%u\t%u\t1\n", f->name, layers == VA_EXPORT_SURFACE_SEPARATE_LAYERS ? "separate" : "composed", cycles, rejections);
    if (im.image_id != VA_INVALID_ID) vaDestroyImage(display, im.image_id);
    dmabuf_close(&d); free(inventory.entries);
    snprintf(tag, sizeof(tag), "complete-prime3-%s-%s", f->name, layers == VA_EXPORT_SURFACE_SEPARATE_LAYERS ? "separate" : "composed"); result(tag, ok);
}
static void complete_rgb(const struct dmabuf_format *f)
{
    VASurfaceID s[2] = {VA_INVALID_ID, VA_INVALID_ID}; VAImage im = {.image_id = VA_INVALID_ID};
    VAConfigID cfg = VA_INVALID_ID; VAContextID ctx = VA_INVALID_ID;
    VAConfigAttrib rt = {.type = VAConfigAttribRTFormat, .value = f->rt}; char tag[128];
    int ok = surface_size(f->rt, f->va, 128, 128, &s[0]) && surface_size(f->rt, f->va, 128, 128, &s[1]) && image(f->va, 128, 128, &im) &&
        status(vaCreateConfig(display, VAProfileNone, VAEntrypointVideoProc, &rt, 1, &cfg), "complete RGB config") &&
        status(vaCreateContext(display, cfg, 128, 128, VA_PROGRESSIVE, NULL, 0, &ctx), "complete RGB context");
    VAProcPipelineParameterBuffer pipe = {.surface = s[0]}; setenv("VAREQ_VPP_DMABUF", "required", 1);
    /* Materialize both public allocations, then process into a PRIME3 alias. */
    VADRMPRIMESurfaceDescriptor d[2] = {{0}}; VADRMPRIME3SurfaceDescriptor v3; VASurfaceID alias = VA_INVALID_ID;
    for (unsigned i = 0; i < 2 && ok; i++) ok = complete_image(s[i], &im, f, 0, 1) &&
        status(vaExportSurfaceHandle(display, s[i], VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, VA_EXPORT_SURFACE_READ_WRITE | VA_EXPORT_SURFACE_COMPOSED_LAYERS, &d[i]), "complete RGB export");
    complete_v3(&d[1], &v3); ok = ok && status(complete_import(f, &v3, 1, &alias), "complete RGB alias");
    for (unsigned frame = 1; frame <= 32 && ok; frame++) ok = complete_image(s[0], &im, f, frame, 1) && process(ctx, alias, &pipe) && complete_image(s[1], &im, f, frame, 0);
    snprintf(tag, sizeof(tag), "complete-rgb-warm-%s", f->name); result(tag, ok);
    int cold = ok;
    for (unsigned frame = 33; frame <= 40 && cold; frame++) {
        VAContextID fresh = VA_INVALID_ID;
        cold = status(vaCreateContext(display, cfg, 128, 128, VA_PROGRESSIVE, NULL, 0, &fresh), "complete cold context") &&
            complete_image(s[0], &im, f, frame, 1) && process(fresh, alias, &pipe) && complete_image(s[1], &im, f, frame, 0);
        if (fresh != VA_INVALID_ID) vaDestroyContext(display, fresh);
    }
    snprintf(tag, sizeof(tag), "complete-rgb-cold-%s", f->name); result(tag, cold);
    if (ctx != VA_INVALID_ID) vaDestroyContext(display, ctx);
    if (cfg != VA_INVALID_ID) vaDestroyConfig(display, cfg);
    if (im.image_id != VA_INVALID_ID) vaDestroyImage(display, im.image_id);
    if (alias != VA_INVALID_ID) vaDestroySurfaces(display, &alias, 1);
    for (unsigned i = 0; i < 2; i++) { if (s[i] != VA_INVALID_ID) vaDestroySurfaces(display, &s[i], 1); dmabuf_close(&d[i]); }
}
static void complete_modifiers(void)
{
    uint64_t values[] = {DRM_FORMAT_MOD_INVALID, DRM_FORMAT_MOD_LINEAR};
    VADRMFormatModifierList list = {.num_modifiers = 2, .modifiers = values};
    VASurfaceAttrib a = {.type = VASurfaceAttribDRMFormatModifiers, .flags = VA_SURFACE_ATTRIB_SETTABLE, .value = {.type = VAGenericValueTypePointer, .value.p = &list}};
    VASurfaceID id = VA_INVALID_ID; VADRMPRIME3SurfaceDescriptor d = {0};
    int ok = status(vaCreateSurfaces(display, VA_RT_FORMAT_RGB32, 64, 64, &id, 1, &a, 1), "complete modifier allocation") &&
        status(vaExportSurfaceHandle(display, id, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_3, VA_EXPORT_SURFACE_READ_WRITE, &d), "complete modifier export");
    for (unsigned i = 0; i < d.num_objects; i++) { ok &= d.objects[i].drm_format_modifier == DRM_FORMAT_MOD_LINEAR; close(d.objects[i].fd); }
    if (id != VA_INVALID_ID) vaDestroySurfaces(display, &id, 1);
    list.num_modifiers = 1; id = 1234;
    ok = ok && vaCreateSurfaces(display, VA_RT_FORMAT_RGB32, 64, 64, &id, 1, &a, 1) == VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE && id == 1234;
    result("complete-modifier-contract", ok);
}
static void complete_pressure(void)
{
    VASurfaceID sid = VA_INVALID_ID; VADRMPRIMESurfaceDescriptor d = {0};
    int ok = surface(VA_RT_FORMAT_RGB32, VA_FOURCC_BGRX, &sid) &&
        status(vaExportSurfaceHandle(display, sid, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, VA_EXPORT_SURFACE_READ_WRITE, &d), "pressure initial export");
    struct rlimit saved; int held[256], count = 0, maxfd = 2, after = -1;
    struct complete_fds inventory = {0}; int inventoried = complete_fd_snapshot(&inventory); ok &= inventoried;
    for (size_t i = 0; i < inventory.count; i++) if (inventory.entries[i].fd > maxfd) maxfd = inventory.entries[i].fd;
    VADRMPRIME3SurfaceDescriptor v3; complete_v3(&d, &v3);
    if (ok && getrlimit(RLIMIT_NOFILE, &saved) == 0) {
        struct rlimit limited = saved; rlim_t limit = (rlim_t)maxfd + 32;
        if (limit < limited.rlim_cur) limited.rlim_cur = limit;
        if (setrlimit(RLIMIT_NOFILE, &limited)) { perror("pressure setrlimit"); ok = 0; }
        else {
            while (count < 256) { int fd = fcntl(d.objects[0].fd, F_DUPFD_CLOEXEC, 0); if (fd < 0) break; held[count++] = fd; }
            ok = count < 256 && errno == EMFILE;
            VASurfaceID untouched = 1234; VADRMPRIME3SurfaceDescriptor e, poison; memset(&e, 0xa5, sizeof(e)); poison = e;
            VAStatus exported = vaExportSurfaceHandle(display, sid, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_3, VA_EXPORT_SURFACE_READ_WRITE, &e);
            VAStatus imported = complete_import(&dmabuf_formats[14], &v3, 1, &untouched);
            ok &= exported == VA_STATUS_ERROR_ALLOCATION_FAILED && !memcmp(&e, &poison, sizeof(e)) &&
                imported == VA_STATUS_ERROR_ALLOCATION_FAILED && untouched == 1234;
            if (setrlimit(RLIMIT_NOFILE, &saved)) { perror("pressure restore rlimit"); ok = 0; }
        }
    } else ok = 0;
    while (count) close(held[--count]);
    if (inventoried) ok = complete_fd_check(&inventory, &after) && ok;
    VADRMPRIME3SurfaceDescriptor recovered = {0}; VASurfaceID recovered_id = VA_INVALID_ID;
    ok = status(vaExportSurfaceHandle(display, sid, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_3, VA_EXPORT_SURFACE_READ_WRITE, &recovered), "pressure recovered export") && ok;
    ok = status(complete_import(&dmabuf_formats[14], &v3, 1, &recovered_id), "pressure recovered import") && ok;
    if (recovered_id != VA_INVALID_ID) vaDestroySurfaces(display, &recovered_id, 1);
    for (unsigned i = 0; i < recovered.num_objects; i++) close(recovered.objects[i].fd);
    if (inventoried) ok = complete_fd_check(&inventory, &after) && ok;
    free(inventory.entries);
    if (sid != VA_INVALID_ID) vaDestroySurfaces(display, &sid, 1);
    dmabuf_close(&d); result("complete-fd-pressure-recovery", ok);
}
static void complete_jpeg(unsigned format, const char *name, unsigned iteration)
{
    const unsigned planes = format == VA_FOURCC_NV12 ? 2 : 3;
    VADRMPRIMESurfaceDescriptor d = {.fourcc = format, .width = 320, .height = 240, .num_objects = planes, .num_layers = 1};
    d.layers[0].num_planes = planes; d.layers[0].drm_format = format == VA_FOURCC_NV12 ? DRM_FORMAT_NV12 : format == VA_FOURCC_I420 ? DRM_FORMAT_YUV420 : DRM_FORMAT_YVU420;
    VASurfaceID owner[3] = {VA_INVALID_ID, VA_INVALID_ID, VA_INVALID_ID}, imported = VA_INVALID_ID, reference = VA_INVALID_ID;
    VAConfigID cfg = VA_INVALID_ID; VAContextID ctx = VA_INVALID_ID; VABufferID coded = VA_INVALID_ID;
    VAImage im = {.image_id = VA_INVALID_ID}; int ok = 1;
    for (unsigned p = 0; p < planes; p++) d.objects[p].fd = -1;
    for (unsigned p = 0; p < planes && ok; p++) {
        unsigned pitch = p && planes == 3 ? 160 : 320, rows = p ? 120 : 240;
        VADRMPRIMESurfaceDescriptor allocation = {0};
        ok = surface_size(VA_RT_FORMAT_RGB32, VA_FOURCC_BGRX, pitch / 4, rows, &owner[p]) &&
            status(vaExportSurfaceHandle(display, owner[p], VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, VA_EXPORT_SURFACE_READ_WRITE | VA_EXPORT_SURFACE_COMPOSED_LAYERS, &allocation), "JPEG plane backing");
        if (ok) {
            ok = allocation.num_objects == 1 && allocation.objects[0].drm_format_modifier == DRM_FORMAT_MOD_LINEAR;
            d.objects[p].fd = allocation.objects[0].fd; d.objects[p].size = allocation.objects[0].size;
        }
        d.layers[0].object_index[p] = p; d.layers[0].pitch[p] = pitch;
    }
    struct dmabuf_format f = {VA_RT_FORMAT_YUV420, format, d.layers[0].drm_format, planes, 1, 120, name};
    VADRMPRIME3SurfaceDescriptor v3; complete_v3(&d, &v3);
    ok = ok && status(complete_import(&f, &v3, 1, &imported), "JPEG multi-object import") &&
        surface_size(VA_RT_FORMAT_YUV420, format, 320, 240, &reference) && image(format, 320, 240, &im) &&
        status(vaCreateConfig(display, VAProfileJPEGBaseline, VAEntrypointEncPicture, NULL, 0, &cfg), "JPEG DMA config") &&
        status(vaCreateContext(display, cfg, 320, 240, VA_PROGRESSIVE, NULL, 0, &ctx), "JPEG DMA context") &&
        status(vaCreateBuffer(display, ctx, VAEncCodedBufferType, 262144, 1, NULL, &coded), "JPEG DMA coded");
    for (unsigned p = 0; p < planes; p++) if (owner[p] != VA_INVALID_ID) vaDestroySurfaces(display, &owner[p], 1);
    dmabuf_close(&d); /* Only imported surfaces now retain the input objects. */
    for (unsigned frame = 0; frame < 4 && ok; frame++) {
        unsigned index = iteration * 4 + frame;
        setenv("VAREQ_JPEG_DMABUF", "0", 1);
        ok = jpeg_layout_upload(reference, &im, index) && jpeg_layout_encode(ctx, reference, coded, index, "ref");
        setenv("VAREQ_JPEG_DMABUF", "required", 1);
        ok = ok && jpeg_layout_upload(imported, &im, index) && jpeg_layout_encode(ctx, imported, coded, index, "test");
    }
    setenv("VAREQ_JPEG_DMABUF", "auto", 1);
    if (coded != VA_INVALID_ID) vaDestroyBuffer(display, coded);
    if (ctx != VA_INVALID_ID) vaDestroyContext(display, ctx);
    if (cfg != VA_INVALID_ID) vaDestroyConfig(display, cfg);
    if (im.image_id != VA_INVALID_ID) vaDestroyImage(display, im.image_id);
    if (reference != VA_INVALID_ID) vaDestroySurfaces(display, &reference, 1);
    if (imported != VA_INVALID_ID) vaDestroySurfaces(display, &imported, 1);
    char tag[96]; snprintf(tag, sizeof(tag), "complete-jpeg-dma-%s", name); result(tag, ok);
}
#ifndef VA_DMABUF_COMPLETE_MAIN
#define VA_DMABUF_COMPLETE_MAIN main
#endif
int VA_DMABUF_COMPLETE_MAIN(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: %s RENDER-NODE RESULTS.tsv\n", argv[0]); return 2; }
    results = fopen(argv[2], "w"); if (!results) { perror(argv[2]); return 2; }
    report_path = argv[2];
    if (setenv("MESA_SHADER_CACHE_DISABLE", "true", 1)) { perror("disable shader cache for FD inventory"); fclose(results); return 2; }
    int fd = open(argv[1], O_RDWR | O_CLOEXEC), major, minor;
    if (fd < 0) { perror(argv[1]); fclose(results); return 2; }
    display = vaGetDisplayDRM(fd);
    if (!display || !status(vaInitialize(display, &major, &minor), "complete initialize")) { close(fd); fclose(results); return 2; }
    const char *vendor = vaQueryVendorString(display);
    if (!vareq_test_driver_identity(vendor)) { fprintf(stderr, "Unexpected VA driver\n"); vaTerminate(display); close(fd); fclose(results); return 2; }
    for (unsigned i = 0; i < sizeof(dmabuf_formats) / sizeof(dmabuf_formats[0]); i++)
        for (unsigned layer = 0; layer < 2; layer++) complete_prime(&dmabuf_formats[i], layer ? VA_EXPORT_SURFACE_SEPARATE_LAYERS : VA_EXPORT_SURFACE_COMPOSED_LAYERS);
    for (unsigned i = 0; i < sizeof(additions) / sizeof(additions[0]); i++) {
        for (unsigned layer = 0; layer < 2; layer++) complete_prime(&additions[i], layer ? VA_EXPORT_SURFACE_SEPARATE_LAYERS : VA_EXPORT_SURFACE_COMPOSED_LAYERS);
        complete_rgb(&additions[i]);
    }
    complete_modifiers();
    complete_pressure();
    complete_jpeg(VA_FOURCC_NV12, "NV12", 0); complete_jpeg(VA_FOURCC_I420, "I420", 1); complete_jpeg(VA_FOURCC_YV12, "YV12", 2);
    vaTerminate(display); close(fd); fprintf(results, "COMPLETE\t1\n"); if (fclose(results)) return 2;
    printf("DMA_COMPLETE PASS=%u FAIL=%u COMPLETE=1 EXIT=%u\n", passed, failed, failed ? 1 : 0); return failed ? 1 : 0;
}
