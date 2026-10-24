/* Exercise the real Begin/Render/End/status API with deterministic V4L2 hooks. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "va_v4l2.h"
extern VAStatus __vaDriverInit_1_20(VADriverContextP);
static int failures, controls, submits, pulls, fail_submit, fail_pull, no_hold;
static uint8_t expected[256], output[256], remapped[256];
static size_t expected_size, first_size;
static uint64_t expected_timestamp;
static struct v4l2_ctrl_h264_decode_params submitted_params;
static struct v4l2_ctrl_h264_decode_params first_params;
static struct vav4l2_context *remap;
static void expect(int ok, const char *tag) { if (!ok) { fprintf(stderr, "FAIL fields %s\n", tag); failures++; } }
static int ioctl_hook(int fd, unsigned long request, void *arg)
{
    (void)fd;
    if (request == VIDIOC_CREATE_BUFS) {
        struct v4l2_create_buffers *query = arg;
        expect(!query->count && query->memory == V4L2_MEMORY_MMAP, "HOLD-query-does-not-allocate-or-release-queues");
        query->capabilities = no_hold ? 0 : V4L2_BUF_CAP_SUPPORTS_M2M_HOLD_CAPTURE_BUF;
    }
    if (request == VIDIOC_S_EXT_CTRLS) {
        struct v4l2_ext_controls *batch = arg; controls++;
        for (unsigned i = 0; i < batch->count; i++)
            if (batch->controls[i].id == V4L2_CID_STATELESS_H264_DECODE_PARAMS)
                submitted_params = *batch->controls[i].p_h264_decode_params;
    }
    return 0;
}
int __wrap_vav4l2_ensure_capture(struct vav4l2_context *c, int width, int height, uint32_t fourcc)
{
    expect(width == 16 && height == 32 && fourcc == V4L2_PIX_FMT_NV12, "paired-capture-keeps-full-frame-geometry");
    if (remap == c) { c->output_buf_ptr[0] = remapped; remap = NULL; }
    return 0;
}
int __wrap_vav4l2_decode_submit(struct vav4l2_context *c, int index, uint32_t bytes, uint64_t timestamp)
{
    submits++;
    expect(timestamp == expected_timestamp && bytes == expected_size && !memcmp(c->output_buf_ptr[index], expected, bytes),
        "one-output-has-both-owned-fields-and-first-timestamp");
    if (fail_submit) { vav4l2_decode_reset(c); return fail_submit; }
    vav4l2_out_pool_push(c, index); return 0;
}
int __wrap_vav4l2_decode_submit_field(struct vav4l2_context *c, int index, uint32_t bytes, uint64_t timestamp, int capture, int hold)
{
    submits++;
    size_t offset = hold ? 0 : first_size, length = hold ? first_size : expected_size - first_size;
    expect(timestamp == expected_timestamp && bytes == length && !memcmp(c->output_buf_ptr[index], expected + offset, bytes),
        "each-request-has-its-own-owned-field-and-shared-timestamp");
    expect(hold ? capture == -1 : capture == 1, "second-request-reuses-held-capture");
    if (hold) first_params = submitted_params;
    if (fail_submit) { vav4l2_decode_reset(c); return fail_submit; }
    vav4l2_out_pool_push(c, index); return 1;
}
int __wrap_vav4l2_surface_pull_capture(struct vav4l2_context *c, struct vav4l2_surface *s, int index)
{
    pulls++;
    if (fail_pull) return -1;
    s->has_pic = 1; s->capture_ctx = c; s->buf_index = index; s->last_writer = VAV4L2_WRITER_MEMFD;
    return 0;
}
struct writer { uint8_t bytes[64]; unsigned bits; };
static void bit(struct writer *w, unsigned x) { if (x) w->bytes[w->bits / 8] |= 128u >> (w->bits % 8); w->bits++; }
static void fixed(struct writer *w, unsigned x, unsigned n) { while (n) bit(w, (x >> --n) & 1); }
static void ue(struct writer *w, unsigned x) { unsigned n = 0, v = ++x; while (v >>= 1) n++; fixed(w, 0, n); fixed(w, x, n + 1); }
static size_t nal(uint8_t *out, unsigned bottom, unsigned frame, int fault)
{
    struct writer w = {0}; ue(&w, 0); ue(&w, 2); ue(&w, 0); fixed(&w, frame, 4);
    bit(&w, 1); bit(&w, bottom);
    if (fault == 6) ue(&w, 0);
    fixed(&w, bottom ? 3 : 2, 4);
    if (fault == 6) fixed(&w, 0, 2);
    else if (fault != 5) { bit(&w, fault == 7); if (fault == 7) { ue(&w, 5); ue(&w, 0); } }
    bit(&w, 1);
    memcpy(out, (uint8_t[]){0, 0, 1, fault == 5 ? 0x01 : fault == 6 ? 0x65 : 0x41}, 4);
    memcpy(out + 4, w.bytes, (w.bits + 7) / 8); return 4 + (w.bits + 7) / 8;
}
static struct vav4l2_context *context(struct vav4l2_driver_data *dd)
{
    struct vav4l2_context *c = calloc(1, sizeof(*c)); if (!c) exit(1);
    c->context_id = ++dd->next_context_id; c->driver_data = dd; c->codec = VAV4L2_CODEC_H264;
    c->profile = VAProfileH264Main; c->rt_format = VA_RT_FORMAT_YUV420;
    c->v4l2_fd = open("/dev/null", O_RDONLY); c->request_fd = open("/dev/null", O_RDONLY); c->media_fd = -1;
    c->output_bufs_allocd = c->n_free_out = 1; c->output_buf_ptr[0] = output; c->output_buf_size = sizeof(output);
    c->capture_bufs_allocd = 2; c->n_free_cap = 1; c->free_cap_bufs[0] = 1; c->streamed = 1;
    c->next = dd->contexts; dd->contexts = c; return c;
}
static VAStatus field(struct VADriverContext *driver, struct vav4l2_context *c, VASurfaceID surface,
    unsigned bottom, unsigned frame, int self, int corrupt)
{
    if (c->request_fd < 0) c->request_fd = open("/dev/null", O_RDONLY);
    VAStatus status = driver->vtable->vaBeginPicture(driver, c->context_id, surface);
    if (status) return status;
    uint8_t payload[64]; size_t bytes = nal(payload, bottom, frame, corrupt);
    if (!expected_size) { expected_timestamp = c->current_surface->timestamp; first_size = bytes; }
    if (!corrupt) { memcpy(expected + expected_size, payload, bytes); expected_size += bytes; }
    VAPictureParameterBufferH264 pic = {.frame_num = frame, .picture_height_in_mbs_minus1 = 1};
    pic.seq_fields.bits.chroma_format_idc = 1; pic.pic_fields.bits.field_pic_flag = pic.pic_fields.bits.reference_pic_flag = 1;
    if (corrupt == 5) pic.pic_fields.bits.reference_pic_flag = 0;
    pic.CurrPic = (VAPictureH264){.picture_id = surface, .TopFieldOrderCnt = bottom ? 0 : 2, .BottomFieldOrderCnt = bottom ? 3 : 0,
        .flags = bottom ? VA_PICTURE_H264_BOTTOM_FIELD : VA_PICTURE_H264_TOP_FIELD};
    for (unsigned i = 0; i < 16; i++) pic.ReferenceFrames[i].flags = VA_PICTURE_H264_INVALID;
    if (self) pic.ReferenceFrames[0] = (VAPictureH264){.picture_id = surface, .frame_idx = frame,
        .TopFieldOrderCnt = 2, .BottomFieldOrderCnt = 3,
        .flags = VA_PICTURE_H264_SHORT_TERM_REFERENCE | (bottom ? VA_PICTURE_H264_TOP_FIELD : VA_PICTURE_H264_BOTTOM_FIELD)};
    VASliceParameterBufferH264 slice = {.slice_type = 2, .slice_data_size = bytes};
    if (corrupt == 1) payload[3] |= 128;
    if (corrupt == 2) pic.pic_fields.bits.weighted_pred_flag = 1;
    if (corrupt == 3) pic.num_ref_frames = 1;
    VABufferID ids[3];
    expect(!driver->vtable->vaCreateBuffer(driver, c->context_id, VAPictureParameterBufferType, sizeof(pic), 1, &pic, &ids[0]) &&
        !driver->vtable->vaCreateBuffer(driver, c->context_id, VASliceParameterBufferType, sizeof(slice), 1, &slice, &ids[1]) &&
        !driver->vtable->vaCreateBuffer(driver, c->context_id, VASliceDataBufferType, bytes, 1, payload, &ids[2]), "owned-VA-buffer-fixture");
    expect(!driver->vtable->vaRenderPicture(driver, c->context_id, ids, 3), "render-field-parameters");
    if (corrupt == 4) expect(!driver->vtable->vaDestroyBuffer(driver, ids[2]), "destroy-pending-second-field-buffer");
    status = driver->vtable->vaEndPicture(driver, c->context_id);
    memset(payload, 0xee, sizeof(payload)); memset(&pic, 0xee, sizeof(pic));
    for (unsigned i = 0; i < 3; i++) if (corrupt != 4 || i != 2)
        expect(!driver->vtable->vaDestroyBuffer(driver, ids[i]), "client-can-destroy-all-first-field-buffers");
    return status;
}
static void pending_api(struct VADriverContext *d, struct vav4l2_context *c, VASurfaceID sid)
{
    struct VADriverVTable *v = d->vtable; VASurfaceStatus status;
    expect(v->vaQuerySurfaceStatus(d, sid, &status) == VA_STATUS_SUCCESS && status == VASurfaceRendering,
        "first-field-remains-rendering");
    expect(v->vaSyncSurface(d, sid) == VA_STATUS_ERROR_SURFACE_BUSY && v->vaSyncSurface2(d, sid, 0) == VA_STATUS_ERROR_TIMEDOUT,
        "first-field-sync-does-not-report-completed-pixels");
    VAImage image = {0}; VAImageFormat fmt = {.fourcc = VA_FOURCC_NV12};
    expect(!v->vaCreateImage(d, &fmt, 16, 32, &image), "pending-readback-image-fixture");
    expect(v->vaGetImage(d, sid, 0, 0, 16, 32, image.image_id) == VA_STATUS_ERROR_SURFACE_BUSY &&
        v->vaPutImage(d, sid, image.image_id, 0, 0, 16, 32, 0, 0, 16, 32) == VA_STATUS_ERROR_SURFACE_BUSY,
        "pending-field-cannot-read-old-content-or-overwrite-it");
    expect(!v->vaDestroyImage(d, image.image_id), "pending-readback-image-cleanup");
    expect(v->vaDeriveImage(d, sid, &image) == VA_STATUS_ERROR_SURFACE_BUSY, "pending-field-cannot-create-a-derived-borrow");
    VADRMPRIMESurfaceDescriptor descriptor;
    expect(v->vaExportSurfaceHandle(d, sid, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, VA_EXPORT_SURFACE_READ_ONLY, &descriptor) == VA_STATUS_ERROR_SURFACE_BUSY,
        "pending-field-cannot-export-a-completed-frame");
    struct vav4l2_context *other = context(d->pDriverData);
    enum vav4l2_codec codecs[] = {VAV4L2_CODEC_H264, VAV4L2_CODEC_VPP, VAV4L2_CODEC_JPEG_ENC};
    for (unsigned i = 0; i < sizeof(codecs) / sizeof(codecs[0]); i++) {
        other->codec = codecs[i];
        expect(v->vaBeginPicture(d, other->context_id, sid) == VA_STATUS_ERROR_SURFACE_BUSY && !other->current_surface,
            "pending-field-ownership-excludes-decoder-VPP-and-JPEG-contexts");
    }
    expect(!v->vaDestroyContext(d, other->context_id), "foreign-fixture-cleanup");
    expect(vav4l2_h264_field_pending(c, sid), "rejected-operations-keep-the-first-field");
    expect(v->vaBeginPicture(d, c->context_id, VA_INVALID_SURFACE) == VA_STATUS_ERROR_INVALID_SURFACE &&
        vav4l2_h264_field_pending(c, sid), "invalid-new-target-does-not-discard-a-valid-first-field");
}
int main(void)
{
    struct VADriverVTable v = {0}; struct VADriverContext driver = {.vtable = &v};
    setenv("VAV4L2_PROBE_NOCACHE", "1", 1); setenv("VAV4L2_EXPBUF_EXPORT", "1", 1);
    if (__vaDriverInit_1_20(&driver)) return 1;
    struct vav4l2_driver_data *dd = driver.pDriverData; struct vav4l2_context *c = context(dd);
    VASurfaceID surfaces[2]; if (v.vaCreateSurfaces(&driver, 16, 32, VA_RT_FORMAT_YUV420, 2, surfaces)) return 1;
    vav4l2_set_ioctl_hook(ioctl_hook);
    for (unsigned bottom = 0; bottom < 2; bottom++) {
        controls = submits = pulls = 0; expected_size = 0;
        expect(field(&driver, c, surfaces[0], bottom, 1, 0, 0) == VA_STATUS_SUCCESS && !controls && !submits && !pulls,
            "first-field-never-queues-controls-or-buffers");
        pending_api(&driver, c, surfaces[0]); remap = c;
        expect(field(&driver, c, surfaces[0], !bottom, 1, 1, 0) == VA_STATUS_SUCCESS && submits == 2 && pulls == 1 && !c->h264_field,
            "complementary-fields-use-HOLD-then-complete-in-both-field-orders");
        expect((submitted_params.flags & V4L2_H264_DECODE_PARAM_FLAG_FIELD_PIC) &&
            !!(first_params.flags & V4L2_H264_DECODE_PARAM_FLAG_BOTTOM_FIELD) == bottom &&
            !!(submitted_params.flags & V4L2_H264_DECODE_PARAM_FLAG_BOTTOM_FIELD) != bottom &&
            submitted_params.top_field_order_cnt == 2 && submitted_params.bottom_field_order_cnt == 3,
            "each-control-keeps-its-field-parity-and-final-valid-POCs");
        expect(!v.vaSyncSurface(&driver, surfaces[0]) && dd->surfaces[surfaces[0]]->status == VASurfaceReady,
            "only-the-complete-pair-becomes-ready");
    }
    expected_size = 0; no_hold = 1; controls = submits = 0;
    expect(field(&driver, c, surfaces[0], 0, 1, 0, 0) == VA_STATUS_ERROR_UNIMPLEMENTED && !submits && !controls && !c->h264_field,
        "missing-HOLD-capability-is-not-emulated-by-concatenation");
    no_hold = 0;
    for (int fault = 0; fault < 11; fault++) {
        expected_size = 0; controls = submits = pulls = 0;
        expect(!field(&driver, c, surfaces[0], 0, 2, 0, 0), "failure-fixture-first-field");
        if (fault == 3) fail_submit = -2;
        if (fault == 4) fail_pull = 1;
        VAStatus status = field(&driver, c, surfaces[0], fault == 0 ? 0 : 1, fault == 1 ? 3 : 2, 0,
            fault == 2 ? 1 : fault >= 5 ? fault - 3 : 0);
        VAStatus wanted = fault == 3 ? VA_STATUS_ERROR_DECODING_ERROR : fault == 4 ? VA_STATUS_ERROR_OPERATION_FAILED :
            fault == 7 ? VA_STATUS_ERROR_INVALID_BUFFER : fault == 5 || fault == 6 ? VA_STATUS_ERROR_UNIMPLEMENTED : VA_STATUS_ERROR_INVALID_PARAMETER;
        expect(status == wanted &&
            !c->h264_field && dd->surfaces[surfaces[0]]->status == VASurfaceSkipped, "bad-or-failed-second-field-clears-pair-and-surfaces-the-error");
        fail_submit = fail_pull = 0;
    }
    expected_size = 0; expect(!field(&driver, c, surfaces[0], 0, 4, 0, 0), "missing-field-first-stage");
    expect(!v.vaBeginPicture(&driver, c->context_id, surfaces[1]) && !c->h264_field && dd->surfaces[surfaces[0]]->status == VASurfaceSkipped,
        "new-target-cancels-an-unpaired-field");
    expect(v.vaEndPicture(&driver, c->context_id) != VA_STATUS_SUCCESS, "empty-new-picture-is-an-error");
    expected_size = 0; expect(!field(&driver, c, surfaces[0], 0, 4, 0, 0), "reset-first-stage");
    vav4l2_decode_reset(c);
    expect(!c->h264_field && dd->surfaces[surfaces[0]]->status == VASurfaceSkipped, "queue-reset-discards-the-unsubmitted-field");
    expected_size = 0; expect(!field(&driver, c, surfaces[0], 0, 5, 0, 0), "surface-destroy-first-stage");
    expect(!v.vaDestroySurfaces(&driver, surfaces, 1) && !c->h264_field, "surface-destruction-cancels-owned-field-by-id");
    expected_size = 0; expect(!field(&driver, c, surfaces[1], 0, 6, 0, 0), "context-destroy-first-stage");
    expect(!v.vaDestroyContext(&driver, c->context_id) && dd->surfaces[surfaces[1]]->status == VASurfaceSkipped,
        "context-destruction-releases-the-field-and-invalidates-incomplete-content");
    vav4l2_set_ioctl_hook(NULL); v.vaDestroySurfaces(&driver, surfaces + 1, 1); v.vaTerminate(&driver);
    return failures != 0;
}
