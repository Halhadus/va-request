/*
 * va_h264422_client.c — standalone VA-API client that drives the bridge's
 * full H.264 High 4:2:2 decode path end to end.
 *
 * Stock ffmpeg cannot reach it: the h264 decoder only offers
 * AV_PIX_FMT_VAAPI for 4:2:0 streams (h264_slice.c get_pixel_format) and
 * the vaapi profile map has no H264_HIGH_422 entry. This client mirrors
 * what an ffmpeg-style decoder would submit, including the ffmpeg quirk
 * of baking the bit-depth QP offset into pic_init_qp/qs_minus26 (the
 * bridge strips it — see h264_fill_pps).
 *
 * It reads an Annex-B elementary stream of all-IDR High 4:2:2 pictures
 * (canned x264 clips from tests/run_full_matrix.sh, -g 1 so every frame
 * is an IDR and the DPB stays empty), decodes each picture through
 * VAAPI, and writes the vaGetImage bytes (Y210 for 10-bit, YUY2 for
 * 8-bit) to stdout. The matrix compares that dump against the software
 * decode.
 *
 * Optional 8-bit vpp mode submits the decoded surface directly to VPP
 * before any image readback and compares the resulting YUY2 bytes.
 * Optional 422H/YV16 mode reads decoded NV16 as planar 4:2:2 and writes
 * canonical Y/U/V bytes, independent of the image's physical U/V order.
 * usage: va_h264422_client <render-node> <stream.h264> <8|10> [vpp|422H|YV16|vpp-422H|vpp-YV16]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_vpp.h>
#include "h264422_annexb.h"

#define W 1280
#define H 720

static void die(const char *msg, VAStatus vas)
{
    fprintf(stderr, "va422client: %s (va=%d %s)\n", msg, vas, vaErrorStr(vas));
    exit(1);
}

#define CHECK(x) do { VAStatus _s = (x); if (_s != VA_STATUS_SUCCESS) die(#x, _s); } while (0)

int main(int argc, char **argv)
{
    if (argc != 4 && argc != 5) {
        fprintf(stderr, "usage: %s <render-node> <stream.h264> <8|10> [vpp|422H|YV16|vpp-422H|vpp-YV16]\n", argv[0]);
        return 2;
    }
    int depth = atoi(argv[3]);
    int use_vpp = argc == 5 && (!strcmp(argv[4], "vpp") || !strcmp(argv[4], "vpp-422H") || !strcmp(argv[4], "vpp-YV16"));
    unsigned planar = argc == 5 && (!strcmp(argv[4], "422H") || !strcmp(argv[4], "vpp-422H")) ? VA_FOURCC_422H :
        argc == 5 && (!strcmp(argv[4], "YV16") || !strcmp(argv[4], "vpp-YV16")) ? VA_FOURCC_YV16 : 0;
    if (argc == 5 && ((!use_vpp && !planar) || depth != 8)) {
        fprintf(stderr, "optional mode requires 8-bit input and a listed layout/mode\n"); return 2;
    }
    if (depth != 8 && depth != 10) {
        fprintf(stderr, "depth must be 8 or 10\n");
        return 2;
    }

    /* ---- load the elementary stream and collect slice NALs ---- */
    FILE *f = fopen(argv[2], "rb");
    if (!f) { perror("open stream"); return 2; }
    if (fseek(f, 0, SEEK_END)) { perror("seek stream"); fclose(f); return 2; }
    long len = ftell(f);
    if (len <= 0 || len > INT_MAX || fseek(f, 0, SEEK_SET)) {
        fprintf(stderr, "invalid stream size\n"); fclose(f); return 2;
    }
    unsigned char *es = malloc(len);
    if (!es) { perror("allocate stream"); fclose(f); return 2; }
    if (fread(es, 1, len, f) != (size_t)len) { fprintf(stderr, "short stream read\n"); fclose(f); free(es); return 2; }
    fclose(f);

    unsigned char *slices[16];
    int slice_sizes[16];
    int n_slices = h264422_slices(es, (size_t)len, slices, slice_sizes, 16);
    if (n_slices <= 0) { fprintf(stderr, "no valid slice NALs\n"); free(es); return 2; }

    /* ---- connect to the driver ---- */
    int drm_fd = open(argv[1], O_RDWR);
    if (drm_fd < 0) { perror("open render node"); return 2; }
    VADisplay dpy = vaGetDisplayDRM(drm_fd);
    if (!dpy) { fprintf(stderr, "vaGetDisplayDRM failed\n"); return 2; }
    int major, minor;
    CHECK(vaInitialize(dpy, &major, &minor));

    /* ---- config: exercise the advertised High422 rt formats ---- */
    VAConfigAttrib rt = {
        .type = VAConfigAttribRTFormat,
        .value = depth == 10 ? VA_RT_FORMAT_YUV422_10 : VA_RT_FORMAT_YUV422,
    };
    VAConfigID cfg;
    CHECK(vaCreateConfig(dpy, VAProfileH264High422, VAEntrypointVLD,
                         &rt, 1, &cfg));

    VASurfaceAttrib sattr = {
        .type = VASurfaceAttribPixelFormat,
        .flags = VA_SURFACE_ATTRIB_SETTABLE,
        .value = { VAGenericValueTypeInteger,
                   .value = { .i = depth == 10 ? VA_FOURCC_Y210 : VA_FOURCC_YUY2 } },
    };
    VASurfaceID surf[8];
    CHECK(vaCreateSurfaces(dpy, rt.value, W, H, surf, 8, &sattr, 1));

    VAContextID ctx;
    CHECK(vaCreateContext(dpy, cfg, W, H, VA_PROGRESSIVE, surf, 8, &ctx));
    VAConfigID vpp_cfg = VA_INVALID_ID; VAContextID vpp_ctx = VA_INVALID_ID;
    VASurfaceID vpp_surface = VA_INVALID_ID;
    if (use_vpp) {
        CHECK(vaCreateConfig(dpy, VAProfileNone, VAEntrypointVideoProc, &rt, 1, &vpp_cfg));
        VASurfaceAttrib vpp_attr = sattr;
        if (planar) vpp_attr.value.value.i = planar;
        CHECK(vaCreateSurfaces(dpy, rt.value, W, H, &vpp_surface, 1, &vpp_attr, 1));
        CHECK(vaCreateContext(dpy, vpp_cfg, W, H, VA_PROGRESSIVE, &vpp_surface, 1, &vpp_ctx));
    }

    /* ---- picture parameters: SPS/PPS of the canned clips
     * (chroma_format_idc=2, poc_type=2, log2_max_frame_num_minus4=0,
     *  80x45 mbs, CAVLC, deblocking control present).
     * pic_init_qp/qs_minus26 carry the ffmpeg-style bit-depth offset
     * (QpBdOffsetY = 12 for 10-bit): +9/+12 vs the bitstream's -3/0. */
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
    pp.bit_depth_luma_minus8 = depth == 10 ? 2 : 0;
    pp.bit_depth_chroma_minus8 = depth == 10 ? 2 : 0;
    pp.num_ref_frames = 0;
    pp.seq_fields.bits.chroma_format_idc = 2;
    pp.seq_fields.bits.frame_mbs_only_flag = 1;
    pp.seq_fields.bits.direct_8x8_inference_flag = 1;
    pp.seq_fields.bits.log2_max_frame_num_minus4 = 0;
    pp.seq_fields.bits.pic_order_cnt_type = 2;
    pp.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 = 0;
    pp.pic_init_qp_minus26 = depth == 10 ? 9 : -3;
    pp.pic_init_qs_minus26 = depth == 10 ? 12 : 0;
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
    fmt.fourcc = planar ? planar : depth == 10 ? VA_FOURCC_Y210 : VA_FOURCC_YUY2;

    for (int n = 0; n < n_slices; n++) {
        if (n >= 8) break;
        pp.CurrPic.picture_id = surf[n];

        VASliceParameterBufferH264 sp;
        memset(&sp, 0, sizeof(sp));
        sp.slice_data_size = slice_sizes[n];
        sp.slice_data_offset = 0;
        sp.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
        sp.slice_data_bit_offset = 0; /* the kernel parses the header */
        sp.first_mb_in_slice = 0;
        sp.slice_type = 7; /* I (all slices) */
        sp.num_ref_idx_l0_active_minus1 = 0;
        sp.num_ref_idx_l1_active_minus1 = 0;
        sp.cabac_init_idc = 0;
        sp.slice_qp_delta = 0;
        sp.disable_deblocking_filter_idc = 0;
        sp.slice_alpha_c0_offset_div2 = 0;
        sp.slice_beta_offset_div2 = 0;

        VABufferID b_pp, b_iq, b_sp, b_sd;
        CHECK(vaCreateBuffer(dpy, ctx, VAPictureParameterBufferType,
                             sizeof(pp), 1, &pp, &b_pp));
        CHECK(vaCreateBuffer(dpy, ctx, VAIQMatrixBufferType,
                             sizeof(iq), 1, &iq, &b_iq));
        CHECK(vaCreateBuffer(dpy, ctx, VASliceParameterBufferType,
                             sizeof(sp), 1, &sp, &b_sp));
        CHECK(vaCreateBuffer(dpy, ctx, VASliceDataBufferType,
                             slice_sizes[n], 1, slices[n], &b_sd));

        CHECK(vaBeginPicture(dpy, ctx, surf[n]));
        VABufferID render1[2] = { b_pp, b_iq };
        CHECK(vaRenderPicture(dpy, ctx, render1, 2));
        VABufferID render2[2] = { b_sp, b_sd };
        CHECK(vaRenderPicture(dpy, ctx, render2, 2));
        CHECK(vaEndPicture(dpy, ctx));

        CHECK(vaSyncSurface(dpy, surf[n]));
        VASurfaceID readback = surf[n];
        if (use_vpp) {
            VAProcPipelineParameterBuffer pipe = {.surface = surf[n]};
            VABufferID params;
            CHECK(vaCreateBuffer(dpy, vpp_ctx, VAProcPipelineParameterBufferType, sizeof(pipe), 1, &pipe, &params));
            CHECK(vaBeginPicture(dpy, vpp_ctx, vpp_surface));
            CHECK(vaRenderPicture(dpy, vpp_ctx, &params, 1)); CHECK(vaEndPicture(dpy, vpp_ctx));
            CHECK(vaSyncSurface(dpy, vpp_surface)); CHECK(vaDestroyBuffer(dpy, params));
            readback = vpp_surface;
        }

        VAImage img;
        CHECK(vaCreateImage(dpy, &fmt, W, H, &img));
        CHECK(vaGetImage(dpy, readback, 0, 0, W, H, img.image_id));
        void *p = NULL;
        CHECK(vaMapBuffer(dpy, img.buf, &p));
        if (img.num_planes != (planar ? 3u : 1u)) die("unexpected readback planes", VA_STATUS_ERROR_OPERATION_FAILED);
        for (unsigned plane = 0; plane < img.num_planes; plane++) {
            unsigned index = planar == VA_FOURCC_YV16 && plane ? 3 - plane : plane;
            size_t row = planar ? (plane ? W / 2 : W) : (size_t)W * (depth == 10 ? 4 : 2);
            for (int y = 0; y < H; y++)
                if (fwrite((unsigned char *)p + img.offsets[index] + (size_t)y * img.pitches[index], 1, row, stdout) != row) {
                    perror("write"); return 2;
                }
        }
        CHECK(vaUnmapBuffer(dpy, img.buf));
        CHECK(vaDestroyImage(dpy, img.image_id));

        vaDestroyBuffer(dpy, b_pp);
        vaDestroyBuffer(dpy, b_iq);
        vaDestroyBuffer(dpy, b_sp);
        vaDestroyBuffer(dpy, b_sd);
    }

    if (vpp_ctx != VA_INVALID_ID) vaDestroyContext(dpy, vpp_ctx);
    if (vpp_surface != VA_INVALID_ID) vaDestroySurfaces(dpy, &vpp_surface, 1);
    if (vpp_cfg != VA_INVALID_ID) vaDestroyConfig(dpy, vpp_cfg);
    vaDestroyContext(dpy, ctx);
    vaDestroySurfaces(dpy, surf, 8);
    vaDestroyConfig(dpy, cfg);
    vaTerminate(dpy);
    close(drm_fd);
    return 0;
}
