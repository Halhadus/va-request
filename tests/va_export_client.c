/*
 * va_export_client.c — end-to-end proof that vaExportSurfaceHandle hands
 * out the real decoded pixels.
 *
 * Decodes an all-IDR 8-bit 4:2:0 stream through the bridge (same canned
 * clip family as va_h264422_client), then for every frame:
 *   1. grabs the reference bytes with vaGetImage (NV12)
 *   2. calls vaExportSurfaceHandle(DRM_PRIME_2, COMPOSED_LAYERS) and
 *      checks the descriptor is the single-object NV12 shape Chromium
 *      requires (num_objects == 1, fourcc NV12)
 *   3. imports the dma-buf through real EGL: plane0 as an R8 image at
 *      offset[0], plane1 as an R8 image at offset[1] (Mesa's per-plane
 *      GR88 import is broken on this box — R8 at offset reads the same
 *      bytes), renders each into an FBO and glReadPixels them back
 *   4. requires byte-exact equality with the vaGetImage reference
 *
 * Exits 0 only if every frame is exact. Prints EXPORT_EXACT <n> per frame.
 *
 * usage: va_export_client <render-node> <stream.h264>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#include <gbm.h>
#define EGL_EGLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include "h264422_annexb.h"

#ifndef EGL_PLATFORM_GBM_KHR
#define EGL_PLATFORM_GBM_KHR 0x31D6
#endif
#ifndef EGL_LINUX_DMA_BUF_EXT
#define EGL_LINUX_DMA_BUF_EXT 0x3270
#endif
#ifndef EGL_DMA_BUF_PLANE0_FD_EXT
#define EGL_DMA_BUF_PLANE0_FD_EXT 0x3272
#define EGL_DMA_BUF_PLANE0_OFFSET_EXT 0x3273
#define EGL_DMA_BUF_PLANE0_PITCH_EXT 0x3274
#endif

#define W 1280
#define H 720

typedef void (GL_APIENTRYP PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)(GLenum, void *);

static void die(const char *msg, VAStatus vas)
{
    fprintf(stderr, "va_export_client: %s (va=%d %s)\n", msg, vas,
            vaErrorStr(vas));
    exit(1);
}

#define CHECK(x) do { VAStatus _s = (x); if (_s != VA_STATUS_SUCCESS) die(#x, _s); } while (0)

static const char *vsrc =
    "attribute vec2 pos;\n"
    "varying vec2 uv;\n"
    "void main() { uv = vec2(pos.x * 0.5 + 0.5, 0.5 - pos.y * 0.5);\n"
    "  gl_Position = vec4(pos, 0.0, 1.0); }\n";
static const char *fsrc =
    "precision mediump float;\n"
    "varying vec2 uv;\n"
    "uniform sampler2D texY;\n"
    "void main() { gl_FragColor = vec4(texture2D(texY, uv).r); }\n";

static GLuint compile(GLenum type, const char *src)
{
    GLuint sh = glCreateShader(type);
    GLint ok = 0;
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(sh, sizeof(log), NULL, log);
        fprintf(stderr, "shader compile: %s\n", log);
        exit(1);
    }
    return sh;
}

static PFNEGLCREATEIMAGEKHRPROC g_create_img;
static PFNEGLDESTROYIMAGEKHRPROC g_destroy_img;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC g_img2tex;
static GLuint g_prog, g_fbo, g_rtex;
/* vertex data must outlive egl_setup(): the attrib pointer references it */
static const GLfloat g_quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };

static void egl_setup(const char *node)
{
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_plat_dpy;
    int fd = open(node, O_RDWR | O_CLOEXEC);
    struct gbm_device *gdev;
    EGLDisplay dpy;
    EGLConfig cfg;
    EGLint n = 0;
    EGLContext ctx;
    EGLint cfgat[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
                       EGL_NONE };
    EGLint cattribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    GLint pos;

    if (fd < 0) { perror("open render node (egl)"); exit(2); }
    gdev = gbm_create_device(fd);
    if (!gdev) { fprintf(stderr, "gbm_create_device failed\n"); exit(2); }

    get_plat_dpy = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress(
        "eglGetPlatformDisplayEXT");
    g_create_img = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress(
        "eglCreateImageKHR");
    g_img2tex = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress(
        "glEGLImageTargetTexture2DOES");
    g_destroy_img = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress(
        "eglDestroyImageKHR");
    if (!get_plat_dpy || !g_create_img || !g_img2tex || !g_destroy_img) {
        fprintf(stderr, "EGL ext procs missing\n");
        exit(2);
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    dpy = get_plat_dpy(EGL_PLATFORM_GBM_KHR, (void *)gdev, NULL);
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, NULL, NULL)) {
        fprintf(stderr, "EGL display init failed\n");
        exit(2);
    }
    if (!eglChooseConfig(dpy, cfgat, &cfg, 1, &n) || n < 1) {
        fprintf(stderr, "eglChooseConfig failed\n");
        exit(2);
    }
    ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, cattribs);
    if (ctx == EGL_NO_CONTEXT ||
        !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
        fprintf(stderr, "EGL context failed (0x%x)\n", eglGetError());
        exit(2);
    }

    g_prog = glCreateProgram();
    glAttachShader(g_prog, compile(GL_VERTEX_SHADER, vsrc));
    glAttachShader(g_prog, compile(GL_FRAGMENT_SHADER, fsrc));
    glLinkProgram(g_prog);
    GLint lk = 0;
    glGetProgramiv(g_prog, GL_LINK_STATUS, &lk);
    if (!lk) { fprintf(stderr, "program link failed\n"); exit(2); }

    glGenFramebuffers(1, &g_fbo);
    glGenTextures(1, &g_rtex);
    glBindTexture(GL_TEXTURE_2D, g_rtex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, NULL);
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, g_rtex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "FBO incomplete\n");
        exit(2);
    }
    glUseProgram(g_prog);
    glUniform1i(glGetUniformLocation(g_prog, "texY"), 0);
    pos = glGetAttribLocation(g_prog, "pos");
    glEnableVertexAttribArray(pos);
    glVertexAttribPointer(pos, 2, GL_FLOAT, GL_FALSE, 0, (void *)g_quad);
}

/*
 * Import one R8 view of the dma-buf (fd at byte offset, pitch, w x h),
 * render 1:1 into the FBO and read back the single-channel bytes.
 * out must hold w*h bytes. Returns 0 on success.
 */
static int read_r8_view(int fd, uint32_t offset, uint32_t pitch,
                        int w, int h, uint8_t *out)
{
    EGLint at[] = {
        EGL_WIDTH, w, EGL_HEIGHT, h,
        EGL_LINUX_DRM_FOURCC_EXT, GBM_FORMAT_R8,
        EGL_DMA_BUF_PLANE0_FD_EXT, fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)offset,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)pitch,
        EGL_NONE,
    };
    EGLImage img = g_create_img(eglGetCurrentDisplay(), EGL_NO_CONTEXT,
                                EGL_LINUX_DMA_BUF_EXT, (EGLClientBuffer)NULL,
                                at);
    GLuint tex;
    uint32_t *rgba;

    if (img == EGL_NO_IMAGE_KHR)
        return -1;
    glGenTextures(1, &tex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    g_img2tex(GL_TEXTURE_2D, img);
    glViewport(0, 0, w, h);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    rgba = malloc((size_t)w * h * 4);
    if (!rgba) { glDeleteTextures(1, &tex); return -1; }
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            /* glReadPixels origin is bottom-left; texture row 0 is top */
            out[(size_t)y * w + x] =
                (uint8_t)(rgba[(size_t)(h - 1 - y) * w + x] & 0xff);
    free(rgba);
    glDeleteTextures(1, &tex);
    g_destroy_img(eglGetCurrentDisplay(), img);
    return glGetError() == GL_NO_ERROR ? 0 : -1;
}

/* decode one all-IDR slice into surface s (same semantics as the loop
 * body below; shared by pass 1 and the lazy-memfd pass 2) */
