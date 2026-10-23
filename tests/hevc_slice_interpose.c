/* Test-only LD_PRELOAD adapter: keep FFmpeg's codec syntax parameters and alter
 * only SliceData ranges/fragment representation. Never install with the driver. */
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver_identity.h"
#include <va/va.h>
#include <va/va_dec_hevc.h>
#include <va/va_dec_vp8.h>

/* Exported libva helper, declared identically in libva 2.23 va_trace.c.
 * It is not declared by public va.h; this dependency is test-only. */
extern VAStatus vaBufferInfo(VADisplay display, VAContextID context, VABufferID id,
                              VABufferType *type, unsigned *size, unsigned *elements);

static VAStatus (*next_render)(VADisplay, VAContextID, VABufferID *, int);
static VAStatus (*next_end)(VADisplay, VAContextID);
static VAStatus (*next_destroy)(VADisplay, VAContextID);
static pthread_once_t initialize_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t adapter_lock = PTHREAD_MUTEX_INITIALIZER;
static const char *mode_name;
static int mode = -1;
static FILE *trace;
static unsigned picture_number;
struct pending_picture {
    VADisplay display;
    VAContextID context;
    VABufferID ids[256];
    unsigned count, slices, segments;
    VAStatus error;
    struct pending_picture *next;
};
static struct pending_picture *pending;

static void initialize(void)
{
    next_render = dlsym(RTLD_NEXT, "vaRenderPicture");
    next_end = dlsym(RTLD_NEXT, "vaEndPicture");
    next_destroy = dlsym(RTLD_NEXT, "vaDestroyContext");
    mode_name = getenv("VAREQ_HEVC_PACKET_MODE");
    const char *path = getenv("VAREQ_HEVC_PACKET_LOG");
    const char *names[] = {"whole", "offset", "fragments", "array", "split-prefix"};
    if (!mode_name || !path || !next_render || !next_end || !next_destroy) return;
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (!strcmp(mode_name, names[i])) mode = i;
    if (mode < 0) return;
    trace = fopen(path, "w");
    if (!trace || fprintf(trace, "MODE\tPICTURE\tSLICES\tSEGMENTS\tSTATUS\tDRIVER\tEXPBUF\n") < 0 || fflush(trace)) mode = -1;
}

static struct pending_picture *picture(VADisplay display, VAContextID context, int create)
{
    for (struct pending_picture *p = pending; p; p = p->next)
        if (p->display == display && p->context == context) return p;
    if (!create) return NULL;
    struct pending_picture *p = calloc(1, sizeof(*p));
    if (p) { p->display = display; p->context = context; p->next = pending; pending = p; }
    return p;
}

static VAStatus release_picture(struct pending_picture *p)
{
    VAStatus status = VA_STATUS_SUCCESS;
    for (unsigned i = 0; i < p->count; i++) {
        VAStatus cleanup = vaDestroyBuffer(p->display, p->ids[i]);
        if (status == VA_STATUS_SUCCESS) status = cleanup;
    }
    struct pending_picture **link = &pending;
    while (*link != p) link = &(*link)->next;
    *link = p->next;
    free(p);
    return status;
}

static VAStatus submit_pair(struct pending_picture *p, void *params, unsigned element_size,
                            unsigned count, uint8_t *data, unsigned size)
{
    if (p->count > 254) return VA_STATUS_ERROR_ALLOCATION_FAILED;
    unsigned first = p->count;
    VAStatus status = vaCreateBuffer(p->display, p->context, VASliceParameterBufferType,
        element_size, count, params, &p->ids[p->count]);
    if (status != VA_STATUS_SUCCESS) return status;
    p->count++;
    status = vaCreateBuffer(p->display, p->context, VASliceDataBufferType, size, 1, data, &p->ids[p->count]);
    if (status != VA_STATUS_SUCCESS) return status;
    p->count++;
    status = next_render(p->display, p->context, p->ids + first, 2);
    if (status == VA_STATUS_SUCCESS) p->segments += count;
    return status;
}

