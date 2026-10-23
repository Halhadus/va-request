/* Public VA export -> retained EGL import -> GLES sampling / fenced GPU write. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver_identity.h"
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/dma-buf.h>
#include <gbm.h>
#include <drm_fourcc.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
static FILE *logfile;
static unsigned pass, fail, skip;
static char failure_detail[256];
static EGLDisplay egl;
static PFNEGLCREATEIMAGEKHRPROC create_image;
static PFNEGLDESTROYIMAGEKHRPROC destroy_image;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target;
static PFNEGLQUERYDMABUFFORMATSEXTPROC query_formats;
static PFNEGLQUERYDMABUFMODIFIERSEXTPROC query_modifiers;
static void result(const char *state, const char *name, const char *why)
{
    if (!strcmp(state, "PASS")) pass++; else if (!strcmp(state, "FAIL")) fail++; else skip++;
    fprintf(logfile, "%s\t%s\t%s\n", state, name, why); fflush(logfile);
    printf("%s %s %s\n", state, name, why);
}
static int extension(const char *list, const char *name)
{
    if (!list) return 0;
    size_t n = strlen(name);
    for (const char *p = list; (p = strstr(p, name)); p += n)
        if ((p == list || p[-1] == ' ') && (!p[n] || p[n] == ' ')) return 1;
    return 0;
}
static int linear_format(unsigned fourcc, int *external_only)
{
    EGLint count = 0;
    if (!query_formats || !query_modifiers || !query_formats(egl, 0, NULL, &count) || count <= 0 || count > 4096) return -1;
    EGLint *formats = calloc(count, sizeof(*formats));
    if (!formats) return -1;
    EGLint found = 0, actual = 0;
    if (!query_formats(egl, count, formats, &actual) || actual < 0 || actual > count) { free(formats); return -1; }
    for (int i = 0; i < actual; i++) found |= (unsigned)formats[i] == fourcc;
    free(formats);
    if (!found) return 0;
    if (!query_modifiers(egl, fourcc, 0, NULL, NULL, &count) || count < 0 || count > 4096) return -1;
    if (!count) return 0;
    EGLuint64KHR *mods = calloc(count, sizeof(*mods)); EGLBoolean *only = calloc(count, sizeof(*only));
    if (!mods || !only) { free(mods); free(only); return -1; }
    found = 0;
    if (!query_modifiers(egl, fourcc, count, mods, only, &actual) || actual < 0 || actual > count) { free(mods); free(only); return -1; }
    for (int i = 0; i < actual; i++) if (mods[i] == DRM_FORMAT_MOD_LINEAR) { found = 1; *external_only = only[i]; }
    free(mods); free(only); return found;
}
static int image(VADisplay va, unsigned fourcc, VAImage *im)
{
    int n = vaMaxNumImageFormats(va); if (n <= 0) return 0;
    VAImageFormat *formats = calloc(n, sizeof(*formats)); if (!formats) return 0;
    int ok = 0;
    if (vaQueryImageFormats(va, formats, &n) == VA_STATUS_SUCCESS)
        for (int i = 0; i < n; i++) if (formats[i].fourcc == fourcc) { ok = vaCreateImage(va, &formats[i], 64, 64, im) == VA_STATUS_SUCCESS; break; }
    free(formats); return ok;
}
static int upload(VADisplay va, VASurfaceID sid, VAImage *im, unsigned fourcc)
{
    unsigned char *p = NULL;
    if (vaMapBuffer(va, im->buf, (void **)&p) != VA_STATUS_SUCCESS) return 0;
    memset(p, 0, im->data_size);
    for (unsigned y = 0; y < 64; y++) for (unsigned x = 0; x < 64; x++) {
        unsigned char *q = p + im->offsets[0] + y * im->pitches[0];
        if (fourcc == VA_FOURCC_ARGB) { q += x * 4; q[0] = 255; q[1] = 64; q[2] = 128; q[3] = 192; }
        else if (fourcc == VA_FOURCC_RGBA || fourcc == VA_FOURCC_RGBX) { q += x * 4; q[0] = 64; q[1] = 128; q[2] = 192; q[3] = 255; }
        else if (fourcc == VA_FOURCC_RGB565) { q += x * 2; q[0] = 0x18; q[1] = 0x44; }
        else if (fourcc == VA_FOURCC_BGRA || fourcc == VA_FOURCC_BGRX) { q += x * 4; q[0] = 192; q[1] = 128; q[2] = 64; q[3] = 255; }
        else if (fourcc == VA_FOURCC_Y210) { uint16_t *v = (uint16_t *)(q + (x / 2) * 8); v[0] = v[2] = 82 << 8; v[1] = 90 << 8; v[3] = 240 << 8; }
        else if (fourcc == VA_FOURCC_P010) ((uint16_t *)q)[x] = 82 << 8;
        else if (fourcc == VA_FOURCC_YUY2 || fourcc == VA_FOURCC_UYVY || fourcc == VA_FOURCC_VYUY || fourcc == VA_FOURCC_YVYU) {
            const unsigned char yuyv[] = {82, 90, 82, 240}, uyvy[] = {90, 82, 240, 82}, vyuy[] = {240, 82, 90, 82}, yvyu[] = {82, 240, 82, 90};
            memcpy(q + (x / 2) * 4, fourcc == VA_FOURCC_YUY2 ? yuyv : fourcc == VA_FOURCC_UYVY ? uyvy : fourcc == VA_FOURCC_VYUY ? vyuy : yvyu, 4);
        } else q[x] = 82;
    }
    if (im->num_planes > 1) for (unsigned plane = 1; plane < im->num_planes; plane++) {
        unsigned rows = fourcc == VA_FOURCC_422H || fourcc == VA_FOURCC_YV16 ? 64 : 32;
        for (unsigned y = 0; y < rows; y++) {
            unsigned char *q = p + im->offsets[plane] + y * im->pitches[plane];
            if (fourcc == VA_FOURCC_P010) { uint16_t *v = (uint16_t *)q; for (unsigned x = 0; x < 64; x++) v[x] = (x & 1 ? 240 : 90) << 8; }
            else if (im->num_planes == 2) for (unsigned x = 0; x < 64; x++) q[x] = (x & 1) ^ (fourcc == VA_FOURCC_NV21) ? 240 : 90;
            else memset(q, (plane == 1) ^ (fourcc == VA_FOURCC_YV12 || fourcc == VA_FOURCC_YV16) ? 90 : 240, 32);
        }
    }
    return vaUnmapBuffer(va, im->buf) == VA_STATUS_SUCCESS && vaPutImage(va, sid, im->image_id, 0, 0, 64, 64, 0, 0, 64, 64) == VA_STATUS_SUCCESS;
}
static EGLImageKHR import_image(const VADRMPRIMESurfaceDescriptor *d, int yuv)
{
    EGLint a[64]; unsigned n = 0;
#define ATTR(k,v) do { a[n++] = (k); a[n++] = (EGLint)(v); } while (0)
    ATTR(EGL_WIDTH, d->width); ATTR(EGL_HEIGHT, d->height); ATTR(EGL_LINUX_DRM_FOURCC_EXT, d->layers[0].drm_format);
    const EGLint fd_attr[] = {EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE1_FD_EXT, EGL_DMA_BUF_PLANE2_FD_EXT};
    const EGLint off_attr[] = {EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT, EGL_DMA_BUF_PLANE2_OFFSET_EXT};
    const EGLint pitch_attr[] = {EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT, EGL_DMA_BUF_PLANE2_PITCH_EXT};
    const EGLint modlo[] = {EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT};
    const EGLint modhi[] = {EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT};
    if (d->num_layers != 1 || !d->layers[0].num_planes || d->layers[0].num_planes > 3) return EGL_NO_IMAGE_KHR;
    for (unsigned p = 0; p < d->layers[0].num_planes; p++) {
        unsigned o = d->layers[0].object_index[p];
        if (o >= d->num_objects) return EGL_NO_IMAGE_KHR;
        ATTR(fd_attr[p], d->objects[o].fd); ATTR(off_attr[p], d->layers[0].offset[p]); ATTR(pitch_attr[p], d->layers[0].pitch[p]);
        ATTR(modlo[p], d->objects[o].drm_format_modifier); ATTR(modhi[p], d->objects[o].drm_format_modifier >> 32);
    }
    ATTR(EGL_IMAGE_PRESERVED_KHR, EGL_TRUE);
    if (yuv) { ATTR(EGL_YUV_COLOR_SPACE_HINT_EXT, EGL_ITU_REC601_EXT); ATTR(EGL_SAMPLE_RANGE_HINT_EXT, EGL_YUV_NARROW_RANGE_EXT); }
    a[n] = EGL_NONE;
#undef ATTR
    EGLImageKHR image = create_image(egl, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, a);
    if (image == EGL_NO_IMAGE_KHR) snprintf(failure_detail, sizeof(failure_detail), "EGL import error=0x%x drm=%08x planes=%u pitch=%u,%u,%u offset=%u,%u,%u",
        eglGetError(), d->layers[0].drm_format, d->layers[0].num_planes, d->layers[0].pitch[0], d->layers[0].pitch[1], d->layers[0].pitch[2],
        d->layers[0].offset[0], d->layers[0].offset[1], d->layers[0].offset[2]);
    return image;
}
static GLuint shader(GLenum type, const char *source)
{
    GLuint s = glCreateShader(type); GLint ok = 0; glShaderSource(s, 1, &source, NULL); glCompileShader(s); glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char text[512]; glGetShaderInfoLog(s, sizeof(text), NULL, text); fprintf(stderr, "renderer shader: %s\n", text); glDeleteShader(s); return 0; }
    return s;
}
static int sample(EGLImageKHR im, GLuint program, int rgb)
{
    GLuint texture, output, fbo; glGenTextures(1, &texture); glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    image_target(GL_TEXTURE_EXTERNAL_OES, im);
    glGenTextures(1, &output); glBindTexture(GL_TEXTURE_2D, output); glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo); glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, output, 0);
    int ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    static const GLfloat quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
    glUseProgram(program); glUniform1i(glGetUniformLocation(program, "tex"), 0);
    GLint pos = glGetAttribLocation(program, "pos"); glEnableVertexAttribArray(pos); glVertexAttribPointer(pos, 2, GL_FLOAT, GL_FALSE, 0, quad);
    glViewport(0, 0, 64, 64); glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    unsigned char pixels[64 * 64 * 4]; glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, pixels); glFinish();
    for (unsigned i = 0; i < sizeof(pixels); i += 4) {
        const int expected[] = {rgb == 2 ? 66 : rgb ? 64 : 255, rgb == 2 ? 130 : rgb ? 128 : 1, rgb == 2 ? 197 : rgb ? 192 : 0, 255};
        for (unsigned c = 0; c < 4; c++) if (abs((int)pixels[i + c] - expected[c]) > 5) { if (ok) snprintf(failure_detail, sizeof(failure_detail), "GPU pixel=%u component=%u wanted=%d got=%u", i / 4, c, expected[c], pixels[i + c]); ok = 0; }
    }
    GLenum error = glGetError();
    if (error != GL_NO_ERROR) { snprintf(failure_detail, sizeof(failure_detail), "GL sampling error=0x%x", error); ok = 0; }
    glDisableVertexAttribArray(pos); glDeleteFramebuffers(1, &fbo); glDeleteTextures(1, &output); glDeleteTextures(1, &texture); return ok;
}
/* Compare exported bytes with the independent upload image before GPU import. */
static int raw_export(VADisplay va, const VAImage *im, const VADRMPRIMESurfaceDescriptor *d)
{
    unsigned char *expected = NULL; int ok = d->num_layers == 1 && d->layers[0].num_planes == im->num_planes;
    if (!ok || vaMapBuffer(va, im->buf, (void **)&expected) != VA_STATUS_SUCCESS) return 0;
    unsigned f = im->format.fourcc;
    for (unsigned o = 0; o < d->num_objects && ok; o++) {
        struct dma_buf_sync sync = {.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ}; int ret;
        unsigned attempts = 0;
        do { ret = ioctl(d->objects[o].fd, DMA_BUF_IOCTL_SYNC, &sync); } while (ret < 0 && (errno == EINTR || errno == EAGAIN) && ++attempts < 128);
        if (ret) { ok = 0; break; }
        unsigned char *map = mmap(NULL, d->objects[o].size, PROT_READ, MAP_SHARED, d->objects[o].fd, 0);
        if (map == MAP_FAILED) ok = 0;
        else {
            for (unsigned p = 0; p < im->num_planes && ok; p++) {
                if (d->layers[0].object_index[p] != o) continue;
                unsigned rows = p && f != VA_FOURCC_422H && f != VA_FOURCC_YV16 ? 32 : 64;
                unsigned row = im->num_planes == 3 && p ? 32 : f == VA_FOURCC_P010 || (im->num_planes == 1 && f != VA_FOURCC_Y210 && f != VA_FOURCC_ARGB && f != VA_FOURCC_BGRA && f != VA_FOURCC_BGRX && f != VA_FOURCC_RGBA && f != VA_FOURCC_RGBX) ? 128 : im->num_planes == 1 ? 256 : 64;
                uint64_t end = d->layers[0].offset[p] + (uint64_t)d->layers[0].pitch[p] * (rows - 1) + row;
                if (end > d->objects[o].size) { ok = 0; break; }
                for (unsigned y = 0; y < rows && ok; y++)
                    ok = !memcmp(map + d->layers[0].offset[p] + (size_t)y * d->layers[0].pitch[p], expected + im->offsets[p] + (size_t)y * im->pitches[p], row);
            }
            munmap(map, d->objects[o].size);
        }
        sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
        attempts = 0;
        do { ret = ioctl(d->objects[o].fd, DMA_BUF_IOCTL_SYNC, &sync); } while (ret < 0 && (errno == EINTR || errno == EAGAIN) && ++attempts < 128);
        if (ret) ok = 0;
    }
    if (vaUnmapBuffer(va, im->buf) != VA_STATUS_SUCCESS) ok = 0;
    if (!ok) snprintf(failure_detail, sizeof(failure_detail), "raw export differs from upload image or CPU synchronization failed");
    return ok;
}