static void decode_one(VADisplay dpy, VAContextID ctx, VASurfaceID s,
                       VAPictureParameterBufferH264 *pp,
                       VAIQMatrixBufferH264 *iq,
                       unsigned char *slice, int slice_size)
{
    VASliceParameterBufferH264 sp;
    VABufferID b_pp, b_iq, b_sp, b_sd;

    memset(&sp, 0, sizeof(sp));
    sp.slice_data_size = slice_size;
    sp.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
    sp.first_mb_in_slice = 0;
    sp.slice_type = 7;
    pp->CurrPic.picture_id = s;

    CHECK(vaCreateBuffer(dpy, ctx, VAPictureParameterBufferType,
                         sizeof(*pp), 1, pp, &b_pp));
    CHECK(vaCreateBuffer(dpy, ctx, VAIQMatrixBufferType,
                         sizeof(*iq), 1, iq, &b_iq));
    CHECK(vaCreateBuffer(dpy, ctx, VASliceParameterBufferType,
                         sizeof(sp), 1, &sp, &b_sp));
    CHECK(vaCreateBuffer(dpy, ctx, VASliceDataBufferType,
                         slice_size, 1, slice, &b_sd));
    CHECK(vaBeginPicture(dpy, ctx, s));
    VABufferID r1[2] = { b_pp, b_iq };
    CHECK(vaRenderPicture(dpy, ctx, r1, 2));
    VABufferID r2[2] = { b_sp, b_sd };
    CHECK(vaRenderPicture(dpy, ctx, r2, 2));
    CHECK(vaEndPicture(dpy, ctx));
    CHECK(vaSyncSurface(dpy, s));
    vaDestroyBuffer(dpy, b_pp);
    vaDestroyBuffer(dpy, b_iq);
    vaDestroyBuffer(dpy, b_sp);
    vaDestroyBuffer(dpy, b_sd);
}

/* Same canned IDR, represented as offset ranges or fragments. The baseline
 * decode above is unchanged; compare every output byte with it below. */
static void decode_packetized(VADisplay dpy, VAContextID ctx, VASurfaceID s,
                              VAPictureParameterBufferH264 *pp, VAIQMatrixBufferH264 *iq,
                              const unsigned char *slice, int slice_size, int mode)
{
    VABufferID picture[2], ids[6]; int count = 0;
    VASliceParameterBufferH264 sp[3] = {{0}};
    unsigned prefix = mode == 2 ? 3 : mode == 3 ? 4 : 0;
    unsigned size = slice_size + prefix, parts = mode == 1 ? 1 : 3;
    if (slice_size < 3) { fprintf(stderr, "packetization test requires a complete IDR\n"); exit(1); }
    unsigned char *payload = malloc(size + 24);
    if (!payload) { fprintf(stderr, "packetization allocation failed\n"); exit(1); }
    memset(payload, 0xab, size + 24);
    if (prefix) { memset(payload + 11, 0, prefix); payload[10 + prefix] = 1; }
    memcpy(payload + 11 + prefix, slice, slice_size);
    unsigned offsets[] = {0, 1, 2};
    unsigned sizes[] = {parts == 1 ? size : 1, 1, size - 2};
    for (unsigned i = 0; i < parts; i++) {
        sp[i].slice_data_size = sizes[i]; sp[i].slice_data_offset = 11 + offsets[i]; sp[i].slice_type = 7;
        sp[i].slice_data_flag = parts == 1 ? VA_SLICE_DATA_FLAG_ALL : i == 0 ? VA_SLICE_DATA_FLAG_BEGIN :
            i == parts - 1 ? VA_SLICE_DATA_FLAG_END : VA_SLICE_DATA_FLAG_MIDDLE;
    }
    pp->CurrPic.picture_id = s;
    CHECK(vaCreateBuffer(dpy, ctx, VAPictureParameterBufferType, sizeof(*pp), 1, pp, &picture[0]));
    CHECK(vaCreateBuffer(dpy, ctx, VAIQMatrixBufferType, sizeof(*iq), 1, iq, &picture[1]));
    CHECK(vaBeginPicture(dpy, ctx, s));
    CHECK(vaRenderPicture(dpy, ctx, picture, 2));
    if (mode == 3) {
        CHECK(vaCreateBuffer(dpy, ctx, VASliceParameterBufferType, sizeof(sp[0]), parts, sp, &ids[0]));
        CHECK(vaCreateBuffer(dpy, ctx, VASliceDataBufferType, size + 24, 1, payload, &ids[1]));
        count = 2;
        CHECK(vaRenderPicture(dpy, ctx, ids, count));
    } else {
        for (unsigned i = 0; i < parts; i++) {
            CHECK(vaCreateBuffer(dpy, ctx, VASliceParameterBufferType, sizeof(sp[i]), 1, &sp[i], &ids[count]));
            CHECK(vaCreateBuffer(dpy, ctx, VASliceDataBufferType, size + 24, 1, payload, &ids[count + 1]));
            CHECK(vaRenderPicture(dpy, ctx, ids + count, 2));
            count += 2;
        }
    }
    CHECK(vaEndPicture(dpy, ctx));
    CHECK(vaSyncSurface(dpy, s));
    for (int i = 0; i < count; i++) CHECK(vaDestroyBuffer(dpy, ids[i]));
    for (int i = 0; i < 2; i++) CHECK(vaDestroyBuffer(dpy, picture[i]));
    free(payload);
}