static VAStatus transform_slice(struct pending_picture *p, const void *source, unsigned element_size,
                                const uint8_t *data)
{
    VASliceParameterBufferBase base; memcpy(&base, source, sizeof(base));
    unsigned size = base.slice_data_size;
    int raw = element_size == sizeof(VASliceParameterBufferVP8) || element_size == sizeof(VASliceParameterBufferMPEG2);
    if (!raw && size >= 3 && !data[0] && !data[1] && data[2] == 1) { data += 3; size -= 3; }
    else if (!raw && size >= 4 && !data[0] && !data[1] && !data[2] && data[3] == 1) { data += 4; size -= 4; }
    unsigned prefix = raw ? 0 : mode == 3 ? 4 : mode == 4 ? 3 : 0;
    if (size < 3 || size > UINT32_MAX - prefix - 24) return VA_STATUS_ERROR_INVALID_PARAMETER;
    unsigned bytes = size + prefix;
    uint8_t *payload = malloc(bytes + 24);
    if (!payload) return VA_STATUS_ERROR_ALLOCATION_FAILED;
    memset(payload, 0xab, bytes + 24);
    if (prefix) { memset(payload + 11, 0, prefix); payload[10 + prefix] = 1; }
    memcpy(payload + 11 + prefix, data, size);
    unsigned parts = mode == 1 ? 1 : 3;
    union { VASliceParameterBufferH264 h264; VASliceParameterBufferHEVC hevc; } largest;
    uint8_t params[3 * sizeof(largest)];
    if (element_size > sizeof(params) / 3) { free(payload); return VA_STATUS_ERROR_INVALID_PARAMETER; }
    for (unsigned i = 0; i < parts; i++) {
        memcpy(params + i * element_size, source, element_size);
        base.slice_data_offset = 11 + i;
        base.slice_data_size = parts == 1 ? bytes : i < 2 ? 1 : bytes - 2;
        base.slice_data_flag = parts == 1 ? VA_SLICE_DATA_FLAG_ALL : i == 0 ? VA_SLICE_DATA_FLAG_BEGIN :
            i == 2 ? VA_SLICE_DATA_FLAG_END : VA_SLICE_DATA_FLAG_MIDDLE;
        memcpy(params + i * element_size, &base, sizeof(base));
    }
    VAStatus status = VA_STATUS_SUCCESS;
    if (mode == 3) status = submit_pair(p, params, element_size, parts, payload, bytes + 24);
    else {
        for (unsigned i = 0; i < parts && status == VA_STATUS_SUCCESS; i++)
            status = submit_pair(p, params + i * element_size, element_size, 1, payload, bytes + 24);
    }
    free(payload);
    return status;
}

static VAStatus slice_pair(struct pending_picture *p, VABufferID parameter_id, VABufferID data_id)
{
    VABufferType type; unsigned size, count, data_size, data_count;
    VAStatus status = vaBufferInfo(p->display, p->context, parameter_id, &type, &size, &count);
    if (status != VA_STATUS_SUCCESS) return status;
    if (type != VASliceParameterBufferType || (size != sizeof(VASliceParameterBufferH264) && size != sizeof(VASliceParameterBufferHEVC) && size != sizeof(VASliceParameterBufferVP8) && size != sizeof(VASliceParameterBufferMPEG2)) || !count || count > 256)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    status = vaBufferInfo(p->display, p->context, data_id, &type, &data_size, &data_count);
    if (status != VA_STATUS_SUCCESS) return status;
    if (type != VASliceDataBufferType || !data_size || !data_count || data_size > UINT32_MAX / data_count)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    data_size *= data_count;
    uint8_t *params = NULL, *data = NULL;
    status = vaMapBuffer(p->display, parameter_id, (void **)&params);
    if (status != VA_STATUS_SUCCESS) return status;
    status = vaMapBuffer(p->display, data_id, (void **)&data);
    int data_mapped = status == VA_STATUS_SUCCESS;
    if (status == VA_STATUS_SUCCESS && (!params || !data)) status = VA_STATUS_ERROR_INVALID_PARAMETER;
    for (unsigned i = 0; i < count && status == VA_STATUS_SUCCESS; i++) {
        VASliceParameterBufferBase base; memcpy(&base, params + i * size, sizeof(base));
        if (base.slice_data_flag != VA_SLICE_DATA_FLAG_ALL || !base.slice_data_size ||
            base.slice_data_offset > data_size || base.slice_data_size > data_size - base.slice_data_offset) {
            status = VA_STATUS_ERROR_INVALID_PARAMETER;
            break;
        }
        if (mode) status = transform_slice(p, params + i * size, size, data + base.slice_data_offset);
        if (status == VA_STATUS_SUCCESS) { p->slices++; if (!mode) p->segments++; }
    }
    if (data_mapped) {
        VAStatus unmap = vaUnmapBuffer(p->display, data_id);
        if (status == VA_STATUS_SUCCESS) status = unmap;
    }
    VAStatus unmap = vaUnmapBuffer(p->display, parameter_id);
    return status == VA_STATUS_SUCCESS ? unmap : status;
}