/* Planar422 also has a standard separate R8-layer export contract. This is
 * independent evidence; it never replaces a failing composed import result. */
static int separate_422(VADisplay va, unsigned fourcc)
{
    VASurfaceID sid = VA_INVALID_ID; VAImage im = {.image_id = VA_INVALID_ID}; VADRMPRIMESurfaceDescriptor d = {0};
    VASurfaceAttrib a = {.type = VASurfaceAttribPixelFormat, .flags = VA_SURFACE_ATTRIB_SETTABLE, .value = {.type = VAGenericValueTypeInteger, .value.i = fourcc}};
    int ok = vaCreateSurfaces(va, VA_RT_FORMAT_YUV422, 64, 64, &sid, 1, &a, 1) == VA_STATUS_SUCCESS && image(va, fourcc, &im) && upload(va, sid, &im, fourcc) &&
        vaExportSurfaceHandle(va, sid, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS, &d) == VA_STATUS_SUCCESS && d.num_layers == 3;
    if (sid != VA_INVALID_ID) vaDestroySurfaces(va, &sid, 1);
    if (im.image_id != VA_INVALID_ID) vaDestroyImage(va, im.image_id);
    EGLImageKHR images[3] = {EGL_NO_IMAGE_KHR, EGL_NO_IMAGE_KHR, EGL_NO_IMAGE_KHR}; GLuint textures[3] = {0}, output = 0, fbo = 0;
    for (unsigned p = 0; p < 3 && ok; p++) {
        unsigned plane = p && fourcc == VA_FOURCC_YV16 ? 3 - p : p;
        VADRMPRIMESurfaceDescriptor layer = d; layer.num_layers = 1; layer.layers[0] = d.layers[plane]; layer.width = p ? 32 : 64;
        ok = layer.layers[0].drm_format == DRM_FORMAT_R8 && layer.layers[0].num_planes == 1;
        int only = 0; if (ok) ok = linear_format(DRM_FORMAT_R8, &only) == 1 && !only;
        images[p] = ok ? import_image(&layer, 0) : EGL_NO_IMAGE_KHR; ok = ok && images[p] != EGL_NO_IMAGE_KHR;
        if (ok) {
            glGenTextures(1, &textures[p]); glActiveTexture(GL_TEXTURE0 + p); glBindTexture(GL_TEXTURE_2D, textures[p]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            image_target(GL_TEXTURE_2D, images[p]);
        }
    }
    for (unsigned o = 0; o < d.num_objects; o++) close(d.objects[o].fd);
    GLuint vs = shader(GL_VERTEX_SHADER, "attribute vec2 pos; varying vec2 uv; void main(){ uv=pos*0.5+0.5; gl_Position=vec4(pos,0.0,1.0); }");
    GLuint fs = shader(GL_FRAGMENT_SHADER, "precision mediump float; varying vec2 uv; uniform sampler2D yp,up,vp; void main(){ float y=(texture2D(yp,uv).r*255.0-16.0)/219.0; float u=(texture2D(up,uv).r*255.0-128.0)/224.0; float v=(texture2D(vp,uv).r*255.0-128.0)/224.0; gl_FragColor=vec4(y+1.402*v,y-0.344136*u-0.714136*v,y+1.772*u,1.0); }");
    GLuint program = glCreateProgram(); glAttachShader(program, vs); glAttachShader(program, fs); glLinkProgram(program); GLint linked = 0; glGetProgramiv(program, GL_LINK_STATUS, &linked); ok = ok && vs && fs && linked;
    GLint pos = -1;
    if (ok) {
        glActiveTexture(GL_TEXTURE3); glGenTextures(1, &output); glBindTexture(GL_TEXTURE_2D, output); glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo); glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, output, 0);
        ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        glUseProgram(program); glUniform1i(glGetUniformLocation(program, "yp"), 0); glUniform1i(glGetUniformLocation(program, "up"), 1); glUniform1i(glGetUniformLocation(program, "vp"), 2);
        static const GLfloat quad[] = {-1, -1, 1, -1, -1, 1, 1, 1}; pos = glGetAttribLocation(program, "pos");
        glEnableVertexAttribArray(pos); glVertexAttribPointer(pos, 2, GL_FLOAT, GL_FALSE, 0, quad); glViewport(0, 0, 64, 64); glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        unsigned char pixels[64 * 64 * 4]; glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, pixels); glFinish();
        for (unsigned i = 0; i < sizeof(pixels) && ok; i += 4) ok = pixels[i] >= 250 && pixels[i + 1] <= 6 && pixels[i + 2] <= 5 && pixels[i + 3] == 255;
    }
    if (pos >= 0) glDisableVertexAttribArray(pos);
    GLenum error = glGetError(); if (error != GL_NO_ERROR) ok = 0;
    if (!ok && !failure_detail[0]) snprintf(failure_detail, sizeof(failure_detail), "separate R8 layers import/shader/pixels failed GL=0x%x", error);
    glDeleteProgram(program); glDeleteShader(vs); glDeleteShader(fs);
    if (fbo) glDeleteFramebuffers(1, &fbo);
    if (output) glDeleteTextures(1, &output);
    glDeleteTextures(3, textures); for (unsigned p = 0; p < 3; p++) if (images[p] != EGL_NO_IMAGE_KHR) destroy_image(egl, images[p]); glActiveTexture(GL_TEXTURE0);
    return ok;
}
static int fenced_write(VADisplay va, const char *extensions)
{
    if (!extension(extensions, "EGL_ANDROID_native_fence_sync")) return 2;
    PFNEGLCREATESYNCKHRPROC create_sync = (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
    PFNEGLDESTROYSYNCKHRPROC destroy_sync = (PFNEGLDESTROYSYNCKHRPROC)eglGetProcAddress("eglDestroySyncKHR");
    PFNEGLDUPNATIVEFENCEFDANDROIDPROC dup_fence = (PFNEGLDUPNATIVEFENCEFDANDROIDPROC)eglGetProcAddress("eglDupNativeFenceFDANDROID");
    if (!create_sync || !destroy_sync || !dup_fence) return 0;
    VASurfaceID sid = VA_INVALID_ID; VAImage im = {.image_id = VA_INVALID_ID}; VADRMPRIMESurfaceDescriptor d = {0};
    VASurfaceAttrib a = {.type = VASurfaceAttribPixelFormat, .flags = VA_SURFACE_ATTRIB_SETTABLE, .value = {.type = VAGenericValueTypeInteger, .value.i = VA_FOURCC_BGRA}};
    int ok = vaCreateSurfaces(va, VA_RT_FORMAT_RGB32, 64, 64, &sid, 1, &a, 1) == VA_STATUS_SUCCESS && image(va, VA_FOURCC_BGRA, &im) && upload(va, sid, &im, VA_FOURCC_BGRA) &&
        vaExportSurfaceHandle(va, sid, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, VA_EXPORT_SURFACE_READ_WRITE | VA_EXPORT_SURFACE_COMPOSED_LAYERS, &d) == VA_STATUS_SUCCESS;
    EGLImageKHR egl_image = ok ? import_image(&d, 0) : EGL_NO_IMAGE_KHR;
    GLuint texture = 0, fbo = 0;
    if (egl_image == EGL_NO_IMAGE_KHR) ok = 0;
    if (ok) {
        glGenTextures(1, &texture); glBindTexture(GL_TEXTURE_2D, texture); image_target(GL_TEXTURE_2D, egl_image);
        glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo); glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
        ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    }
    for (unsigned frame = 0; frame < 16 && ok; frame++) {
        glClearColor(frame & 1 ? 1.0f : 0.0f, 0, frame & 1 ? 0.0f : 1.0f, 1); glClear(GL_COLOR_BUFFER_BIT);
        EGLint attrs[] = {EGL_NONE}; EGLSyncKHR sync = create_sync(egl, EGL_SYNC_NATIVE_FENCE_ANDROID, attrs); glFlush();
        int fd = sync == EGL_NO_SYNC_KHR ? -1 : dup_fence(egl, sync);
        if (sync != EGL_NO_SYNC_KHR) destroy_sync(egl, sync);
        if (fd < 0) { ok = 0; break; }
        struct dma_buf_import_sync_file fence = {.flags = DMA_BUF_SYNC_WRITE, .fd = fd};
        int ret; do { ret = ioctl(d.objects[0].fd, DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &fence); } while (ret < 0 && errno == EINTR);
        close(fd);
        ok = !ret && vaGetImage(va, sid, 0, 0, 64, 64, im.image_id) == VA_STATUS_SUCCESS;
        unsigned char *pixels = NULL;
        if (ok && vaMapBuffer(va, im.buf, (void **)&pixels) == VA_STATUS_SUCCESS) {
            for (unsigned y = 0; y < 64; y++) for (unsigned x = 0; x < 64; x++) {
                unsigned char *p = pixels + im.offsets[0] + y * im.pitches[0] + x * 4;
                if (p[0] != (frame & 1 ? 0 : 255) || p[1] || p[2] != (frame & 1 ? 255 : 0) || p[3] != 255) ok = 0;
            }
            if (vaUnmapBuffer(va, im.buf) != VA_STATUS_SUCCESS) ok = 0;
        } else ok = 0;
    }
    glFinish(); if (fbo) glDeleteFramebuffers(1, &fbo); if (texture) glDeleteTextures(1, &texture);
    if (egl_image != EGL_NO_IMAGE_KHR) destroy_image(egl, egl_image);
    for (unsigned i = 0; i < d.num_objects; i++) close(d.objects[i].fd);
    if (im.image_id != VA_INVALID_ID) vaDestroyImage(va, im.image_id);
    if (sid != VA_INVALID_ID) vaDestroySurfaces(va, &sid, 1);
    return ok;
}
int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: %s RENDER_NODE RESULTS_TSV\n", argv[0]); return 2; }
    logfile = fopen(argv[2], "w"); if (!logfile) { perror(argv[2]); return 2; }
    int fd = open(argv[1], O_RDWR | O_CLOEXEC), major, minor;
    if (fd < 0) { perror(argv[1]); fclose(logfile); return 2; }
    VADisplay va = vaGetDisplayDRM(fd);
    if (!va || vaInitialize(va, &major, &minor) != VA_STATUS_SUCCESS) return 2;
    const char *vendor = vaQueryVendorString(va); if (!vareq_test_driver_identity(vendor)) return 2;
    struct gbm_device *gbm = gbm_create_device(fd);
    EGLContext context = EGL_NO_CONTEXT;
    PFNEGLGETPLATFORMDISPLAYEXTPROC platform = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    egl = platform && gbm ? platform(EGL_PLATFORM_GBM_KHR, gbm, NULL) : EGL_NO_DISPLAY;
    if (egl == EGL_NO_DISPLAY || !eglInitialize(egl, NULL, NULL) || !eglBindAPI(EGL_OPENGL_ES_API)) goto unavailable;
    EGLConfig config; EGLint count = 0, ca[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR, EGL_NONE}, ctxa[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    if (!eglChooseConfig(egl, ca, &config, 1, &count) || !count) goto unavailable;
    context = eglCreateContext(egl, config, EGL_NO_CONTEXT, ctxa);
    if (context == EGL_NO_CONTEXT || !eglMakeCurrent(egl, EGL_NO_SURFACE, EGL_NO_SURFACE, context)) goto unavailable;
    const char *extensions = eglQueryString(egl, EGL_EXTENSIONS);
    printf("EGL_VENDOR=%s\nEGL_VERSION=%s\nGL_VENDOR=%s\nGL_VERSION=%s\nGL_RENDERER=%s\n", eglQueryString(egl, EGL_VENDOR), eglQueryString(egl, EGL_VERSION), glGetString(GL_VENDOR), glGetString(GL_VERSION), glGetString(GL_RENDERER));
    create_image = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR"); destroy_image = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    image_target = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    query_formats = (PFNEGLQUERYDMABUFFORMATSEXTPROC)eglGetProcAddress("eglQueryDmaBufFormatsEXT"); query_modifiers = (PFNEGLQUERYDMABUFMODIFIERSEXTPROC)eglGetProcAddress("eglQueryDmaBufModifiersEXT");
    if (!create_image || !destroy_image || !image_target || !extension(extensions, "EGL_EXT_image_dma_buf_import_modifiers") ||
        !extension((const char *)glGetString(GL_EXTENSIONS), "GL_OES_EGL_image_external")) { result("SKIP", "renderer-capabilities", "required EGL/GLES DMA import extensions missing"); goto done; }
    GLuint vs = shader(GL_VERTEX_SHADER, "attribute vec2 pos; varying vec2 uv; void main(){ uv=pos*0.5+0.5; gl_Position=vec4(pos,0.0,1.0); }");
    GLuint fs = shader(GL_FRAGMENT_SHADER, "#extension GL_OES_EGL_image_external : require\nprecision mediump float; varying vec2 uv; uniform samplerExternalOES tex; void main(){ gl_FragColor=texture2D(tex,uv); }");
    GLuint program = glCreateProgram(); glAttachShader(program, vs); glAttachShader(program, fs); glLinkProgram(program); GLint linked; glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!vs || !fs || !linked) { result("FAIL", "renderer-shader", "compile/link failed"); glDeleteProgram(program); glDeleteShader(vs); glDeleteShader(fs); goto done; }
    const struct { unsigned fourcc, rt; const char *name; } formats[] = {
        {VA_FOURCC_NV12, VA_RT_FORMAT_YUV420, "NV12"}, {VA_FOURCC_NV21, VA_RT_FORMAT_YUV420, "NV21"}, {VA_FOURCC_I420, VA_RT_FORMAT_YUV420, "I420"}, {VA_FOURCC_YV12, VA_RT_FORMAT_YUV420, "YV12"},
        {VA_FOURCC_P010, VA_RT_FORMAT_YUV420_10, "P010"}, {VA_FOURCC_YUY2, VA_RT_FORMAT_YUV422, "YUY2"}, {VA_FOURCC_UYVY, VA_RT_FORMAT_YUV422, "UYVY"}, {VA_FOURCC_VYUY, VA_RT_FORMAT_YUV422, "VYUY"}, {VA_FOURCC_YVYU, VA_RT_FORMAT_YUV422, "YVYU"},
        {VA_FOURCC_422H, VA_RT_FORMAT_YUV422, "422H"}, {VA_FOURCC_YV16, VA_RT_FORMAT_YUV422, "YV16"}, {VA_FOURCC_Y210, VA_RT_FORMAT_YUV422_10, "Y210"},
        {VA_FOURCC_ARGB, VA_RT_FORMAT_RGB32, "ARGB"}, {VA_FOURCC_BGRA, VA_RT_FORMAT_RGB32, "BGRA"}, {VA_FOURCC_BGRX, VA_RT_FORMAT_RGB32, "BGRX"},
        {VA_FOURCC_RGB565, VA_RT_FORMAT_RGB16, "RGB565"}, {VA_FOURCC_RGBA, VA_RT_FORMAT_RGB32, "RGBA"}, {VA_FOURCC_RGBX, VA_RT_FORMAT_RGB32, "RGBX"}};
    for (unsigned i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
        failure_detail[0] = 0;
        VASurfaceID sid = VA_INVALID_ID; VAImage im = {.image_id = VA_INVALID_ID}; VADRMPRIMESurfaceDescriptor d = {0}; int only = 0;
        VASurfaceAttrib a = {.type = VASurfaceAttribPixelFormat, .flags = VA_SURFACE_ATTRIB_SETTABLE, .value = {.type = VAGenericValueTypeInteger, .value.i = formats[i].fourcc}};
        int ok = vaCreateSurfaces(va, formats[i].rt, 64, 64, &sid, 1, &a, 1) == VA_STATUS_SUCCESS && image(va, formats[i].fourcc, &im) && upload(va, sid, &im, formats[i].fourcc) &&
            vaExportSurfaceHandle(va, sid, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_COMPOSED_LAYERS, &d) == VA_STATUS_SUCCESS;
        if (ok) ok = raw_export(va, &im, &d);
        int advertised = ok ? linear_format(d.layers[0].drm_format, &only) : -1;
        if (sid != VA_INVALID_ID) { ok = vaDestroySurfaces(va, &sid, 1) == VA_STATUS_SUCCESS && ok; sid = VA_INVALID_ID; }
        int rgb = formats[i].rt == VA_RT_FORMAT_RGB16 ? 2 : formats[i].rt == VA_RT_FORMAT_RGB32;
        EGLImageKHR imported = ok && advertised == 1 ? import_image(&d, !rgb) : EGL_NO_IMAGE_KHR;
        for (unsigned o = 0; o < d.num_objects; o++) close(d.objects[o].fd);
        if (im.image_id != VA_INVALID_ID) vaDestroyImage(va, im.image_id);
        if (ok && !advertised) result("SKIP", formats[i].name, "EGL does not advertise this DRM format with LINEAR modifier");
        else {
            ok = ok && advertised == 1 && imported != EGL_NO_IMAGE_KHR && sample(imported, program, rgb);
            result(ok ? "PASS" : "FAIL", formats[i].name, ok ? "raw bytes / retained export / EGL import / GPU pixels" : failure_detail[0] ? failure_detail : "VA export or advertised EGL import/pixel validation failed");
        }
        if (imported != EGL_NO_IMAGE_KHR) destroy_image(egl, imported);
    }
    glDeleteProgram(program); glDeleteShader(vs); glDeleteShader(fs);
    for (unsigned i = 0; i < 2; i++) {
        failure_detail[0] = 0; int ok = separate_422(va, i ? VA_FOURCC_YV16 : VA_FOURCC_422H);
        result(ok ? "PASS" : "FAIL", i ? "YV16-separate" : "422H-separate", ok ? "separate R8 layers / retained EGL import / GPU pixels" : failure_detail);
    }
    {
        int only = 0, advertised = linear_format(DRM_FORMAT_ARGB8888, &only);
        int ok = advertised < 0 ? 0 : advertised == 1 && !only ? fenced_write(va, extensions) : 2;
        result(ok == 2 ? "SKIP" : ok ? "PASS" : "FAIL", "gpu-fence-writeback", ok == 2 ? "native-fence extension or renderable LINEAR BGRA not advertised" : ok ? "16 GPU writes / native sync-file / DMA reservation / VA pixels" : "fenced GPU writeback failed");
    }
done:
    if (!pass && !fail && !skip) result("FAIL", "renderer-no-pixel-proof", "no format produced validated GPU pixels");
    fprintf(logfile, "COMPLETE\t1\n"); fclose(logfile);
    printf("PASS=%u FAIL=%u SKIP=%u COMPLETE=1 EXIT=%u\n", pass, fail, skip, fail ? 1 : 0);
    eglMakeCurrent(egl, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT); eglDestroyContext(egl, context); eglTerminate(egl); gbm_device_destroy(gbm); vaTerminate(va); close(fd);
    return fail ? 1 : pass ? 0 : 77;
unavailable:
    result("SKIP", "renderer-context", "EGL/GLES context unavailable");
    fprintf(logfile, "COMPLETE\t1\n"); fclose(logfile);
    if (context != EGL_NO_CONTEXT) eglDestroyContext(egl, context);
    if (egl != EGL_NO_DISPLAY) eglTerminate(egl);
    if (gbm) gbm_device_destroy(gbm);
    vaTerminate(va); close(fd); return 77;
}
