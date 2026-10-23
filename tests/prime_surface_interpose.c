/* Test allocator: force public decoder pools through PRIME2; no fallback.
 * Drop every exporter BO/fd immediately after vaCreateSurfaces returns. */
#include <dlfcn.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <gbm.h>
#include <drm_fourcc.h>
#include <va/va.h>
#include <va/va_drmcommon.h>

typedef VAStatus (*create_fn)(VADisplay, unsigned, unsigned, unsigned, VASurfaceID *, unsigned, VASurfaceAttrib *, unsigned);
VAStatus vaCreateSurfaces(VADisplay dpy, unsigned format, unsigned width, unsigned height, VASurfaceID *surfaces,
                          unsigned count, VASurfaceAttrib *attrs, unsigned num_attrs)
{
    create_fn real = (create_fn)dlsym(RTLD_NEXT, "vaCreateSurfaces");
    const char *directory = getenv("VAREQ_PRIME_TRACE_DIR");
    if (!directory) return real ? real(dpy, format, width, height, surfaces, count, attrs, num_attrs) : VA_STATUS_ERROR_OPERATION_FAILED;
    unsigned fcc = format == VA_RT_FORMAT_YUV420_10 ? VA_FOURCC_P010 : VA_FOURCC_NV12;
    for (unsigned i = 0; i < num_attrs; i++) if (attrs[i].type == VASurfaceAttribPixelFormat) fcc = attrs[i].value.value.i;
    VAStatus st = VA_STATUS_ERROR_OPERATION_FAILED;
    VADRMPRIMESurfaceDescriptor *descs = NULL; struct gbm_bo **bos = NULL; struct gbm_device *dev = NULL; int render = -1;
    if (!real || !count || count > 256 || !width || !height || (width & 1) || (height & 1) || width > 16384 || height > 16384 ||
        (fcc != VA_FOURCC_NV12 && fcc != VA_FOURCC_P010)) goto done;
    const char *node = getenv("VAREQ_RENDER_NODE");
    render = open(node ? node : "/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (render < 0 || !(dev = gbm_create_device(render))) goto done;
    descs = calloc(count, sizeof(*descs)); bos = calloc(count, sizeof(*bos));
    if (descs) for (unsigned i = 0; i < count; i++) descs[i].objects[0].fd = -1;
    if (!descs || !bos) goto done;
    const char *layout = getenv("VAREQ_PRIME_CANONICAL");
    int canonical = layout && !strcmp(layout, "1") && fcc == VA_FOURCC_NV12;
    for (unsigned i = 0; i < count; i++) {
        unsigned bytes = (width * (fcc == VA_FOURCC_P010 ? 2 : 1) + 63) & ~63u;
        bos[i] = gbm_bo_create(dev, bytes, (canonical ? ((height + 15) & ~15u) * 3 + 256 : height + height / 2 + 8), GBM_FORMAT_R8, GBM_BO_USE_LINEAR);
        int fd = bos[i] ? gbm_bo_get_fd_for_plane(bos[i], 0) : -1;
        descs[i].objects[0].fd = fd;
        off_t size = fd >= 0 ? lseek(fd, 0, SEEK_END) : -1;
        if (size <= 0 || (uint64_t)size > UINT32_MAX || lseek(fd, 0, SEEK_SET) < 0) goto done;
        unsigned pitch = gbm_bo_get_stride_for_plane(bos[i], 0);
        descs[i].fourcc = fcc; descs[i].width = width; descs[i].height = height; descs[i].num_objects = descs[i].num_layers = 1;
        descs[i].objects[0].size = size; descs[i].objects[0].drm_format_modifier = gbm_bo_get_modifier(bos[i]);
        descs[i].layers[0].drm_format = fcc == VA_FOURCC_P010 ? DRM_FORMAT_P010 : DRM_FORMAT_NV12;
        descs[i].layers[0].num_planes = 2; descs[i].layers[0].pitch[0] = descs[i].layers[0].pitch[1] = pitch;
        descs[i].layers[0].offset[1] = pitch * (canonical ? ((height + 15) & ~15u) : height + 4);
    }
    VASurfaceAttrib imported[] = {
        {.type = VASurfaceAttribPixelFormat, .flags = VA_SURFACE_ATTRIB_SETTABLE, .value = {.type = VAGenericValueTypeInteger, .value.i = fcc}},
        {.type = VASurfaceAttribMemoryType, .flags = VA_SURFACE_ATTRIB_SETTABLE, .value = {.type = VAGenericValueTypeInteger, .value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2}},
        {.type = VASurfaceAttribExternalBufferDescriptor, .flags = VA_SURFACE_ATTRIB_SETTABLE, .value = {.type = VAGenericValueTypePointer, .value.p = descs}}};
    st = real(dpy, format, width, height, surfaces, count, imported, 3);
done:
    if (descs) for (unsigned i = 0; i < count; i++) if (descs[i].objects[0].fd >= 0) close(descs[i].objects[0].fd);
    if (bos) for (unsigned i = 0; i < count; i++) if (bos[i]) gbm_bo_destroy(bos[i]);
    free(descs); free(bos); if (dev) gbm_device_destroy(dev); if (render >= 0) close(render);
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/prime-%ld.tsv", directory, (long)getpid()) >= (int)sizeof(path)) return VA_STATUS_ERROR_OPERATION_FAILED;
    FILE *log = fopen(path, "a");
    if (!log) return VA_STATUS_ERROR_OPERATION_FAILED;
    int failed = fprintf(log, "%u\t%u\t%u\t%u\t%d\n", fcc, width, height, count, st) < 0;
    if (fclose(log)) failed = 1;
    return failed ? VA_STATUS_ERROR_OPERATION_FAILED : st;
}
