/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Private EGL worker and raw-byte plane views adapted from Radxa (MIT).
 * The worker owns its GBM display and never changes caller EGL state. */
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <drm_fourcc.h>
#include "gpu_copy.h"
int vav4l2_gpu_copy_planes(const VADRMPRIMESurfaceDescriptor *d, unsigned width, unsigned height, struct vav4l2_gpu_plane out[2])
{
    if (!d || !width || !height || width > d->width || height > d->height ||
        (d->fourcc != VA_FOURCC_NV12 && d->fourcc != VA_FOURCC_P010) ||
        d->num_layers != 1 || d->layers[0].num_planes != 2 || !d->num_objects || d->num_objects > 4 ||
        d->layers[0].drm_format != (d->fourcc == VA_FOURCC_P010 ? DRM_FORMAT_P010 : DRM_FORMAT_NV12)) return -1;
    struct vav4l2_gpu_plane planes[2];
    for (unsigned p = 0; p < 2; p++) {
        unsigned object = d->layers[0].object_index[p];
        if (object >= d->num_objects || d->objects[object].fd < 0 || d->objects[object].drm_format_modifier != DRM_FORMAT_MOD_LINEAR) return -1;
        uint64_t row = (uint64_t)(p ? (width + 1u) & ~1u : width) * (d->fourcc == VA_FOURCC_P010 ? 2u : 1u);
        uint64_t columns = (row + 3) / 4, rows = p ? (height + 1u) / 2 : height;
        uint64_t pitch = d->layers[0].pitch[p], offset = d->layers[0].offset[p];
        if (!columns || columns > INT_MAX || rows > INT_MAX || pitch > INT_MAX || offset > INT_MAX || (pitch & 3) || (offset & 3) || columns * 4 > pitch ||
            offset + (rows - 1) * pitch + columns * 4 > d->objects[object].size) return -1;
        planes[p] = (struct vav4l2_gpu_plane){d->objects[object].fd, offset, pitch, columns, rows};
    }
    memcpy(out, planes, sizeof(planes)); return 0;
}
#ifdef VAV4L2_HAVE_GPU_COPY
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>
struct vav4l2_gpu_copy {
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t wake;
    int fd, stop, pending, result;
    struct vav4l2_gpu_plane source[2], target[2];
};
struct gpu_state {
    void *egl, *gles;
    PFNEGLGETPROCADDRESSPROC get_proc;
    PFNEGLINITIALIZEPROC initialize;
    PFNEGLBINDAPIPROC bind_api;
    PFNEGLCREATECONTEXTPROC create_context;
    PFNEGLMAKECURRENTPROC make_current;
    PFNEGLDESTROYCONTEXTPROC destroy_context;
    PFNEGLTERMINATEPROC terminate;
    PFNEGLRELEASETHREADPROC release_thread;
    PFNGLGETSTRINGPROC get_string;
    PFNGLGETSTRINGIPROC get_string_i;
    PFNGLGETINTEGERVPROC get_integer;
    PFNGLGENTEXTURESPROC gen_textures;
    PFNGLBINDTEXTUREPROC bind_texture;
    PFNGLTEXPARAMETERIPROC tex_parameter;
    PFNGLGETERRORPROC get_error;
    PFNGLCOPYIMAGESUBDATAPROC copy_image;
    PFNGLFINISHPROC finish;
    PFNGLDELETETEXTURESPROC delete_textures;
    struct gbm_device *gbm;
    EGLDisplay display;
    EGLContext context;
    int initialized;
    PFNEGLCREATEIMAGEKHRPROC create_image;
    PFNEGLDESTROYIMAGEKHRPROC destroy_image;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC bind_image;
};
static int gpu_init(struct gpu_state *g, int fd)
{
    /* Loading after VA initialization keeps dependency VA entry points out of
     * libva's driver-symbol lookup. RTLD_LOCAL also contains graphics symbols. */
    g->egl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
    g->gles = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_LOCAL);
    if (!g->egl || !g->gles) return -1;
#define EGL_LOAD(member, type, symbol) do { g->member = (type)dlsym(g->egl, symbol); if (!g->member) return -1; } while (0)
    EGL_LOAD(get_proc, PFNEGLGETPROCADDRESSPROC, "eglGetProcAddress");
    EGL_LOAD(initialize, PFNEGLINITIALIZEPROC, "eglInitialize");
    EGL_LOAD(bind_api, PFNEGLBINDAPIPROC, "eglBindAPI");
    EGL_LOAD(create_context, PFNEGLCREATECONTEXTPROC, "eglCreateContext");
    EGL_LOAD(make_current, PFNEGLMAKECURRENTPROC, "eglMakeCurrent");
    EGL_LOAD(destroy_context, PFNEGLDESTROYCONTEXTPROC, "eglDestroyContext");
    EGL_LOAD(terminate, PFNEGLTERMINATEPROC, "eglTerminate");
    EGL_LOAD(release_thread, PFNEGLRELEASETHREADPROC, "eglReleaseThread");
