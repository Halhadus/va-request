/* An export created after DeriveImage must observe CPU writes on Unmap. */
#include <stdio.h>
#include <stdlib.h>
#include "va_v4l2.h"
extern VAStatus VAV4L2_DRIVER_INIT(VADriverContextP ctx);
static int calls, fail_upload;
int __wrap_vav4l2_gbm_surface_sync(struct vav4l2_surface *s)
{
    if (s->last_writer == VAV4L2_WRITER_CPU && s->cpu_ptr && ((unsigned char *)s->cpu_ptr)[0] == 0x73)
        calls++;
    return fail_upload ? -1 : 0;
}
int main(void)
{
    struct VADriverVTable v = {0};
    struct VADriverContext ctx = {.vtable = &v};
    setenv("VAV4L2_PROBE_NOCACHE", "1", 1);
    if (VAV4L2_DRIVER_INIT(&ctx)) return 1;
    VASurfaceID sid;
    VAImage derived;
    if (v.vaCreateSurfaces(&ctx, 4, 4, VA_RT_FORMAT_YUV420, 1, &sid)) return 1;
    struct vav4l2_surface *s = ((struct vav4l2_driver_data *)ctx.pDriverData)->surfaces[sid];
    if (vav4l2_surface_ensure_cpu(s) || v.vaDeriveImage(&ctx, sid, &derived)) return 1;
    unsigned char *pixels;
    if (v.vaMapBuffer(&ctx, derived.buf, (void **)&pixels)) return 1;
    pixels[0] = 0x73;
    s->gbm_bo = (struct gbm_bo *)s; /* wrapped sync; never passed to libgbm */
    int failed = v.vaUnmapBuffer(&ctx, derived.buf) != VA_STATUS_SUCCESS || calls != 1;
    fail_upload = 1;
    failed |= v.vaUnmapBuffer(&ctx, derived.buf) != VA_STATUS_ERROR_OPERATION_FAILED || calls != 2;
    s->gbm_bo = NULL;
    failed |= v.vaUnmapBuffer(&ctx, VA_INVALID_ID) != VA_STATUS_ERROR_INVALID_BUFFER;
    v.vaDestroyImage(&ctx, derived.image_id); v.vaDestroySurfaces(&ctx, &sid, 1); v.vaTerminate(&ctx);
    if (failed) fprintf(stderr, "FAIL derived image export coherence or failed upload propagation\n");
    return failed;
}
