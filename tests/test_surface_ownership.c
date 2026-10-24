/* Surface reuse across decoder/encoder contexts must not mix V4L2 queues. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "va_v4l2.h"

extern VAStatus __vaDriverInit_1_20(VADriverContextP ctx);
static int failures, ioctls;
static void expect(int ok, const char *tag)
{
    if (!ok) { fprintf(stderr, "FAIL %s\n", tag); failures++; }
}
static int count_ioctl(int fd, unsigned long request, void *arg)
{
    (void)fd; (void)request; (void)arg;
    ioctls++;
    return -1;
}
static struct vav4l2_context *context(struct vav4l2_driver_data *dd, enum vav4l2_codec codec)
{
    struct vav4l2_context *c = calloc(1, sizeof(*c));
    if (!c) exit(1);
    c->context_id = ++dd->next_context_id;
    c->driver_data = dd; c->codec = codec;
    c->v4l2_fd = c->media_fd = c->request_fd = -1;
    c->capture_bufs_allocd = 2;
    c->next = dd->contexts; dd->contexts = c;
    return c;
}
static int run(int expbuf)
{
    struct VADriverVTable v = {0};
    struct VADriverContext ctx = {.vtable = &v};
    setenv("VAV4L2_PROBE_NOCACHE", "1", 1);
    setenv("VAV4L2_EXPBUF_EXPORT", expbuf ? "1" : "0", 1);
    if (__vaDriverInit_1_20(&ctx) != VA_STATUS_SUCCESS) return 1;
    struct vav4l2_driver_data *dd = ctx.pDriverData;
    struct vav4l2_context *a = context(dd, VAV4L2_CODEC_H264);
    struct vav4l2_context *b = context(dd, VAV4L2_CODEC_HEVC);
    struct vav4l2_context *jpeg = context(dd, VAV4L2_CODEC_JPEG_ENC);
    VASurfaceID sid;
    if (v.vaCreateSurfaces(&ctx, 4, 4, VA_RT_FORMAT_YUV420, 1, &sid) != VA_STATUS_SUCCESS) return 1;
    struct vav4l2_surface *s = dd->surfaces[sid];
    unsigned char pixels[24] = {0};
    s->buf_index = 1; s->cap_view = pixels; s->capture_ctx = a;
    s->has_pic = 1; s->cap_fourcc = V4L2_PIX_FMT_NV12;
    s->stride = s->aligned_h = 4; s->timestamp = 7654321;
    s->last_writer = VAV4L2_WRITER_MEMFD;
    a->n_free_cap = 1; a->free_cap_bufs[0] = 0;
    b->n_free_cap = 1; b->free_cap_bufs[0] = 0;
    expect(v.vaBeginPicture(&ctx, jpeg->context_id, sid) == VA_STATUS_SUCCESS,
           "jpeg-begins-with-decoded-input");
    expect(s->timestamp == 7654321 && s->capture_ctx == a && s->buf_index == 1 &&
           s->cap_view == pixels && s->status == VASurfaceReady &&
           a->n_free_cap == 1 && jpeg->n_free_cap == 0 && jpeg->frame_count == 0,
           "jpeg-preserves-input-capture-and-reference-identity");
    expect(v.vaEndPicture(&ctx, jpeg->context_id) != VA_STATUS_SUCCESS &&
           s->status == VASurfaceReady && s->timestamp == 7654321 && s->capture_ctx == a,
           "failed-encode-does-not-invalidate-decoded-input");
    expect(v.vaBeginPicture(&ctx, b->context_id, sid) == VA_STATUS_SUCCESS,
           "surface-moves-to-second-decoder");
    expect(a->n_free_cap == 2 && a->free_cap_bufs[1] == 1 && b->n_free_cap == 1 &&
           s->buf_index == -1 && !s->cap_view && !s->capture_ctx,
           "slot-returns-to-original-context-only");
    b->current_surface = NULL;
    /* Same-context EXPBUF claims stay stable; the copy path recycles them. */
    s->buf_index = 1; s->cap_view = pixels; s->capture_ctx = b;
    expect(v.vaBeginPicture(&ctx, b->context_id, sid) == VA_STATUS_SUCCESS &&
           (expbuf ? (s->buf_index == 1 && s->capture_ctx == b && b->n_free_cap == 1) :
                     (s->buf_index == -1 && !s->capture_ctx && b->n_free_cap == 2)),
           "same-decoder-retains-expbuf-or-recycles-copy-slot");
    b->current_surface = NULL;
    if (expbuf) {
        VADRMPRIMESurfaceDescriptor desc = {0};
        s->last_writer = VAV4L2_WRITER_CPU;
        b->v4l2_fd = 97;
        setenv("VAV4L2_RENDER_NODE", "/vav4l2-test-no-render-node", 1);
        vav4l2_set_ioctl_hook(count_ioctl);
        expect(v.vaExportSurfaceHandle(&ctx, sid, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, 0, &desc) !=
               VA_STATUS_SUCCESS && ioctls == 0, "cpu-upload-never-exports-old-capture");
        vav4l2_set_ioctl_hook(NULL);
        b->v4l2_fd = -1;
        s->last_writer = VAV4L2_WRITER_MEMFD;
    }
    if (expbuf) {
        /* Submit helper must independently reject a foreign queue index. */
        a->v4l2_fd = 99; a->request_fd = 98;
        a->output_bufs_allocd = 1; a->output_buf_size = 64;
        a->current_surface = s;
        vav4l2_set_ioctl_hook(count_ioctl);
        expect(vav4l2_decode_submit(a, 0, 8, 1000) == -1 && ioctls == 0 &&
               a->n_free_out == 1 && a->free_out_bufs[0] == 0,
               "foreign-capture-never-reaches-qbuf");
        vav4l2_set_ioctl_hook(NULL);
        a->v4l2_fd = a->request_fd = -1; a->current_surface = NULL;
    }
    s->buf_index = 1; s->cap_view = pixels; s->capture_ctx = b;
    s->status = VASurfaceReady;
    expect(vav4l2_surface_ensure_cpu(s) == 0, "upload-backing");
    if (s->cpu_ptr) memset(s->cpu_ptr, 0x6c, s->cpu_size);
    s->last_writer = VAV4L2_WRITER_CPU;
    expect(v.vaDestroyContext(&ctx, b->context_id) == VA_STATUS_SUCCESS &&
           s->last_writer == VAV4L2_WRITER_CPU && s->status == VASurfaceReady &&
           s->cpu_ptr && ((unsigned char *)s->cpu_ptr)[0] == 0x6c &&
           s->buf_index == -1 && !s->capture_ctx && !s->cap_view,
           "teardown-keeps-newer-upload-instead-of-old-capture");
    s->buf_index = -1; s->cap_view = NULL; s->capture_ctx = NULL;
    v.vaTerminate(&ctx);
    return failures != 0;
}
int main(int argc, char **argv)
{
    if (argc == 2) return run(!strcmp(argv[1], "1"));
    /* The runtime export preference is cached once per process. */
    for (int mode = 0; mode <= 1; mode++) {
        pid_t pid = fork();
        if (pid < 0) return 1;
        if (!pid) {
            execl(argv[0], argv[0], mode ? "1" : "0", (char *)NULL);
            return 1;
        }
        int status;
        if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status))
            return 1;
    }
    return 0;
}
