#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "va_v4l2.h"
extern VAStatus VAV4L2_DRIVER_INIT(VADriverContextP ctx);
static pthread_mutex_t fixture_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fixture_ready = PTHREAD_COND_INITIALIZER;
static int wait_decode, entered, released;
VAStatus __wrap_vav4l2_vp8_translate(struct vav4l2_context *ctx, struct vav4l2_buffer **buffers, int count)
{
    pthread_mutex_lock(&fixture_lock);
    if (wait_decode) {
        entered = 1; pthread_cond_broadcast(&fixture_ready);
        while (!released) pthread_cond_wait(&fixture_ready, &fixture_lock);
    }
    pthread_mutex_unlock(&fixture_lock);
    (void)buffers; (void)count; ctx->current_surface->has_pic = 1; return VA_STATUS_SUCCESS;
}
static int failures;
static void expect(int ok, const char *tag)
{
    if (!ok) { fprintf(stderr, "FAIL %s\n", tag); failures++; }
}
static struct vav4l2_context *context(struct vav4l2_driver_data *dd, enum vav4l2_codec codec)
{
    struct vav4l2_context *c = calloc(1, sizeof(*c));
    if (!c) exit(1);
    c->context_id = ++dd->next_context_id; c->codec = codec; c->driver_data = dd;
    c->v4l2_fd = c->media_fd = c->request_fd = -1;
    c->next = dd->contexts; dd->contexts = c;
    return c;
}
struct api_job { VADriverContextP driver; VAContextID context; VAStatus result; int errors; };
static void *finish_picture(void *arg)
{
    struct api_job *job = arg; job->result = job->driver->vtable->vaEndPicture(job->driver, job->context); return NULL;
}
static void *surface_stress(void *arg)
{
    struct api_job *job = arg; VADriverContextP driver = job->driver;
    for (unsigned i = 0; i < 64; i++) {
        VASurfaceID ids[6];
        if (driver->vtable->vaCreateSurfaces(driver, 4, 4, VA_RT_FORMAT_YUV420, 6, ids)) { job->errors++; break; }
        for (unsigned j = 0; j < 6; j++) if (driver->vtable->vaSyncSurface2(driver, ids[j], VA_TIMEOUT_INFINITE)) job->errors++;
        if (driver->vtable->vaDestroySurfaces(driver, ids, 6)) { job->errors++; break; }
    }
    return NULL;
}
int main(void)
{
    struct VADriverVTable v = {0}; struct VADriverContext ctx = {.vtable = &v};
    setenv("VAV4L2_PROBE_NOCACHE", "1", 1);
    if (VAV4L2_DRIVER_INIT(&ctx)) return 1;
    struct vav4l2_driver_data *dd = ctx.pDriverData;
    struct vav4l2_context *a = context(dd, VAV4L2_CODEC_VPP), *b = context(dd, VAV4L2_CODEC_VPP);
    struct vav4l2_context *jpeg = context(dd, VAV4L2_CODEC_JPEG_ENC);
    VASurfaceID ids[2];
    if (v.vaCreateSurfaces(&ctx, 4, 4, VA_RT_FORMAT_YUV420, 2, ids)) return 1;
    struct vav4l2_surface *s = dd->surfaces[ids[0]];
    expect(v.vaBeginPicture(&ctx, a->context_id, ids[0]) == VA_STATUS_SUCCESS, "begin-first-picture");
    expect(v.vaSyncSurface(&ctx, ids[0]) == VA_STATUS_ERROR_SURFACE_BUSY && s->status == VASurfaceRendering,
           "sync-cannot-complete-an-unsubmitted-picture");
    expect(v.vaSyncSurface2 && v.vaSyncBuffer && v.vaSyncSurface2(&ctx, ids[0], 0) == VA_STATUS_ERROR_TIMEDOUT,
           "timed-sync-does-not-complete-unsubmitted-picture");
    expect(v.vaBeginPicture(&ctx, a->context_id, ids[1]) == VA_STATUS_ERROR_OPERATION_FAILED &&
           a->current_surface == s && dd->surfaces[ids[1]]->status == VASurfaceReady, "nested-begin-keeps-original-picture");
    expect(v.vaBeginPicture(&ctx, b->context_id, ids[0]) == VA_STATUS_ERROR_SURFACE_BUSY && !b->current_surface,
           "another-context-cannot-write-a-pending-target");
    expect(v.vaBeginPicture(&ctx, jpeg->context_id, ids[0]) == VA_STATUS_ERROR_SURFACE_BUSY,
           "encoder-cannot-read-a-pending-writer");
    expect(v.vaEndPicture(&ctx, a->context_id) == VA_STATUS_ERROR_INVALID_PARAMETER && !a->current_surface,
           "failed-end-releases-picture-state");
    expect(v.vaBeginPicture(&ctx, b->context_id, ids[0]) == VA_STATUS_SUCCESS, "target-reusable-after-failed-end");
    expect(v.vaEndPicture(&ctx, b->context_id) == VA_STATUS_ERROR_INVALID_PARAMETER, "finish-second-failed-picture");
    a->codec = VAV4L2_CODEC_VP8;
    a->v4l2_fd = open("/dev/null", O_RDWR | O_CLOEXEC); a->request_fd = dup(a->v4l2_fd);
    VASurfaceStatus status;
    expect(v.vaBeginPicture(&ctx, a->context_id, ids[0]) == VA_STATUS_SUCCESS &&
           v.vaEndPicture(&ctx, a->context_id) == VA_STATUS_SUCCESS &&
           v.vaQuerySurfaceStatus(&ctx, ids[0], &status) == VA_STATUS_SUCCESS && status == VASurfaceReady &&
           v.vaSyncSurface(&ctx, ids[0]) == VA_STATUS_SUCCESS, "completed-synchronous-decode-is-ready-before-sync");
    expect(v.vaSyncSurface2(&ctx, ids[0], 0) == VA_STATUS_SUCCESS &&
        v.vaSyncSurface2(&ctx, VA_INVALID_ID, 0) == VA_STATUS_ERROR_INVALID_SURFACE &&
        v.vaSyncBuffer(&ctx, VA_INVALID_ID, 0) == VA_STATUS_ERROR_INVALID_BUFFER, "timed-sync-validates-object-and-ready-state");
    VADRMPRIMESurfaceDescriptor descriptor, untouched; memset(&descriptor, 0xa5, sizeof(descriptor)); untouched = descriptor;
    expect(v.vaExportSurfaceHandle(&ctx, ids[0], VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, 0x8000, &descriptor) == VA_STATUS_ERROR_INVALID_PARAMETER &&
        v.vaExportSurfaceHandle(&ctx, ids[0], VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
            VA_EXPORT_SURFACE_SEPARATE_LAYERS | VA_EXPORT_SURFACE_COMPOSED_LAYERS, &descriptor) == VA_STATUS_ERROR_INVALID_PARAMETER && !memcmp(&descriptor, &untouched, sizeof(descriptor)),
        "export-rejects-invalid-layout-without-mutating-descriptor");
    VABufferID coded, picture;
    VAEncPictureParameterBufferJPEG jp = {.coded_buf = VA_INVALID_ID};
    expect(v.vaCreateBuffer(&ctx, jpeg->context_id, VAEncCodedBufferType, 64, 1, NULL, &coded) == VA_STATUS_SUCCESS, "sync-buffer-create");
    jp.coded_buf = coded;
    expect(v.vaCreateBuffer(&ctx, jpeg->context_id, VAEncPictureParameterBufferType, sizeof(jp), 1, &jp, &picture) == VA_STATUS_SUCCESS &&
        v.vaBeginPicture(&ctx, jpeg->context_id, ids[0]) == VA_STATUS_SUCCESS &&
        v.vaRenderPicture(&ctx, jpeg->context_id, &picture, 1) == VA_STATUS_SUCCESS &&
        v.vaSyncBuffer(&ctx, coded, 0) == VA_STATUS_ERROR_TIMEDOUT, "sync-buffer-detects-pending-encoder-output");
    VAStatus encoded = v.vaEndPicture(&ctx, jpeg->context_id);
    expect(encoded != VA_STATUS_SUCCESS && v.vaSyncBuffer(&ctx, coded, 0) == encoded, "sync-buffer-propagates-encoder-failure");
    v.vaDestroyBuffer(&ctx, picture);
    wait_decode = 1; entered = released = 0;
    expect(v.vaBeginPicture(&ctx, a->context_id, ids[0]) == VA_STATUS_SUCCESS, "concurrent-sync-begin");
    struct api_job blocked = {.driver = &ctx, .context = a->context_id}; pthread_t decoding;
    int created = pthread_create(&decoding, NULL, finish_picture, &blocked) == 0;
    expect(created, "concurrent-sync-thread");
    if (created) {
        pthread_mutex_lock(&fixture_lock); while (!entered) pthread_cond_wait(&fixture_ready, &fixture_lock); pthread_mutex_unlock(&fixture_lock);
        expect(v.vaSyncSurface2(&ctx, ids[0], 0) == VA_STATUS_ERROR_TIMEDOUT &&
            v.vaSyncBuffer(&ctx, coded, 0) == VA_STATUS_ERROR_TIMEDOUT, "zero-timeout-does-not-wait-for-decoder-global-lock");
        pthread_mutex_lock(&fixture_lock); released = 1; pthread_cond_broadcast(&fixture_ready); pthread_mutex_unlock(&fixture_lock);
        pthread_join(decoding, NULL); expect(blocked.result == VA_STATUS_SUCCESS, "concurrent-decode-finishes-after-sync-poll");
    }
    wait_decode = 0; v.vaDestroyBuffer(&ctx, coded);
    VASurfaceID invalid[] = {ids[0], VA_INVALID_ID}, duplicate[] = {ids[0], ids[0]};
    expect(v.vaDestroySurfaces(&ctx, invalid, 2) == VA_STATUS_ERROR_INVALID_SURFACE && dd->surfaces[ids[0]] == s,
           "invalid-destroy-list-is-atomic");
    expect(v.vaDestroySurfaces(&ctx, duplicate, 2) == VA_STATUS_ERROR_INVALID_SURFACE && dd->surfaces[ids[0]] == s,
           "duplicate-destroy-list-is-rejected-before-free");
    expect(v.vaDestroySurfaces(&ctx, NULL, 1) == VA_STATUS_ERROR_INVALID_PARAMETER &&
           v.vaDestroySurfaces(&ctx, ids, -1) == VA_STATUS_ERROR_INVALID_PARAMETER &&
           v.vaDestroySurfaces(&ctx, NULL, 0) == VA_STATUS_SUCCESS, "destroy-count-and-pointer-contract");
    expect(v.vaQuerySurfaceStatus(&ctx, ids[0], NULL) == VA_STATUS_ERROR_INVALID_PARAMETER, "null-status-output");
    expect(v.vaQueryConfigEntrypoints(&ctx, VAProfileNone, NULL, NULL) == VA_STATUS_ERROR_INVALID_PARAMETER,
           "null-entrypoint-count-output");
    void *error = s;
    expect(v.vaQuerySurfaceError(&ctx, VA_INVALID_ID, VA_STATUS_ERROR_DECODING_ERROR, &error) == VA_STATUS_ERROR_INVALID_SURFACE && !error,
           "surface-error-query-validates-id");
    expect(v.vaQuerySurfaceError(&ctx, ids[0], VA_STATUS_ERROR_DECODING_ERROR, NULL) == VA_STATUS_ERROR_INVALID_PARAMETER,
           "null-error-info-output");
    snprintf(dd->dev_h264, sizeof(dd->dev_h264), "%s", "/dev/mock-h264-profile-menu");
    dd->h264_profiles_known = 1;
    dd->h264_profiles = (1u << V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE) | (1u << V4L2_MPEG_VIDEO_H264_PROFILE_MAIN) |
                        (1u << V4L2_MPEG_VIDEO_H264_PROFILE_HIGH) | (1u << V4L2_MPEG_VIDEO_H264_PROFILE_HIGH_10_INTRA);
    VAEntrypoint ep; int num_ep = 1; VAConfigID config = VA_INVALID_ID;
    VAConfigAttrib attr = {.type = VAConfigAttribRTFormat};
    expect(v.vaQueryConfigEntrypoints(&ctx, VAProfileH264High10, &ep, &num_ep) == VA_STATUS_ERROR_UNSUPPORTED_PROFILE && !num_ep &&
           v.vaGetConfigAttributes(&ctx, VAProfileH264High422, VAEntrypointVLD, &attr, 1) == VA_STATUS_ERROR_UNSUPPORTED_PROFILE &&
           v.vaCreateConfig(&ctx, VAProfileH264High10, VAEntrypointVLD, NULL, 0, &config) == VA_STATUS_ERROR_UNSUPPORTED_PROFILE,
           "profile-menu-excludes-unsupported-general-high10-and-high422");
    VAProfile profiles[32]; int num_profiles = 0;
    expect(v.vaQueryConfigProfiles(&ctx, profiles, &num_profiles) == VA_STATUS_SUCCESS, "query-runtime-profile-list");
    for (int i = 0; i < num_profiles; i++) expect(profiles[i] != VAProfileH264High10 && profiles[i] != VAProfileH264High422,
        "profile-list-does-not-overclaim-an-intra-only-or-missing-profile");
    dd->h264_profiles |= (1u << V4L2_MPEG_VIDEO_H264_PROFILE_HIGH_10) | (1u << V4L2_MPEG_VIDEO_H264_PROFILE_HIGH_422);
    expect(v.vaCreateConfig(&ctx, VAProfileH264High10, VAEntrypointVLD, NULL, 0, &config) == VA_STATUS_SUCCESS,
           "runtime-general-high10-profile-is-usable");
    if (config != VA_INVALID_ID) v.vaDestroyConfig(&ctx, config);
    snprintf(dd->dev_hevc, sizeof(dd->dev_hevc), "%s", "/dev/mock-hevc-profile-menu");
    dd->hevc_profiles_known = 1; dd->hevc_profiles = 1u << V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN;
    expect(v.vaCreateConfig(&ctx, VAProfileHEVCMain10, VAEntrypointVLD, NULL, 0, &config) == VA_STATUS_ERROR_UNSUPPORTED_PROFILE,
        "hevc-main-node-does-not-advertise-main10");
    dd->hevc_profiles |= 1u << V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN_10;
    expect(v.vaCreateConfig(&ctx, VAProfileHEVCMain10, VAEntrypointVLD, NULL, 0, &config) == VA_STATUS_SUCCESS,
        "hevc-main10-menu-profile-is-usable");
    if (config != VA_INVALID_ID) v.vaDestroyConfig(&ctx, config);
    pthread_t workers[4]; struct api_job jobs[4] = {{0}}; unsigned count = 0;
    for (; count < 4; count++) { jobs[count].driver = &ctx; if (pthread_create(&workers[count], NULL, surface_stress, &jobs[count])) break; }
    expect(count == 4, "concurrent-surface-stress-threads");
    for (unsigned i = 0; i < count; i++) { pthread_join(workers[i], NULL); expect(!jobs[i].errors, "concurrent-surface-allocation-sync-destruction"); }
    expect(v.vaDestroySurfaces(&ctx, ids, 2) == VA_STATUS_SUCCESS, "destroy-valid-surfaces");
    v.vaTerminate(&ctx);
    return failures != 0;
}