/* vaGetImage into ref: Y rows at ref, UV rows at ref + W*H (tight) */
static void grab_ref(VADisplay dpy, VASurfaceID s, VAImageFormat *fmt,
                     uint8_t *ref)
{
    VAImage img;
    void *p = NULL;
    uint32_t rpitch, uvp;
    const uint8_t *uvsrc;

    CHECK(vaCreateImage(dpy, fmt, W, H, &img));
    CHECK(vaGetImage(dpy, s, 0, 0, W, H, img.image_id));
    CHECK(vaMapBuffer(dpy, img.buf, &p));
    rpitch = img.pitches[0];
    for (int y = 0; y < H; y++)
        memcpy(ref + (size_t)y * W, (uint8_t *)p + (size_t)y * rpitch, W);
    uvp = img.pitches[1] ? img.pitches[1] : img.pitches[0];
    uvsrc = (uint8_t *)p + img.offsets[1];
    for (int y = 0; y < H / 2; y++)
        memcpy(ref + (size_t)W * H + (size_t)y * W,
               uvsrc + (size_t)y * uvp, W);
    CHECK(vaUnmapBuffer(dpy, img.buf));
    CHECK(vaDestroyImage(dpy, img.image_id));
}

static int mism(const char *what, int frame, int x, int y,
                uint8_t exp, uint8_t got)
{
    fprintf(stderr, "MISMATCH %s frame=%d at (%d,%d): expected %u got %u\n",
            what, frame, x, y, exp, got);
    return 1;
}