VAStatus vaRenderPicture(VADisplay display, VAContextID context, VABufferID *buffers, int count)
{
    pthread_once(&initialize_once, initialize);
    if (mode < 0 || count <= 0 || !buffers) return VA_STATUS_ERROR_INVALID_PARAMETER;
    pthread_mutex_lock(&adapter_lock);
    struct pending_picture *p = picture(display, context, 1);
    VAStatus status = p ? p->error : VA_STATUS_ERROR_ALLOCATION_FAILED;
    for (int i = 0; i < count && status == VA_STATUS_SUCCESS; i++) {
        VABufferType type; unsigned size, elements;
        status = vaBufferInfo(display, context, buffers[i], &type, &size, &elements);
        if (status != VA_STATUS_SUCCESS) break;
        if (type == VASliceParameterBufferType) {
            if (i + 1 >= count) { status = VA_STATUS_ERROR_INVALID_PARAMETER; break; }
            status = slice_pair(p, buffers[i], buffers[i + 1]); i++;
        } else if (type == VASliceDataBufferType) status = VA_STATUS_ERROR_INVALID_PARAMETER;
        else if (mode) status = next_render(display, context, buffers + i, 1);
    }
    if (status == VA_STATUS_SUCCESS && !mode) status = next_render(display, context, buffers, count);
    if (p) p->error = status;
    pthread_mutex_unlock(&adapter_lock);
    return status;
}

VAStatus vaEndPicture(VADisplay display, VAContextID context)
{
    pthread_once(&initialize_once, initialize);
    if (mode < 0) return VA_STATUS_ERROR_INVALID_PARAMETER;
    pthread_mutex_lock(&adapter_lock);
    struct pending_picture *p = picture(display, context, 0);
    VAStatus status = p ? p->error : VA_STATUS_ERROR_INVALID_PARAMETER;
    if (p && !p->slices && status == VA_STATUS_SUCCESS) status = VA_STATUS_ERROR_INVALID_PARAMETER;
    const char *vendor = vaQueryVendorString(display);
    if (!vareq_test_driver_identity(vendor)) status = VA_STATUS_ERROR_OPERATION_FAILED;
    if (status == VA_STATUS_SUCCESS) status = next_end(display, context);
    unsigned slices = p ? p->slices : 0, segments = p ? p->segments : 0;
    if (p) {
        VAStatus cleanup = release_picture(p);
        if (status == VA_STATUS_SUCCESS) status = cleanup;
    }
    const char *export_mode = getenv("VAREQ_EXPBUF_EXPORT");
    if (fprintf(trace, "%s\t%u\t%u\t%u\t%d\t%s\t%s\n", mode_name, picture_number++,
        slices, segments, status, vendor ? vendor : "unknown", export_mode ? export_mode : "default") < 0 || fflush(trace))
        status = VA_STATUS_ERROR_OPERATION_FAILED;
    pthread_mutex_unlock(&adapter_lock);
    return status;
}

VAStatus vaDestroyContext(VADisplay display, VAContextID context)
{
    pthread_once(&initialize_once, initialize);
    if (!next_destroy) return VA_STATUS_ERROR_OPERATION_FAILED;
    pthread_mutex_lock(&adapter_lock);
    struct pending_picture *p = picture(display, context, 0);
    VAStatus cleanup = p ? release_picture(p) : VA_STATUS_SUCCESS;
    VAStatus status = next_destroy(display, context);
    pthread_mutex_unlock(&adapter_lock);
    return status == VA_STATUS_SUCCESS ? cleanup : status;
}
