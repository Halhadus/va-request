/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <assert.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <drm_fourcc.h>
#include <linux/dma-buf.h>
#include "memory/gpu_copy.h"
static pthread_t caller;
static unsigned worker_inits;
static int source_fd, target_fd;
static int mock_gpu;
#ifdef VAV4L2_HAVE_GPU_COPY
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>
static unsigned copies, images, destroyed_images, contexts;
static char egl_handle, gles_handle;
static EGLBoolean mock_initialize(EGLDisplay d, EGLint *major, EGLint *minor)
{ (void)d; (void)major; (void)minor; return EGL_TRUE; }
static EGLBoolean mock_bind_api(EGLenum api) { assert(api == EGL_OPENGL_ES_API); return EGL_TRUE; }
static EGLContext mock_context(EGLDisplay d, EGLConfig config, EGLContext share, const EGLint *attributes)
{
    (void)d; (void)config; assert(share == EGL_NO_CONTEXT);
    assert(attributes[0] == EGL_CONTEXT_MAJOR_VERSION_KHR && attributes[1] == 3);
    assert(attributes[2] == EGL_CONTEXT_MINOR_VERSION_KHR && attributes[3] == 0 && attributes[4] == EGL_NONE);
    contexts++; return (EGLContext)1;
}
static EGLBoolean mock_current(EGLDisplay d, EGLSurface draw, EGLSurface read, EGLContext context)
{ (void)d; (void)draw; (void)read; (void)context; return EGL_TRUE; }
static EGLBoolean mock_destroy_context(EGLDisplay d, EGLContext context) { (void)d; (void)context; return EGL_TRUE; }
static EGLBoolean mock_terminate(EGLDisplay d) { (void)d; return EGL_TRUE; }
static EGLBoolean mock_release(void) { return EGL_TRUE; }
static EGLDisplay mock_display(EGLenum platform, void *native, const EGLint *attributes)
{ (void)native; (void)attributes; assert(platform == EGL_PLATFORM_GBM_KHR); return (EGLDisplay)1; }
static EGLImageKHR mock_image(EGLDisplay d, EGLContext context, EGLenum target, EGLClientBuffer buffer, const EGLint *attributes)
{ (void)d; (void)context; (void)buffer; (void)attributes; assert(target == EGL_LINUX_DMA_BUF_EXT); return (EGLImageKHR)(uintptr_t)++images; }
static EGLBoolean mock_destroy_image(EGLDisplay d, EGLImageKHR image) { (void)d; assert(image != EGL_NO_IMAGE_KHR); destroyed_images++; return EGL_TRUE; }
static const GLubyte *mock_string(GLenum name) { assert(name == GL_VERSION); return (const GLubyte *)"OpenGL ES 3.x fixture"; }
static const GLubyte *mock_string_i(GLenum name, GLuint index)
{
    assert(name == GL_EXTENSIONS && !index);
    return (const GLubyte *)(mock_gpu == 2 || mock_gpu == 6 ? "GL_EXT_copy_image" : mock_gpu == 3 ? "GL_OES_copy_image" : "GL_EXT_copy_image_extra");
}
static void mock_integer(GLenum name, GLint *value)
{
    if (name == GL_MAJOR_VERSION) *value = 3;
    else if (name == GL_MINOR_VERSION) *value = mock_gpu == 1 ? 2 : mock_gpu == 3 ? 0 : 1;
    else { assert(name == GL_NUM_EXTENSIONS); *value = mock_gpu == 4 ? 0 : 1; }
}
static void mock_textures(GLsizei count, GLuint *textures) { for (GLsizei i = 0; i < count; i++) textures[i] = images; }
static void mock_bind(GLenum target, GLuint texture) { assert(target == GL_TEXTURE_2D && texture); }
static void mock_parameter(GLenum target, GLenum name, GLint value) { (void)name; (void)value; assert(target == GL_TEXTURE_2D); }
static GLenum mock_error(void) { return GL_NO_ERROR; }
static void mock_bind_image(GLenum target, void *image) { assert(target == GL_TEXTURE_2D && image); }
static void mock_finish(void) {}
static void mock_delete(GLsizei count, const GLuint *textures) { (void)textures; assert(count == 4); }
static void mock_copy(GLuint src, GLenum src_target, GLint src_level, GLint sx, GLint sy, GLint sz,
    GLuint dst, GLenum dst_target, GLint dst_level, GLint dx, GLint dy, GLint dz, GLsizei width, GLsizei height, GLsizei depth)
{
    assert(src && dst && src_target == GL_TEXTURE_2D && dst_target == GL_TEXTURE_2D && !src_level && !dst_level);
    assert(!sx && !sy && !sz && !dx && !dy && !dz && width == 16 && (height == 32 || height == 16) && depth == 1); copies++;
}
void *__wrap_dlsym(void *, const char *);
static __eglMustCastToProperFunctionPointerType mock_proc(const char *name)
{ return (__eglMustCastToProperFunctionPointerType)__wrap_dlsym(&egl_handle, name); }
#endif
void *__real_dlopen(const char *, int);
void *__real_dlsym(void *, const char *);
int __real_dlclose(void *);
void *__wrap_dlopen(const char *name, int flags)
{
#ifdef VAV4L2_HAVE_GPU_COPY
    if (mock_gpu) { assert(flags == (RTLD_NOW | RTLD_LOCAL)); return !strcmp(name, "libEGL.so.1") ? &egl_handle : &gles_handle; }
#endif
    return __real_dlopen(name, flags);
}
int __wrap_dlclose(void *handle)
{
#ifdef VAV4L2_HAVE_GPU_COPY
    if (handle == &egl_handle || handle == &gles_handle) return 0;
#endif
    return __real_dlclose(handle);
}
void *__wrap_dlsym(void *handle, const char *name)
{
#ifdef VAV4L2_HAVE_GPU_COPY
    if (handle == &egl_handle || handle == &gles_handle) {
#define SYMBOL(symbol, function) if (!strcmp(name, symbol)) return (void *)function
        SYMBOL("eglGetProcAddress", mock_proc); SYMBOL("eglInitialize", mock_initialize); SYMBOL("eglBindAPI", mock_bind_api);
        SYMBOL("eglCreateContext", mock_context); SYMBOL("eglMakeCurrent", mock_current); SYMBOL("eglDestroyContext", mock_destroy_context);
        SYMBOL("eglTerminate", mock_terminate); SYMBOL("eglReleaseThread", mock_release); SYMBOL("eglGetPlatformDisplayEXT", mock_display);
        SYMBOL("eglCreateImageKHR", mock_image); SYMBOL("eglDestroyImageKHR", mock_destroy_image); SYMBOL("glGetString", mock_string);
        SYMBOL("glGetStringi", mock_string_i); SYMBOL("glGetIntegerv", mock_integer); SYMBOL("glGenTextures", mock_textures);
        SYMBOL("glBindTexture", mock_bind); SYMBOL("glTexParameteri", mock_parameter); SYMBOL("glGetError", mock_error);
        SYMBOL("glFinish", mock_finish); SYMBOL("glDeleteTextures", mock_delete); SYMBOL("glEGLImageTargetTexture2DOES", mock_bind_image);
        if (!strcmp(name, "glCopyImageSubDataEXT") && (mock_gpu == 2 || mock_gpu == 6)) return mock_gpu == 6 ? NULL : (void *)mock_copy;
        if (!strcmp(name, "glCopyImageSubDataOES") && mock_gpu == 3) return (void *)mock_copy;
        if (!strcmp(name, "glCopyImageSubData")) return (void *)mock_copy; /* An address alone does not imply support. */
        assert(!"unexpected GPU symbol"); return NULL;
#undef SYMBOL
    }
#endif
    return __real_dlsym(handle, name);
}
void __wrap_gbm_device_destroy(struct gbm_device *device)
{ assert(mock_gpu && device == (struct gbm_device *)1 && !pthread_equal(pthread_self(), caller)); }