#undef EGL_LOAD
#define GL_LOAD(member, type, symbol) do { g->member = (type)g->get_proc(symbol); if (!g->member) return -1; } while (0)
    GL_LOAD(get_string, PFNGLGETSTRINGPROC, "glGetString");
    GL_LOAD(get_string_i, PFNGLGETSTRINGIPROC, "glGetStringi");
    GL_LOAD(get_integer, PFNGLGETINTEGERVPROC, "glGetIntegerv");
    GL_LOAD(gen_textures, PFNGLGENTEXTURESPROC, "glGenTextures");
    GL_LOAD(bind_texture, PFNGLBINDTEXTUREPROC, "glBindTexture");
    GL_LOAD(tex_parameter, PFNGLTEXPARAMETERIPROC, "glTexParameteri");
    GL_LOAD(get_error, PFNGLGETERRORPROC, "glGetError");
    GL_LOAD(finish, PFNGLFINISHPROC, "glFinish");
    GL_LOAD(delete_textures, PFNGLDELETETEXTURESPROC, "glDeleteTextures");
#undef GL_LOAD
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)g->get_proc("eglGetPlatformDisplayEXT");
    g->create_image = (PFNEGLCREATEIMAGEKHRPROC)g->get_proc("eglCreateImageKHR");
    g->destroy_image = (PFNEGLDESTROYIMAGEKHRPROC)g->get_proc("eglDestroyImageKHR");
    g->bind_image = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)g->get_proc("glEGLImageTargetTexture2DOES");
    if (!get_display || !g->create_image || !g->destroy_image || !g->bind_image || !(g->gbm = gbm_create_device(fd))) return -1;
    g->display = get_display(EGL_PLATFORM_GBM_KHR, g->gbm, NULL);
    if (g->display == EGL_NO_DISPLAY || !g->initialize(g->display, NULL, NULL)) return -1;
    g->initialized = 1;
    if (!g->bind_api(EGL_OPENGL_ES_API)) return -1;
    const EGLint attributes[] = {EGL_CONTEXT_MAJOR_VERSION_KHR, 3, EGL_CONTEXT_MINOR_VERSION_KHR, 0, EGL_NONE};
    g->context = g->create_context(g->display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attributes);
    if (g->context == EGL_NO_CONTEXT || !g->make_current(g->display, EGL_NO_SURFACE, EGL_NO_SURFACE, g->context)) return -1;
    if (!g->get_string(GL_VERSION)) return -1;
    GLint major = 0, minor = 0, count = 0;
    g->get_integer(GL_MAJOR_VERSION, &major); g->get_integer(GL_MINOR_VERSION, &minor);
    if (major > 3 || (major == 3 && minor >= 2))
        g->copy_image = (PFNGLCOPYIMAGESUBDATAPROC)g->get_proc("glCopyImageSubData");
    if (!g->copy_image) {
        g->get_integer(GL_NUM_EXTENSIONS, &count);
        for (GLint i = 0; i < count && !g->copy_image; i++) {
            const char *extension = (const char *)g->get_string_i(GL_EXTENSIONS, i);
            if (extension && !strcmp(extension, "GL_EXT_copy_image"))
                g->copy_image = (PFNGLCOPYIMAGESUBDATAPROC)g->get_proc("glCopyImageSubDataEXT");
            else if (extension && !strcmp(extension, "GL_OES_copy_image"))
                g->copy_image = (PFNGLCOPYIMAGESUBDATAPROC)g->get_proc("glCopyImageSubDataOES");
        }
    }
    return g->copy_image && g->get_error() == GL_NO_ERROR ? 0 : -1;
}
static int gpu_job(struct gpu_state *g, const struct vav4l2_gpu_plane source[2], const struct vav4l2_gpu_plane target[2])
{
    EGLImageKHR images[4] = {EGL_NO_IMAGE_KHR, EGL_NO_IMAGE_KHR, EGL_NO_IMAGE_KHR, EGL_NO_IMAGE_KHR};
    GLuint textures[4] = {0}; int result = -1;
    for (unsigned i = 0; i < 4; i++) {
        const struct vav4l2_gpu_plane *p = i < 2 ? &source[i] : &target[i - 2];
        if (vav4l2_dmabuf_wait(p->fd, i >= 2) < 0) goto done;
        const EGLint attributes[] = {EGL_WIDTH, p->columns, EGL_HEIGHT, p->rows, EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_ABGR8888,
            EGL_DMA_BUF_PLANE0_FD_EXT, p->fd, EGL_DMA_BUF_PLANE0_OFFSET_EXT, p->offset, EGL_DMA_BUF_PLANE0_PITCH_EXT, p->pitch,
            EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, 0, EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, 0, EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
        images[i] = g->create_image(g->display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attributes);
        if (images[i] == EGL_NO_IMAGE_KHR) goto done;
        g->gen_textures(1, &textures[i]); g->bind_texture(GL_TEXTURE_2D, textures[i]);
        g->tex_parameter(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); g->tex_parameter(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        g->tex_parameter(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); g->tex_parameter(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        g->bind_image(GL_TEXTURE_2D, images[i]);
        if (g->get_error() != GL_NO_ERROR) goto done;
    }
    for (unsigned p = 0; p < 2; p++) g->copy_image(textures[p], GL_TEXTURE_2D, 0, 0, 0, 0,
        textures[p + 2], GL_TEXTURE_2D, 0, 0, 0, 0, source[p].columns, source[p].rows, 1);
    g->finish(); result = g->get_error() == GL_NO_ERROR ? 0 : -1;
done:
    /* Complete even a partially submitted copy before deleting imported views. */
    g->finish(); g->delete_textures(4, textures);
    for (unsigned i = 0; i < 4; i++) if (images[i] != EGL_NO_IMAGE_KHR) g->destroy_image(g->display, images[i]);
    return result;
}
static void *gpu_worker(void *arg)
{
    struct vav4l2_gpu_copy *c = arg;
    struct gpu_state g = {.display = EGL_NO_DISPLAY, .context = EGL_NO_CONTEXT};
    int initialized = gpu_init(&g, c->fd) == 0;
    pthread_mutex_lock(&c->lock);
    for (;;) {
        while (!c->stop && !c->pending) pthread_cond_wait(&c->wake, &c->lock);
        if (c->stop) break;
        pthread_mutex_unlock(&c->lock);
        int result = initialized ? gpu_job(&g, c->source, c->target) : -1;
        pthread_mutex_lock(&c->lock); c->result = result; c->pending = 0; pthread_cond_broadcast(&c->wake);
    }
    pthread_mutex_unlock(&c->lock);
    if (g.context != EGL_NO_CONTEXT) { g.make_current(g.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT); g.destroy_context(g.display, g.context); }
    if (g.initialized) g.terminate(g.display);
    if (g.gbm) gbm_device_destroy(g.gbm);
    if (g.release_thread) g.release_thread();
    if (g.gles) dlclose(g.gles);
    if (g.egl) dlclose(g.egl);
    return NULL;
}
static int copy(struct vav4l2_driver_data *dd, const struct vav4l2_gpu_plane source[2], const struct vav4l2_gpu_plane target[2])
{
    if (!dd->gpu_copy) {
        int fd = dd->gbm.fd >= 0 ? dd->gbm.fd : dd->gbm.drm_fd;
        if (fd < 0) return -1;
        struct vav4l2_gpu_copy *c = calloc(1, sizeof(*c)); if (!c) return -1;
        c->fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
        if (c->fd < 0) { free(c); return -1; }
        if (pthread_mutex_init(&c->lock, NULL)) { close(c->fd); free(c); return -1; }
        if (pthread_cond_init(&c->wake, NULL)) { pthread_mutex_destroy(&c->lock); close(c->fd); free(c); return -1; }
        if (pthread_create(&c->thread, NULL, gpu_worker, c)) { pthread_cond_destroy(&c->wake); pthread_mutex_destroy(&c->lock); close(c->fd); free(c); return -1; }
        dd->gpu_copy = c;
    }
    struct vav4l2_gpu_copy *c = dd->gpu_copy;
    pthread_mutex_lock(&c->lock);
    memcpy(c->source, source, sizeof(c->source)); memcpy(c->target, target, sizeof(c->target)); c->pending = 1;
    pthread_cond_broadcast(&c->wake);
    while (c->pending) pthread_cond_wait(&c->wake, &c->lock);
    int result = c->result; pthread_mutex_unlock(&c->lock); return result;
}
void vav4l2_gpu_copy_finish(struct vav4l2_driver_data *dd)
{
    struct vav4l2_gpu_copy *c = dd->gpu_copy; if (!c) return;
    pthread_mutex_lock(&c->lock); c->stop = 1; pthread_cond_broadcast(&c->wake); pthread_mutex_unlock(&c->lock);
    pthread_join(c->thread, NULL); pthread_cond_destroy(&c->wake); pthread_mutex_destroy(&c->lock);
    close(c->fd); free(c); dd->gpu_copy = NULL;
}
#else
static int copy(struct vav4l2_driver_data *dd, const struct vav4l2_gpu_plane source[2], const struct vav4l2_gpu_plane target[2])
{ (void)dd; (void)source; (void)target; errno = ENOTSUP; return -1; }
void vav4l2_gpu_copy_finish(struct vav4l2_driver_data *dd) { (void)dd; }
#endif
int vav4l2_gpu_copy_capture(struct vav4l2_context *ctx, struct vav4l2_surface *s, int index)
{
    const char *mode = getenv("VAV4L2_GPU_COPY");
    if (!mode || !strcmp(mode, "0") || (!s->prime && !s->gbm_bo) || !ctx->driver_data ||
        (ctx->cap_pixelformat != V4L2_PIX_FMT_NV12 && ctx->cap_pixelformat != V4L2_PIX_FMT_P010)) return 0;
    if (ctx->capture_layout && !vav4l2_capture_layout_exportable(ctx)) return 0;
    if (strcmp(mode, "1") || (s->format != VA_FOURCC_NV12 && s->format != VA_FOURCC_P010)) return -1;
    VADRMPRIMESurfaceDescriptor source = {0}, target = {0};
    VAStatus status;
    if (ctx->capture_layout) {
        struct vav4l2_surface view = *s; view.buf_index = index;
        status = vav4l2_capture_layout_export(ctx, &view, VA_EXPORT_SURFACE_COMPOSED_LAYERS, &source);
    } else {
        int fd = vav4l2_capture_expbuf(ctx, index); unsigned size;
        if (fd < 0) return -1;
        if (vav4l2_export_fd_validate(fd, ctx->cap_sizeimage, &size) < 0) { close(fd); return -1; }
        int ten = ctx->cap_pixelformat == V4L2_PIX_FMT_P010;
        vav4l2_fill_prime_layers(&source, fd, size, DRM_FORMAT_MOD_LINEAR, ten ? VA_FOURCC_P010 : VA_FOURCC_NV12,
            s->width, s->height, ctx->cap_stride, ctx->cap_height, ten ? DRM_FORMAT_P010 : DRM_FORMAT_NV12,
            ten ? DRM_FORMAT_R16 : DRM_FORMAT_R8, ten ? DRM_FORMAT_GR1616 : DRM_FORMAT_GR88, VA_EXPORT_SURFACE_COMPOSED_LAYERS);
        status = VA_STATUS_SUCCESS;
    }
    if (status != VA_STATUS_SUCCESS) return -1;
    status = s->prime ? vav4l2_prime_export_backing(s, VA_EXPORT_SURFACE_COMPOSED_LAYERS, &target, 1) :
        vav4l2_surface_fill_prime_gbm(s, VA_EXPORT_SURFACE_COMPOSED_LAYERS | VA_EXPORT_SURFACE_WRITE_ONLY, &target);
    struct vav4l2_gpu_plane from[2], to[2]; int result = -1;
    if (status == VA_STATUS_SUCCESS && source.fourcc == target.fourcc &&
        !vav4l2_gpu_copy_planes(&source, s->width, s->height, from) && !vav4l2_gpu_copy_planes(&target, s->width, s->height, to)) {
        int alias = 0;
        for (unsigned a = 0; a < source.num_objects; a++) for (unsigned b = 0; b < target.num_objects; b++) {
            struct stat x, y;
            if (fstat(source.objects[a].fd, &x) < 0 || fstat(target.objects[b].fd, &y) < 0 || (x.st_dev == y.st_dev && x.st_ino == y.st_ino)) alias = 1;
        }
        if (!alias) {
            vav4l2_prime_device_written(s, 0);
            result = copy(ctx->driver_data, from, to);
            vav4l2_prime_device_written(s, result == 0);
        }
    }
    for (unsigned p = 0; p < source.num_objects; p++) close(source.objects[p].fd);
    for (unsigned p = 0; p < target.num_objects; p++) close(target.objects[p].fd);
    if (result < 0) return -1;
    s->buf_index = index; s->capture_ctx = ctx; s->cap_view = NULL; s->prime_capture_valid = 1;
    s->cap_fourcc = ctx->cap_pixelformat; s->stride = ctx->cap_stride; s->aligned_h = ctx->cap_height;
    vav4l2_decode_trace(ctx, "capture-gpu-copy", 0, index); return 1;
}
