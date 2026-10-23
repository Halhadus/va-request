/* Exercise the actual public client, including fork, with DMA-BUF file I/O
 * semantics: mapping works, while pread is forbidden even on memfd fixtures. */
#define VA_DMABUF_COMPLETE_MAIN unused_hardware_complete_main
#include "va_dmabuf_complete.c"
#include <stdarg.h>

static unsigned errors, next_id, image_size, current_format, in_child, active, maps, fault;
static unsigned char image_bytes[131072];
static VAImage fixture_image;
static struct { int live; VADRMPRIMESurfaceDescriptor d; } surfaces[128];
static unsigned fd_cursor, inventory_calls, transient_inventory;
static int transient_fd = -1, inventory_error;
static struct dirent fd_entry;
/* Enumerate real descriptor state without requiring procfs in the test sandbox. */
DIR *__wrap_opendir(const char *path)
{
    if (strcmp(path, "/proc/self/fd") || inventory_error) { errno = EACCES; return NULL; }
    inventory_calls++;
    if (transient_inventory && inventory_calls == transient_inventory) { transient_fd = memfd_create("transient cache file", MFD_CLOEXEC); if (transient_fd < 0) return NULL; }
    if (transient_fd >= 0 && inventory_calls == transient_inventory + 1) { close(transient_fd); transient_fd = -1; }
    fd_cursor = 0; return (DIR *)&fd_cursor;
}
int __wrap_dirfd(DIR *d) { (void)d; return 255; }
struct dirent *__wrap_readdir(DIR *d)
{
    (void)d; int saved = errno;
    while (fd_cursor < 256) { unsigned fd = fd_cursor++; if (fcntl(fd, F_GETFD) >= 0) { snprintf(fd_entry.d_name, sizeof(fd_entry.d_name), "%u", fd); errno = saved; return &fd_entry; } }
    errno = saved;
    return NULL;
}
struct dirent64 *__wrap_readdir64(DIR *d) { struct dirent *e = __wrap_readdir(d); static struct dirent64 out; if (!e) return NULL; snprintf(out.d_name, sizeof(out.d_name), "%s", e->d_name); return &out; }
int __wrap_closedir(DIR *d) { (void)d; return 0; }
static const struct dmabuf_format *format_at(unsigned n) { return n < 15 ? &dmabuf_formats[n] : &additions[n - 15]; }
static void expect(int ok, const char *tag) { if (!ok) { fprintf(stderr, "FAIL complete client %s fault=%u\n", tag, fault); errors++; } }
pid_t __real_fork(void);
pid_t __wrap_fork(void) { pid_t p = __real_fork(); if (!p) in_child = 1; return p; }
void __real__exit(int);
__attribute__((noreturn)) void __wrap__exit(int code) { __real__exit(active || maps || errors ? 90 : code); __builtin_unreachable(); }
ssize_t __wrap_pread(int fd, void *p, size_t n, off_t o) { (void)fd; (void)p; (void)n; (void)o; errno = ESPIPE; return -1; }
ssize_t __wrap_pread64(int fd, void *p, size_t n, off64_t o) { (void)fd; (void)p; (void)n; (void)o; errno = ESPIPE; return -1; }
int __wrap_ioctl(int fd, unsigned long req, ...)
{
    (void)fd; va_list args; va_start(args, req); struct dma_buf_sync *s = va_arg(args, struct dma_buf_sync *); va_end(args);
    if (req != DMA_BUF_IOCTL_SYNC || (s->flags & DMA_BUF_SYNC_RW) != DMA_BUF_SYNC_READ) { errno = EINVAL; return -1; }
    if (s->flags & DMA_BUF_SYNC_END) {
        expect(active && !maps, "END-after-unmap"); active = 0;
        if (fault == 5) { errno = EIO; return -1; }
    } else {
        expect(!active && !maps, "START-before-map");
        if (fault == 1) { errno = EIO; return -1; }
        active = 1;
    }
    return 0;
}
void *__real_mmap(void *, size_t, int, int, int, off_t);
void *__real_mmap64(void *, size_t, int, int, int, off64_t);
int __real_munmap(void *, size_t);
static int map_allowed(int prot)
{
    if (!in_child) return 1;
    expect(active && prot == PROT_READ, "child-read-only-map-inside-sync");
    if (fault == 2) { errno = ENOMEM; return 0; }
    return 1;
}
void *__wrap_mmap(void *a, size_t n, int p, int f, int fd, off_t o)
{
    if (!map_allowed(p)) return MAP_FAILED;
    void *m = __real_mmap(a, n, p, f, fd, o); if (m != MAP_FAILED && in_child) maps++; return m;
}
void *__wrap_mmap64(void *a, size_t n, int p, int f, int fd, off64_t o)
{
    if (!map_allowed(p)) return MAP_FAILED;
    void *m = __real_mmap64(a, n, p, f, fd, o); if (m != MAP_FAILED && in_child) maps++; return m;
}
int __wrap_munmap(void *a, size_t n)
{
    int ret = __real_munmap(a, n);
    if (in_child) { expect(active && maps, "child-unmap-before-END"); if (!ret) maps--; }
    if (in_child && fault == 4) { errno = EIO; return -1; }
    return ret;
}
int vaMaxNumImageFormats(VADisplay d) { (void)d; return 18; }
VAStatus vaQueryImageFormats(VADisplay d, VAImageFormat *f, int *n)
{
    (void)d; for (unsigned i = 0; i < 18; i++) f[i] = (VAImageFormat){.fourcc = format_at(i)->va}; *n = 18; return 0;
}
VAStatus vaCreateImage(VADisplay d, VAImageFormat *f, int w, int h, VAImage *im)
{
    (void)d; const struct dmabuf_format *fmt = NULL;
    for (unsigned i = 0; i < 18; i++) if (format_at(i)->va == f->fourcc) { fmt = format_at(i); current_format = i; }
    if (!fmt) return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    *im = (VAImage){.format = *f, .width = w, .height = h, .num_planes = fmt->planes}; unsigned offset = 0, stride = (w * fmt->bytes + 63) & ~63u;
    for (unsigned p = 0; p < fmt->planes; p++) {
        unsigned rows = p && f->fourcc != VA_FOURCC_422H && f->fourcc != VA_FOURCC_YV16 ? (unsigned)h / 2 : (unsigned)h;
        im->pitches[p] = p && fmt->planes == 3 ? stride / 2 : stride; im->offsets[p] = offset; offset += im->pitches[p] * rows;
    }
    im->data_size = image_size = offset; fixture_image = *im;
    expect(offset <= sizeof(image_bytes), "image-fixture-capacity"); return 0;
}
VAStatus vaMapBuffer(VADisplay d, VABufferID id, void **p) { (void)d; (void)id; *p = image_bytes; return 0; }
VAStatus vaUnmapBuffer(VADisplay d, VABufferID id) { (void)d; (void)id; return 0; }
VAStatus vaDestroyImage(VADisplay d, VAImageID id) { (void)d; (void)id; return 0; }
static VAStatus fixture_transfer(VASurfaceID id, int write)
{
    const struct dmabuf_format *f = format_at(current_format); VADRMPRIMESurfaceDescriptor *d = &surfaces[id].d;
    if (!surfaces[id].live) return VA_STATUS_ERROR_INVALID_SURFACE;
    for (unsigned p = 0; p < f->planes; p++) {
        unsigned l = d->num_layers == 1 ? 0 : p, dp = d->num_layers == 1 ? p : 0, o = d->layers[l].object_index[dp];
        unsigned row = d->width * f->bytes / (p && f->planes == 3 ? 2 : 1);
        unsigned rows = p && f->va != VA_FOURCC_422H && f->va != VA_FOURCC_YV16 ? d->height / 2 : d->height;
        unsigned char *map = mmap(NULL, d->objects[o].size, PROT_READ | PROT_WRITE, MAP_SHARED, d->objects[o].fd, 0);
        if (map == MAP_FAILED) return VA_STATUS_ERROR_OPERATION_FAILED;
        for (unsigned y = 0; y < rows; y++) {
            void *external = map + d->layers[l].offset[dp] + y * d->layers[l].pitch[dp];
            void *image = image_bytes + fixture_image.offsets[p] + y * fixture_image.pitches[p];
            if (write) memcpy(external, image, row); else memcpy(image, external, row);
        }
        munmap(map, d->objects[o].size);
    }
    return 0;
}
VAStatus vaGetImage(VADisplay d, VASurfaceID s, int x, int y, unsigned w, unsigned h, VAImageID id)
{ (void)d; (void)x; (void)y; (void)w; (void)h; (void)id; return fixture_transfer(s, 0); }
VAStatus vaPutImage(VADisplay d, VASurfaceID s, VAImageID id, int x, int y, unsigned w, unsigned h, int dx, int dy, unsigned dw, unsigned dh)
{ (void)d; (void)id; (void)x; (void)y; (void)w; (void)h; (void)dx; (void)dy; (void)dw; (void)dh; return fixture_transfer(s, 1); }
VAStatus vaCreateSurfaces(VADisplay display_arg, unsigned rt, unsigned w, unsigned h, VASurfaceID *ids, unsigned count, VASurfaceAttrib *a, unsigned n)
{
    (void)display_arg; (void)rt; unsigned fourcc = 0, memory = VA_SURFACE_ATTRIB_MEM_TYPE_VA; const void *external = NULL;
    for (unsigned i = 0; i < n; i++) {
        if (a[i].type == VASurfaceAttribPixelFormat) fourcc = a[i].value.value.i;
        if (a[i].type == VASurfaceAttribMemoryType) memory = a[i].value.value.i;
        if (a[i].type == VASurfaceAttribExternalBufferDescriptor) external = a[i].value.value.p;
    }
    int prime3 = memory == VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_3;
    size_t stride = prime3 ? sizeof(VADRMPRIME3SurfaceDescriptor) : sizeof(VADRMPRIMESurfaceDescriptor);
    if (external && memory != VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2 && !prime3) return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
    if (external && prime3) for (unsigned i = 0; i < count; i++) {
        const VADRMPRIME3SurfaceDescriptor *d = (const void *)((const char *)external + i * stride);
        if (d->flags) return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
        for (unsigned j = 0; j < sizeof(d->reserved) / sizeof(d->reserved[0]); j++) if (d->reserved[j]) return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    for (unsigned i = 0; i < count; i++) {
        unsigned id = ++next_id; if (id >= 128) return VA_STATUS_ERROR_ALLOCATION_FAILED;
        VADRMPRIMESurfaceDescriptor *d = &surfaces[id].d;
        if (external) {
            memset(d, 0, sizeof(*d)); memcpy(d, (const char *)external + i * stride, offsetof(VADRMPRIME3SurfaceDescriptor, flags));
            for (unsigned o = 0; o < d->num_objects; o++) { d->objects[o].fd = fcntl(d->objects[o].fd, F_DUPFD_CLOEXEC, 0); expect(d->objects[o].fd >= 0 && d->objects[o].fd < 256, "retained-FD-in-inventory-range"); }
        } else {
            const struct dmabuf_format *f = NULL; for (unsigned j = 0; j < 18; j++) if (format_at(j)->va == fourcc) f = format_at(j);
            if (!f) return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
            unsigned stride = (w * f->bytes + 63) & ~63u, offset = 0;
            *d = (VADRMPRIMESurfaceDescriptor){.fourcc = fourcc, .width = w, .height = h, .num_objects = 1, .num_layers = 1};
            d->layers[0].num_planes = f->planes; d->layers[0].drm_format = f->drm;
            for (unsigned p = 0; p < f->planes; p++) {
                unsigned rows = p && fourcc != VA_FOURCC_422H && fourcc != VA_FOURCC_YV16 ? h / 2 : h;
                d->layers[0].pitch[p] = p && f->planes == 3 ? stride / 2 : stride; d->layers[0].offset[p] = offset; offset += d->layers[0].pitch[p] * rows;
            }
            d->objects[0].fd = memfd_create("map-only DMA fixture", MFD_CLOEXEC); d->objects[0].size = offset;
            expect(d->objects[0].fd >= 0 && d->objects[0].fd < 256, "native-FD-in-inventory-range");
            if (d->objects[0].fd < 0 || ftruncate(d->objects[0].fd, offset)) return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
        surfaces[id].live = 1; ids[i] = id;
    }
    return 0;
}
VAStatus vaDestroySurfaces(VADisplay d, VASurfaceID *ids, int n)
{
    (void)d; for (int i = 0; i < n; i++) { dmabuf_close(&surfaces[ids[i]].d); surfaces[ids[i]].live = 0; } return 0;
}
VAStatus vaExportSurfaceHandle(VADisplay d, VASurfaceID id, uint32_t type, uint32_t flags, void *out)
{
    (void)d; VADRMPRIMESurfaceDescriptor e = surfaces[id].d;
    if (e.num_layers == 1 && e.layers[0].num_planes > 1 && (flags & VA_EXPORT_SURFACE_SEPARATE_LAYERS)) {
        e.num_layers = e.layers[0].num_planes;
        for (unsigned p = 1; p < e.num_layers; p++) { e.layers[p].num_planes = 1; e.layers[p].object_index[0] = e.layers[0].object_index[p]; e.layers[p].offset[0] = e.layers[0].offset[p]; e.layers[p].pitch[0] = e.layers[0].pitch[p]; }
        e.layers[0].num_planes = 1;
    }
    for (unsigned o = 0; o < e.num_objects; o++) { e.objects[o].fd = fcntl(e.objects[o].fd, F_DUPFD_CLOEXEC, 0); expect(e.objects[o].fd >= 0 && e.objects[o].fd < 256, "exported-FD-in-inventory-range"); }
    if (type == VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_3) complete_v3(&e, out); else *(VADRMPRIMESurfaceDescriptor *)out = e;
    return 0;
}
static void child_failures(void)
{
    int fd = memfd_create("child failure fixture", MFD_CLOEXEC); expect(fd >= 0 && !ftruncate(fd, 4096), "failure-storage");
    unsigned char *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0); if (p == MAP_FAILED) exit(1); p[17] = 123; munmap(p, 4096);
    VADRMPRIMESurfaceDescriptor d = {.num_objects = 1}; d.objects[0].fd = fd; d.objects[0].size = 4096; d.layers[0].offset[0] = 17;
    in_child = 1;
    const int codes[] = {0, 12, 13, 14, 15, 16};
    for (fault = 0; fault < 6; fault++) { expect(complete_child_read(&d, fault == 3 ? 124 : 123) == codes[fault], "child-error-stage"); expect(!active && !maps, "child-error-cleanup"); }
    fault = 0; d.layers[0].offset[0] = 4096; expect(complete_child_read(&d, 123) == 11 && !active && !maps, "child-span-validation");
    in_child = 0; close(fd);
}
static void inventory_failures(void)
{
    struct complete_fds before = {0}; int after = -1;
    expect(complete_fd_snapshot(&before) && complete_fd_check(&before, &after), "inventory-baseline");
    transient_inventory = inventory_calls + 1;
    expect(complete_fd_check(&before, &after) && transient_fd < 0, "transient-extra-must-close");
    transient_inventory = 0;
    int leak = fcntl(results ? fileno(results) : -1, F_DUPFD_CLOEXEC, 0);
    expect(leak >= 0 && !complete_fd_check(&before, &after), "persistent-duplicate-must-fail");
    close(leak); expect(complete_fd_check(&before, &after), "closed-duplicate-recovers");
    free(before.entries);
    int a = memfd_create("original inventory object", MFD_CLOEXEC), b = memfd_create("replacement inventory object", MFD_CLOEXEC);
    expect(a >= 0 && b >= 0 && complete_fd_snapshot(&before), "replacement-baseline");
    expect(dup2(b, a) == a && !complete_fd_check(&before, &after) && after == (int)before.count, "same-count-replaced-inode-must-fail");
    free(before.entries); close(a); close(b);
    expect(complete_fd_snapshot(&before), "inventory-error-baseline");
    inventory_error = 1; expect(!complete_fd_check(&before, &after), "inventory-read-error-must-fail"); inventory_error = 0;
    free(before.entries); detail[0] = 0;
}
int main(void)
{
    int report_fd = memfd_create("complete client results", MFD_CLOEXEC);
    results = report_fd >= 0 ? fdopen(report_fd, "w+") : NULL; if (!results) { perror("complete results"); if (report_fd >= 0) close(report_fd); return 1; }
    for (unsigned n = 0; n < 18; n++) for (unsigned layer = 0; layer < 2; layer++) {
        next_id = 0; memset(surfaces, 0, sizeof(surfaces));
        complete_prime(format_at(n), layer ? VA_EXPORT_SURFACE_SEPARATE_LAYERS : VA_EXPORT_SURFACE_COMPOSED_LAYERS);
        for (unsigned id = 1; id <= next_id; id++) expect(!surfaces[id].live, "every-created-surface-destroyed");
    }
    expect(passed == 36 && !failed, "all-public-lifetime-cases-with-map-only-FDs");
    child_failures(); inventory_failures(); fclose(results);
    printf("complete client:%s,36 lifetime cases,1152 cycles,seven child access/cleanup cases,FD identity and transient/persistent/replacement/read-error cases\n", errors ? "FAIL" : "PASS"); return errors != 0;
}