struct gbm_device *__wrap_gbm_create_device(int fd)
{
    assert(fd >= 0 && !pthread_equal(pthread_self(), caller)); worker_inits++; return mock_gpu ? (struct gbm_device *)1 : NULL;
}
int __wrap_vav4l2_capture_expbuf(struct vav4l2_context *ctx, int index)
{ (void)ctx; assert(!index); return dup(source_fd); }
VAStatus __wrap_vav4l2_surface_fill_prime_gbm(const struct vav4l2_surface *s, uint32_t flags, void *out)
{
    (void)flags;
    vav4l2_fill_prime_layers(out, dup(target_fd), 3072, DRM_FORMAT_MOD_LINEAR, VA_FOURCC_NV12,
        s->width, s->height, 64, 32, DRM_FORMAT_NV12, DRM_FORMAT_R8, DRM_FORMAT_GR88, VA_EXPORT_SURFACE_COMPOSED_LAYERS);
    return VA_STATUS_SUCCESS;
}
static int sync_only(int fd, unsigned long request, void *argument)
{ (void)fd; (void)argument; assert(request == DMA_BUF_IOCTL_SYNC); return 0; }
static void worker_case(int mode, int success)
{
    mock_gpu = mode; worker_inits = 0; caller = pthread_self();
    source_fd = memfd_create("gpu-source", MFD_CLOEXEC); target_fd = memfd_create("gpu-target", MFD_CLOEXEC);
    assert(source_fd >= 0 && target_fd >= 0 && !ftruncate(source_fd, 3072) && !ftruncate(target_fd, 3072));
    struct vav4l2_driver_data dd = {0}; dd.gbm.fd = open("/dev/null", O_RDWR | O_CLOEXEC); assert(dd.gbm.fd >= 0);
    struct vav4l2_context ctx = {.driver_data = &dd, .cap_pixelformat = V4L2_PIX_FMT_NV12,
        .cap_width = 64, .cap_height = 32, .cap_stride = 64, .cap_sizeimage = 3072, .capture_bufs_allocd = 1};
    struct vav4l2_surface surface = {.surface_id = 1, .status = VASurfaceReady, .format = VA_FOURCC_NV12,
        .width = 64, .height = 32, .gbm_bo = (void *)1, .prime_capture_valid = 1};
    dd.surfaces[1] = &surface;
    setenv("VAV4L2_GPU_COPY", "1", 1);
    for (unsigned i = 0; i < 2; i++) {
        int result = vav4l2_gpu_copy_capture(&ctx, &surface, 0);
        assert(success ? result == 1 && !surface.gbm_dirty && surface.has_pic && surface.prime_capture_valid :
            result < 0 && surface.gbm_dirty && !surface.has_pic && !surface.prime_capture_valid);
        if (success) {
            VADRMPRIMESurfaceDescriptor target;
            assert(__wrap_vav4l2_surface_fill_prime_gbm(&surface, 0, &target) == VA_STATUS_SUCCESS);
            surface.gbm_bo = NULL; surface.cpu_stride = 64; surface.cpu_size = 3072; vav4l2_set_ioctl_hook(sync_only);
            assert(vav4l2_prime_import(&surface, &target) == VA_STATUS_SUCCESS); close(target.objects[0].fd);
            assert(!vav4l2_prime_read(&surface) && surface.last_writer == VAV4L2_WRITER_CPU);
            assert(vav4l2_validate_reference(&ctx, 1) == VA_STATUS_SUCCESS);
            assert(!vav4l2_prime_write(&surface) && !surface.prime_capture_valid);
            assert(vav4l2_validate_reference(&ctx, 1) == VA_STATUS_ERROR_DECODING_ERROR);
            vav4l2_set_ioctl_hook(NULL); vav4l2_prime_destroy(&surface); free(surface.cpu_ptr);
            surface.cpu_ptr = NULL; surface.cpu_size = surface.cpu_stride = 0; surface.gbm_bo = (void *)1;
        }
    }
#ifdef VAV4L2_HAVE_GPU_COPY
    assert(worker_inits == 1 && dd.gpu_copy);
#else
    assert(!worker_inits && !dd.gpu_copy);
#endif
    vav4l2_gpu_copy_finish(&dd); assert(!dd.gpu_copy);
    unsetenv("VAV4L2_GPU_COPY"); close(dd.gbm.fd); close(source_fd); close(target_fd);
}
int main(void)
{
    VADRMPRIMESurfaceDescriptor d = {.fourcc = VA_FOURCC_NV12, .width = 64, .height = 32, .num_objects = 2, .num_layers = 1};
    d.objects[0].fd = 7; d.objects[1].fd = 8; d.objects[0].size = 4096; d.objects[1].size = 4096;
    d.objects[0].drm_format_modifier = d.objects[1].drm_format_modifier = DRM_FORMAT_MOD_LINEAR;
    d.layers[0].drm_format = DRM_FORMAT_NV12; d.layers[0].num_planes = 2;
    d.layers[0].object_index[1] = 1; d.layers[0].pitch[0] = 64; d.layers[0].pitch[1] = 128; d.layers[0].offset[1] = 16;
    struct vav4l2_gpu_plane p[2];
    assert(!vav4l2_gpu_copy_planes(&d, 64, 32, p) && p[0].columns == 16 && p[1].rows == 16 && p[1].pitch == 128 && p[1].fd == 8 && p[1].offset == 16);
    assert(!vav4l2_gpu_copy_planes(&d, 63, 31, p) && p[0].columns == 16 && p[1].columns == 16 && p[1].rows == 16);
    struct vav4l2_gpu_plane saved[2]; memcpy(saved, p, sizeof(saved));
    VADRMPRIMESurfaceDescriptor original = d;
    for (unsigned fault = 0; fault < 7; fault++) {
        d = original;
        if (fault == 0) d.objects[1].size = 100;
        if (fault == 1) d.layers[0].object_index[1] = 2;
        if (fault == 2) d.objects[0].drm_format_modifier = DRM_FORMAT_MOD_INVALID;
        if (fault == 3) d.layers[0].offset[1] = UINT_MAX;
        if (fault == 4) d.layers[0].pitch[0] = 63;
        if (fault == 5) d.layers[0].drm_format = DRM_FORMAT_NV21;
        if (fault == 6) d.objects[1].fd = -1;
        assert(vav4l2_gpu_copy_planes(&d, 64, 32, p) < 0 && !memcmp(p, saved, sizeof(saved)));
    }
    d = original; d.fourcc = VA_FOURCC_P010; d.layers[0].drm_format = DRM_FORMAT_P010; d.layers[0].pitch[0] = 128;
    assert(!vav4l2_gpu_copy_planes(&d, 64, 32, p) && p[0].columns == 32 && p[1].columns == 32);
    worker_case(0, 0);
#ifdef VAV4L2_HAVE_GPU_COPY
    for (int mode = 1; mode <= 6; mode++) {
        copies = images = destroyed_images = contexts = 0;
        worker_case(mode, mode <= 3);
        assert(contexts == 1 && copies == (mode <= 3 ? 4u : 0u) && images == destroyed_images);
    }
#endif
    puts("GPU copy raw-byte views, independent pitches and bounded descriptor validation: PASS"); return 0;
}
