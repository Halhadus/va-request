/* Use real libva metadata dispatch and the real bridge vtable. No vaBufferInfo
 * mock: a missing registration must be observable before any VPU operation. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "va_v4l2.h"
#include "driver_identity.h"

extern VAStatus VAV4L2_DRIVER_INIT(VADriverContextP ctx);
extern VAStatus vaBufferInfo(VADisplay display, VAContextID context, VABufferID id,
                              VABufferType *type, unsigned *size, unsigned *elements);
static int failures;
static void expect(int ok, const char *tag)
{
    if (!ok) { fprintf(stderr, "FAIL %s\n", tag); failures++; }
}
static void metadata(VADisplay display, VAContextID context, VABufferID id,
                      VABufferType want_type, unsigned want_size, unsigned want_count)
{
    VABufferType type = VAEncCodedBufferType; unsigned size = 0, count = UINT32_MAX;
    expect(vaBufferInfo(display, context, id, &type, &size, &count) == VA_STATUS_SUCCESS &&
           type == want_type && size == want_size && count == want_count, "real-libva-reports-current-element-metadata");
}
static void adapter_dispatch(VADisplay display, struct vav4l2_driver_data *dd, int h264)
{
    struct vav4l2_context *context = calloc(1, sizeof(*context));
    expect(context != NULL, "adapter-context-fixture");
    if (!context) return;
    struct vav4l2_surface surface = {.width = 8, .height = 8, .status = VASurfaceRendering};
    context->context_id = ++dd->next_context_id; context->driver_data = dd; context->codec = h264 ? VAV4L2_CODEC_H264 : VAV4L2_CODEC_HEVC;
    context->v4l2_fd = context->media_fd = context->request_fd = -1; context->current_surface = &surface;
    context->next = dd->contexts; dd->contexts = context;
    VAPictureParameterBufferHEVC pic = {0}; uint8_t payload[] = {0x26, 1, 0xaa, 0xbb};
    VASliceParameterBufferHEVC slice = {.slice_data_size = sizeof(payload)}; VABufferID ids[3];
    VAPictureParameterBufferH264 h264_pic = {0}; VASliceParameterBufferH264 h264_slice = {.slice_data_size = sizeof(payload)};
    if (h264) payload[0] = 0x65;
    if (vaCreateBuffer(display, context->context_id, VAPictureParameterBufferType, h264 ? sizeof(h264_pic) : sizeof(pic), 1, h264 ? (void *)&h264_pic : (void *)&pic, &ids[0]) ||
        vaCreateBuffer(display, context->context_id, VASliceParameterBufferType, h264 ? sizeof(h264_slice) : sizeof(slice), 1, h264 ? (void *)&h264_slice : (void *)&slice, &ids[1]) ||
        vaCreateBuffer(display, context->context_id, VASliceDataBufferType, sizeof(payload), 1, payload, &ids[2])) {
        expect(0, "adapter-buffer-fixture"); vaDestroyContext(display, context->context_id); return;
    }
    expect(vaRenderPicture(display, context->context_id, ids, 3) == VA_STATUS_SUCCESS && context->num_pending_buffers == 3,
           "real-preload-libva-driver-metadata-render-path");
    /* No VPU in this fixture: EndPicture must return the device-unavailable error,
     * never SIGSEGV or a fabricated decode success. */
    expect(vaEndPicture(display, context->context_id) == VA_STATUS_ERROR_OPERATION_FAILED &&
           !context->num_pending_buffers && !context->current_surface && surface.status == VASurfaceSkipped,
           "real-adapter-reaches-clean-device-unavailable-error");
    for (unsigned i = 0; i < 3; i++) expect(vaDestroyBuffer(display, ids[i]) == VA_STATUS_SUCCESS, "adapter-original-buffer-cleanup");
    expect(vaDestroyContext(display, context->context_id) == VA_STATUS_SUCCESS, "adapter-context-cleanup");
}
int main(int argc, char **argv)
{
    struct VADriverVTable v = {0}; struct VADriverContext driver = {.vtable = &v};
    struct VADisplayContext display = {.vadpy_magic = VA_DISPLAY_MAGIC, .pDriverContext = &driver};
    setenv("VAV4L2_PROBE_NOCACHE", "1", 1);
    if (VAV4L2_DRIVER_INIT(&driver)) return 1;
    expect(vav4l2_test_driver_identity(driver.str_vendor), "acceptance-client-recognizes-current-driver");
    expect(!vav4l2_test_driver_identity(NULL) && !vav4l2_test_driver_identity("another-driver") && !vav4l2_test_driver_identity("other libva-v4l2 0.1.0"), "acceptance-client-rejects-unrelated-drivers");
    expect(v.vaBufferInfo != NULL, "buffer-info-callback-is-registered");
    if (!v.vaBufferInfo) { v.vaTerminate(&driver); return 1; }
    struct vav4l2_driver_data *dd = driver.pDriverData;
    struct vav4l2_context *context = calloc(1, sizeof(*context));
    expect(context != NULL, "context-fixture");
    if (!context) { v.vaTerminate(&driver); return 1; }
    context->context_id = ++dd->next_context_id; context->driver_data = dd;
    context->v4l2_fd = context->media_fd = context->request_fd = -1;
    context->next = dd->contexts; dd->contexts = context;
    VAContextID cid = context->context_id;
    VASliceParameterBufferHEVC params[3] = {{.slice_data_size = 4}, {.slice_data_size = 8}, {.slice_data_size = 12}};
    uint8_t payload[16] = {0x26, 1, 0xaa}; VABufferID parameter_id, data_id, coded_id;
    if (vaCreateBuffer(&display, cid, VASliceParameterBufferType, sizeof(params[0]), 3, params, &parameter_id) ||
        vaCreateBuffer(&display, VA_INVALID_ID, VASliceDataBufferType, 4, 4, payload, &data_id) ||
        vaCreateBuffer(&display, cid, VAEncCodedBufferType, 128, 1, NULL, &coded_id)) {
        v.vaTerminate(&driver); return 1;
    }
    metadata(&display, cid, parameter_id, VASliceParameterBufferType, sizeof(params[0]), 3);
    metadata(&display, cid, data_id, VASliceDataBufferType, 4, 4);
    metadata(&display, cid, coded_id, VAEncCodedBufferType, 128, 1);
    void *mapped = NULL;
    expect(vaMapBuffer(&display, parameter_id, &mapped) == VA_STATUS_SUCCESS && !memcmp(mapped, params, sizeof(params)),
           "metadata-query-does-not-change-content");
    expect(vaUnmapBuffer(&display, parameter_id) == VA_STATUS_SUCCESS, "metadata-query-keeps-buffer-mappable");
    expect(vaBufferSetNumElements(&display, parameter_id, 1) == VA_STATUS_SUCCESS, "shrink-logical-elements");
    metadata(&display, cid, parameter_id, VASliceParameterBufferType, sizeof(params[0]), 1);
    expect(vaBufferSetNumElements(&display, parameter_id, 0) == VA_STATUS_SUCCESS, "zero-logical-elements");
    metadata(&display, cid, parameter_id, VASliceParameterBufferType, sizeof(params[0]), 0);
    expect(vaBufferSetNumElements(&display, parameter_id, 5) == VA_STATUS_SUCCESS, "grow-buffer-allocation");
    metadata(&display, cid, parameter_id, VASliceParameterBufferType, sizeof(params[0]), 5);
    VABufferType type = VAEncCodedBufferType; unsigned size = 123, count = 456;
    expect(vaBufferInfo(&display, cid, parameter_id, NULL, &size, &count) == VA_STATUS_ERROR_INVALID_PARAMETER &&
           vaBufferInfo(&display, cid, parameter_id, &type, NULL, &count) == VA_STATUS_ERROR_INVALID_PARAMETER &&
           vaBufferInfo(&display, cid, parameter_id, &type, &size, NULL) == VA_STATUS_ERROR_INVALID_PARAMETER &&
           type == VAEncCodedBufferType && size == 123 && count == 456, "null-output-contract-no-partial-metadata-write");
    expect(vaBufferInfo(&display, cid, VA_INVALID_ID, &type, &size, &count) == VA_STATUS_ERROR_INVALID_BUFFER &&
           type == VAEncCodedBufferType && size == 123 && count == 456, "invalid-buffer-keeps-outputs-untouched");
    expect(vaDestroyBuffer(&display, parameter_id) == VA_STATUS_SUCCESS &&
           vaBufferInfo(&display, cid, parameter_id, &type, &size, &count) == VA_STATUS_ERROR_INVALID_BUFFER, "destroyed-buffer-metadata-is-invalid");
    expect(vaDestroyContext(&display, cid) == VA_STATUS_SUCCESS &&
           vaBufferInfo(&display, cid, coded_id, &type, &size, &count) == VA_STATUS_ERROR_INVALID_BUFFER, "context-destruction-removes-buffer-metadata");
    metadata(&display, VA_INVALID_ID, data_id, VASliceDataBufferType, 4, 4);
    expect(vaDestroyBuffer(&display, data_id) == VA_STATUS_SUCCESS, "destroy-orphan-buffer");
    VAImageFormat fmt = {.fourcc = VA_FOURCC_NV12}; VAImage image = {0};
    VAStatus image_status = vaCreateImage(&display, &fmt, 8, 8, &image);
    expect(image_status == VA_STATUS_SUCCESS, "image-buffer-fixture");
    if (image_status == VA_STATUS_SUCCESS) {
        metadata(&display, VA_INVALID_ID, image.buf, VAImageBufferType, image.data_size, 1);
        expect(vaDestroyImage(&display, image.image_id) == VA_STATUS_SUCCESS, "image-buffer-cleanup");
    }
    VASurfaceID sid;
    if (vaCreateSurfaces(&display, VA_RT_FORMAT_YUV420, 8, 8, &sid, 1, NULL, 0) == VA_STATUS_SUCCESS) {
        expect(vav4l2_surface_ensure_cpu(dd->surfaces[sid]) == 0, "derived-image-cpu-fixture");
        VAStatus derived = vaDeriveImage(&display, sid, &image);
        expect(derived == VA_STATUS_SUCCESS, "derived-image-buffer-fixture");
        if (derived == VA_STATUS_SUCCESS) {
            metadata(&display, VA_INVALID_ID, image.buf, VAImageBufferType, image.data_size, 1);
            expect(vaDestroyImage(&display, image.image_id) == VA_STATUS_SUCCESS, "derived-image-buffer-cleanup");
        }
        expect(vaDestroySurfaces(&display, &sid, 1) == VA_STATUS_SUCCESS, "derived-surface-cleanup");
    } else expect(0, "derived-image-surface-fixture");
    expect(vaBufferInfo(NULL, VA_INVALID_ID, data_id, &type, &size, &count) == VA_STATUS_ERROR_INVALID_DISPLAY,
           "real-libva-invalid-display-guard");
    if (argc == 2 && !strcmp(argv[1], "--adapter")) {
        adapter_dispatch(&display, dd, 0); adapter_dispatch(&display, dd, 1);
    }
    v.vaTerminate(&driver);
    return failures != 0;
}