int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <render-node> <stream.h264>\n", argv[0]);
        return 2;
    }

    /* ---- load the elementary stream and collect slice NALs ---- */
    FILE *f = fopen(argv[2], "rb");
    if (!f) { perror("open stream"); return 2; }
    if (fseek(f, 0, SEEK_END)) { perror("seek stream"); fclose(f); return 2; }
    long len = ftell(f);
    if (len <= 0 || len > INT_MAX || fseek(f, 0, SEEK_SET)) { fprintf(stderr, "invalid stream size\n"); fclose(f); return 2; }
    unsigned char *es = malloc((size_t)len);
    if (!es) { perror("allocate stream"); fclose(f); return 2; }
    if (fread(es, 1, (size_t)len, f) != (size_t)len) { fprintf(stderr, "short stream read\n"); fclose(f); free(es); return 2; }
    fclose(f);
    unsigned char *slices[8]; int slice_sizes[8];
    int n_slices = h264422_slices(es, (size_t)len, slices, slice_sizes, 8);
    if (n_slices != 8) { fprintf(stderr, "export check requires eight complete IDR slices\n"); free(es); return 2; }

    /* ---- connect to the driver ---- */
    int drm_fd = open(argv[1], O_RDWR);
    if (drm_fd < 0) { perror("open render node"); return 2; }
    VADisplay dpy = vaGetDisplayDRM(drm_fd);
    if (!dpy) { fprintf(stderr, "vaGetDisplayDRM failed\n"); return 2; }
    int major, minor;
    CHECK(vaInitialize(dpy, &major, &minor));

    VAConfigAttrib rt = { .type = VAConfigAttribRTFormat,
                          .value = VA_RT_FORMAT_YUV420 };
    VAConfigID cfg;
    CHECK(vaCreateConfig(dpy, VAProfileH264High, VAEntrypointVLD, &rt, 1, &cfg));

    VASurfaceAttrib sattr = {
        .type = VASurfaceAttribPixelFormat,
        .flags = VA_SURFACE_ATTRIB_SETTABLE,
        .value = { VAGenericValueTypeInteger, .value = { .i = VA_FOURCC_NV12 } },
    };
    VASurfaceID surf[9];
    CHECK(vaCreateSurfaces(dpy, rt.value, W, H, surf, 9, &sattr, 1));
    /* Export before any decode context exists: this must be a GBM object.
     * Keep it across changing frames and surface/context destruction. */
    VADRMPRIMESurfaceDescriptor retained;
    CHECK(vaExportSurfaceHandle(dpy, surf[8], VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
        VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_COMPOSED_LAYERS, &retained));
    if (retained.fourcc != VA_FOURCC_NV12 || retained.num_objects != 1 || retained.num_layers != 1 ||
        retained.layers[0].num_planes != 2 || retained.layers[0].object_index[0] || retained.layers[0].object_index[1] ||
        fcntl(retained.objects[0].fd, F_GETFD) < 0 || !(fcntl(retained.objects[0].fd, F_GETFD) & FD_CLOEXEC))
        die("retained export descriptor", VA_STATUS_ERROR_OPERATION_FAILED);

    VAContextID ctx;
    CHECK(vaCreateContext(dpy, cfg, W, H, VA_PROGRESSIVE, surf, 9, &ctx));

    /* ---- picture parameters for the canned x264 clips ---- */
    VAPictureParameterBufferH264 pp;
    memset(&pp, 0, sizeof(pp));
    pp.CurrPic.picture_id = VA_INVALID_SURFACE;
    pp.CurrPic.frame_idx = 0;
    pp.CurrPic.flags = VA_PICTURE_H264_SHORT_TERM_REFERENCE;
    for (int i = 0; i < 16; i++) {
        pp.ReferenceFrames[i].picture_id = VA_INVALID_SURFACE;
        pp.ReferenceFrames[i].flags = VA_PICTURE_H264_INVALID;
    }
    pp.picture_width_in_mbs_minus1 = W / 16 - 1;
    pp.picture_height_in_mbs_minus1 = H / 16 - 1;
    pp.num_ref_frames = 0;
    pp.seq_fields.bits.chroma_format_idc = 1;
    pp.seq_fields.bits.frame_mbs_only_flag = 1;
    pp.seq_fields.bits.direct_8x8_inference_flag = 1;
    pp.seq_fields.bits.log2_max_frame_num_minus4 = 0;
    pp.seq_fields.bits.pic_order_cnt_type = 2;
    pp.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 = 0;
    pp.pic_init_qp_minus26 = -3;
    pp.pic_init_qs_minus26 = 0;
    pp.chroma_qp_index_offset = 0;
    pp.second_chroma_qp_index_offset = 0;
    pp.pic_fields.bits.deblocking_filter_control_present_flag = 1;
    pp.pic_fields.bits.reference_pic_flag = 1;
    pp.frame_num = 0;

    VAIQMatrixBufferH264 iq;
    memset(&iq, 0, sizeof(iq));
    for (int i = 0; i < 6; i++)
        for (int j = 0; j < 16; j++)
            iq.ScalingList4x4[i][j] = 16;
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 64; j++)
            iq.ScalingList8x8[i][j] = 64;

    VAImageFormat fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.fourcc = VA_FOURCC_NV12;

    egl_setup(argv[1]);
    uint8_t *ybuf = malloc((size_t)W * H);
    uint8_t *uvbuf = malloc((size_t)W * H / 2);
    uint8_t *ref = malloc((size_t)W * H * 2);
    uint8_t *store_y[8], *store_uv[8];
    for (int i = 0; i < 8; i++) {
        store_y[i] = malloc((size_t)W * H);
        store_uv[i] = malloc((size_t)W * H / 2);
    }
    for (unsigned i = 0; i < 8; i++) if (!store_y[i] || !store_uv[i]) { fprintf(stderr, "oom\n"); return 2; }
    if (!ybuf || !uvbuf || !ref) {
        fprintf(stderr, "oom\n"); return 2;
    }

    int bad = 0;
    for (int n = 0; n < n_slices && n < 8; n++) {
        decode_one(dpy, ctx, surf[n], &pp, &iq, slices[n], slice_sizes[n]);

        /* reference bytes via vaGetImage (surfaces have no bo yet on this
         * pass: classic memfd path) */
        grab_ref(dpy, surf[n], &fmt, ref);
        memcpy(store_y[n], ref, (size_t)W * H);
        memcpy(store_uv[n], ref + (size_t)W * H, (size_t)W * H / 2);

        /* ---- export and verify the descriptor shape ---- */
        VADRMPRIMESurfaceDescriptor desc;
        VAStatus st = vaExportSurfaceHandle(dpy, surf[n],
                                            VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                            VA_EXPORT_SURFACE_COMPOSED_LAYERS,
                                            &desc);
        if (st != VA_STATUS_SUCCESS)
            die("vaExportSurfaceHandle", st);
        if (desc.num_objects != 1) {
            fprintf(stderr, "frame %d: num_objects=%u (need 1)\n",
                    n, desc.num_objects);
            bad = 1;
            break;
        }
        if (desc.fourcc != VA_FOURCC_NV12 || desc.num_layers != 1 ||
            desc.width != W || desc.height != H) {
            fprintf(stderr, "frame %d: bad descriptor shape "
                    "(fourcc=%u layers=%u %ux%u)\n", n, desc.fourcc,
                    desc.num_layers, desc.width, desc.height);
            bad = 1;
            break;
        }

        /* ---- GPU readback through the real dma-buf path ---- */
        int fd = desc.objects[0].fd;
        uint32_t off0 = desc.layers[0].offset[0];
        uint32_t pitch0 = desc.layers[0].pitch[0];
        uint32_t off1 = desc.layers[0].offset[1];
        uint32_t pitch1 = desc.layers[0].pitch[1];

        if (read_r8_view(fd, off0, pitch0, W, H, ybuf) < 0) {
            fprintf(stderr, "frame %d: Y view import failed\n", n);
            bad = 1;
            close(fd);
            break;
        }
        if (read_r8_view(fd, off1, pitch1, W, H / 2, uvbuf) < 0) {
            fprintf(stderr, "frame %d: UV view import failed\n", n);
            bad = 1;
            close(fd);
            break;
        }
        close(fd);

        for (int y = 0; y < H && !bad; y++)
            for (int x = 0; x < W; x++)
                if (ybuf[(size_t)y * W + x] != ref[(size_t)y * W + x]) {
                    bad = mism("Y", n, x, y, ref[(size_t)y * W + x],
                               ybuf[(size_t)y * W + x]);
                    break;
                }
        for (int y = 0; y < H / 2 && !bad; y++)
            for (int x = 0; x < W; x++)
                if (uvbuf[(size_t)y * W + x] !=
                    ref[(size_t)W * H + (size_t)y * W + x]) {
                    bad = mism("UV", n, x, y,
                               ref[(size_t)W * H + (size_t)y * W + x],
                               uvbuf[(size_t)y * W + x]);
                    break;
                }
        if (bad)
            break;

        printf("EXPORT_EXACT %d\n", n);
    }

    /*
     * Pass 2 — the lazy-memfd guard. Every surface now carries a GBM bo
     * (created by the pass-1 exports), so pull_capture takes the lazy
     * path: bo-only snapshot, memfd marked stale. vaGetImage must refill
     * the memfd from the bo transparently. Re-decode the same all-IDR
     * slices and require byte-identical readback vs pass 1.
     */
    for (int n = 0; n < n_slices && n < 8 && !bad; n++) {
        decode_one(dpy, ctx, surf[n], &pp, &iq, slices[n], slice_sizes[n]);
        grab_ref(dpy, surf[n], &fmt, ref);

        for (int y = 0; y < H && !bad; y++)
            for (int x = 0; x < W; x++)
                if (ref[(size_t)y * W + x] != store_y[n][(size_t)y * W + x]) {
                    bad = mism("LAZY-Y", n, x, y,
                               store_y[n][(size_t)y * W + x],
                               ref[(size_t)y * W + x]);
                    break;
                }
        for (int y = 0; y < H / 2 && !bad; y++)
            for (int x = 0; x < W; x++)
                if (ref[(size_t)W * H + (size_t)y * W + x] !=
                    store_uv[n][(size_t)y * W + x]) {
                    bad = mism("LAZY-UV", n, x, y,
                               store_uv[n][(size_t)y * W + x],
                               ref[(size_t)W * H + (size_t)y * W + x]);
                    break;
                }
        if (bad)
            break;
        printf("LAZY_EXACT %d\n", n);
    }

    for (int mode = 1; mode <= 4 && !bad; mode++) {
        for (int n = 0; n < n_slices && n < 8 && !bad; n++) {
            decode_packetized(dpy, ctx, surf[n], &pp, &iq, slices[n], slice_sizes[n], mode);
            grab_ref(dpy, surf[n], &fmt, ref);
            if (memcmp(ref, store_y[n], (size_t)W * H) ||
                memcmp(ref + (size_t)W * H, store_uv[n], (size_t)W * H / 2)) {
                fprintf(stderr, "packetization mode %d frame %d: readback differs from whole-slice decode\n", mode, n);
                bad = 1;
            } else printf("PACKET_EXACT %d %d\n", mode, n);
        }
    }

    for (int n = 0; n < n_slices && n < 8 && !bad; n++) {
        decode_one(dpy, ctx, surf[8], &pp, &iq, slices[n], slice_sizes[n]);
        if (read_r8_view(retained.objects[0].fd, retained.layers[0].offset[0], retained.layers[0].pitch[0], W, H, ybuf) < 0 ||
            read_r8_view(retained.objects[0].fd, retained.layers[0].offset[1], retained.layers[0].pitch[1], W, H / 2, uvbuf) < 0 ||
            memcmp(ybuf, store_y[n], (size_t)W * H) || memcmp(uvbuf, store_uv[n], (size_t)W * H / 2)) {
            fprintf(stderr, "retained GBM handle frame %d differs from decoded reference\n", n); bad = 1;
        } else printf("RETAINED_EXACT %d\n", n);
    }
    CHECK(vaDestroyContext(dpy, ctx));
    CHECK(vaDestroySurfaces(dpy, surf, 9));
    CHECK(vaDestroyConfig(dpy, cfg));
    CHECK(vaTerminate(dpy));
    if (!bad && n_slices == 8) {
        if (read_r8_view(retained.objects[0].fd, retained.layers[0].offset[0], retained.layers[0].pitch[0], W, H, ybuf) < 0 ||
            read_r8_view(retained.objects[0].fd, retained.layers[0].offset[1], retained.layers[0].pitch[1], W, H / 2, uvbuf) < 0 ||
            memcmp(ybuf, store_y[7], (size_t)W * H) || memcmp(uvbuf, store_uv[7], (size_t)W * H / 2)) bad = 1;
        else printf("RETAINED_LIFETIME_EXACT\n");
    }
    close(retained.objects[0].fd);
    for (int i = 0; i < 8; i++) { free(store_y[i]); free(store_uv[i]); }
    free(ybuf); free(uvbuf); free(ref); free(es);
    close(drm_fd);
    return bad ? 1 : 0;
}
