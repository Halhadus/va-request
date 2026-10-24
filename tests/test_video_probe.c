/*
 * Unit + live tests for vav4l2_pick_device_for_codec.
 * The picker is the shipped function in src/probe.c.
 */

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/videodev2.h>

#include "probe.h"

static int g_fail;

static void expect_eq(const char *got, const char *want, const char *tag)
{
    if (!want) {
        if (got) {
            fprintf(stderr, "FAIL %s: got %s, want NULL\n", tag, got);
            g_fail++;
        } else {
            printf("OK %s NULL\n", tag);
        }
        return;
    }
    if (!got || strcmp(got, want) != 0) {
        fprintf(stderr, "FAIL %s: got %s, want %s\n", tag,
                got ? got : "NULL", want);
        g_fail++;
        return;
    }
    printf("OK %s %s\n", tag, got);
}

static int node_has_fourcc(const char *path, uint32_t want)
{
    uint32_t fcs[VAV4L2_PROBE_MAX_FOURCCS];
    int fd, n, i;

    fd = open(path, O_RDWR | O_NONBLOCK);
    if (fd < 0)
        return 0;
    n = vav4l2_enum_output_fourccs(fd, fcs, VAV4L2_PROBE_MAX_FOURCCS);
    close(fd);
    if (n < 0)
        return 0;
    for (i = 0; i < n; i++) {
        if (fcs[i] == want)
            return 1;
    }
    return 0;
}

static void print_live_node(FILE *file, enum vav4l2_codec codec, const char *path)
{
    fprintf(file, "live %-7s-> %s\n", vav4l2_codec_name(codec), path && *path ? path : "(none)");
}

static void test_live_inventory(void)
{
    char records[256];
    FILE *file = fmemopen(records, sizeof(records), "w+");
    if (!file) { perror("live inventory fixture"); g_fail++; return; }
    print_live_node(file, VAV4L2_CODEC_H264, "/dev/video2048");
    print_live_node(file, VAV4L2_CODEC_HEVC, NULL);
    rewind(file);
    char kind[16], codec[16], arrow[3], path[64];
    for (unsigned i = 0; i < 2; i++) {
        if (fscanf(file, "%15s %15s %2s %63s", kind, codec, arrow, path) != 4) { g_fail++; break; }
        expect_eq(kind, "live", "live-record-kind"); expect_eq(arrow, "->", "live-record-separator");
        expect_eq(codec, i ? "HEVC" : "H.264", "live-record-codec");
        expect_eq(path, i ? "(none)" : "/dev/video2048", "live-record-node");
    }
    fclose(file);
}

static int run_unit(void)
{
    test_live_inventory();
    static const uint32_t rkvdec[] = {
        V4L2_PIX_FMT_HEVC_SLICE, V4L2_PIX_FMT_H264_SLICE
    };
    static const uint32_t av1[] = { V4L2_PIX_FMT_AV1_FRAME };
    static const uint32_t rga[] = { V4L2_PIX_FMT_NV12 };
    static const uint32_t mpegvp8[] = {
        v4l2_fourcc('M', 'G', '2', 'S'), v4l2_fourcc('V', 'P', '8', 'F')
    };
    struct vav4l2_node_fmts current[] = {
        { "/dev/video0", rga, 1, VAV4L2_REQAPI_UNKNOWN, 0, 0, .identity = {.driver = ""} },
        { "/dev/video1", rkvdec, 2, VAV4L2_REQAPI_UNKNOWN, 0, 0, .identity = {.driver = ""} },
        { "/dev/video2", mpegvp8, 2, VAV4L2_REQAPI_UNKNOWN, 0, 0, .identity = {.driver = ""} },
        { "/dev/video3", rga, 1, VAV4L2_REQAPI_UNKNOWN, 0, 0, .identity = {.driver = ""} },
        { "/dev/video4", av1, 1, VAV4L2_REQAPI_UNKNOWN, 0, 0, .identity = {.driver = ""} },
    };
    struct vav4l2_node_fmts swapped[] = {
        { "/dev/video1", av1, 1, VAV4L2_REQAPI_UNKNOWN, 0, 0, .identity = {.driver = ""} },
        { "/dev/video4", rkvdec, 2, VAV4L2_REQAPI_UNKNOWN, 0, 0, .identity = {.driver = ""} },
    };
    struct vav4l2_node_fmts none[] = {
        { "/dev/video0", rga, 1, VAV4L2_REQAPI_UNKNOWN, 0, 0, .identity = {.driver = ""} },
    };
    uint32_t fcc;
    char tag[8];

    printf("== multiple decoder and converter nodes ==\n");
    expect_eq(vav4l2_pick_device_for_codec(VAV4L2_CODEC_H264, current, 5),
              "/dev/video1", "h264-current");
    expect_eq(vav4l2_pick_device_for_codec(VAV4L2_CODEC_HEVC, current, 5),
              "/dev/video1", "hevc-current");
    expect_eq(vav4l2_pick_device_for_codec(VAV4L2_CODEC_AV1, current, 5),
              "/dev/video4", "av1-current");
    expect_eq(vav4l2_pick_device_for_codec(VAV4L2_CODEC_VP8, current, 5),
              "/dev/video2", "vp8-current");
    expect_eq(vav4l2_pick_device_for_codec(VAV4L2_CODEC_MPEG2, current, 5),
              "/dev/video2", "mpeg2-current");

    printf("== swapped /dev/video numbers ==\n");
    expect_eq(vav4l2_pick_device_for_codec(VAV4L2_CODEC_H264, swapped, 2),
              "/dev/video4", "h264-swapped");
    expect_eq(vav4l2_pick_device_for_codec(VAV4L2_CODEC_HEVC, swapped, 2),
              "/dev/video4", "hevc-swapped");
    expect_eq(vav4l2_pick_device_for_codec(VAV4L2_CODEC_AV1, swapped, 2),
              "/dev/video1", "av1-swapped");
    expect_eq(vav4l2_pick_device_for_codec(VAV4L2_CODEC_VP8, swapped, 2),
              NULL, "vp8-swapped-missing");
    expect_eq(vav4l2_pick_device_for_codec(VAV4L2_CODEC_MPEG2, swapped, 2),
              NULL, "mpeg2-swapped-missing");

    printf("== AV1 stub without request API, later sibling usable ==\n");
    {
        struct vav4l2_node_fmts stub_then_real[] = {
            { "/dev/video3", av1, 1, VAV4L2_REQAPI_NO, 0, 0, .identity = {.driver = ""} },
            { "/dev/video4", av1, 1, VAV4L2_REQAPI_YES, 0, 0, .identity = {.driver = ""} },
        };
        expect_eq(vav4l2_pick_device_for_codec(VAV4L2_CODEC_AV1, stub_then_real, 2),
                  "/dev/video4", "av1-skip-stub");
    }
    printf("== swapped numbers plus AV1 stub at video1 ==\n");
    {
        struct vav4l2_node_fmts swapped_stub[] = {
            { "/dev/video1", av1, 1, VAV4L2_REQAPI_NO, 0, 0, .identity = {.driver = ""} },
            { "/dev/video2", av1, 1, VAV4L2_REQAPI_YES, 0, 0, .identity = {.driver = ""} },
            { "/dev/video4", rkvdec, 2, VAV4L2_REQAPI_YES, 0, 0, .identity = {.driver = ""} },
        };
        expect_eq(vav4l2_pick_device_for_codec(VAV4L2_CODEC_AV1, swapped_stub, 3),
                  "/dev/video2", "av1-swapped-skip-stub");
        expect_eq(vav4l2_pick_device_for_codec(VAV4L2_CODEC_H264, swapped_stub, 3),
                  "/dev/video4", "h264-swapped-skip-stub");
        expect_eq(vav4l2_pick_device_for_codec(VAV4L2_CODEC_HEVC, swapped_stub, 3),
                  "/dev/video4", "hevc-swapped-skip-stub");
    }

    printf("== all AV1 nodes lack request API ==\n");
    {
        struct vav4l2_node_fmts all_stub[] = {
            { "/dev/video3", av1, 1, VAV4L2_REQAPI_NO, 0, 0, .identity = {.driver = ""} },
            { "/dev/video4", av1, 1, VAV4L2_REQAPI_NO, 0, 0, .identity = {.driver = ""} },
        };
        expect_eq(vav4l2_pick_device_for_codec(VAV4L2_CODEC_AV1, all_stub, 2),
                  NULL, "av1-all-stubs");
    }

    printf("== competing H.264 decoders, independent of node order ==\n");
    {
        struct vav4l2_node_fmts competing[] = {
            { "/dev/video3", rkvdec, 2, VAV4L2_REQAPI_YES, 1920u * 1088u, 0, .identity = {.driver = ""} },
            { "/dev/video4", rkvdec, 2, VAV4L2_REQAPI_YES, 4096u * 4096u,
              (1u << V4L2_MPEG_VIDEO_H264_PROFILE_HIGH_10) | (1u << V4L2_MPEG_VIDEO_H264_PROFILE_HIGH_422), .identity = {.driver = ""} },
        };
        expect_eq(vav4l2_pick_device_for_codec(VAV4L2_CODEC_H264, competing, 2),
                  "/dev/video4", "h264-prefers-profile-capabilities");
        competing[0].h264_profiles = competing[1].h264_profiles;
        expect_eq(vav4l2_pick_device_for_codec(VAV4L2_CODEC_H264, competing, 2),
                  "/dev/video4", "h264-prefers-frame-area");
        competing[1].request_api = VAV4L2_REQAPI_NO;
        expect_eq(vav4l2_pick_device_for_codec(VAV4L2_CODEC_H264, competing, 2),
                  "/dev/video3", "h264-capabilities-do-not-override-request-api");
        competing[1].request_api = VAV4L2_REQAPI_YES;
        struct vav4l2_node_fmts reversed[] = {competing[1], competing[0]};
        expect_eq(vav4l2_pick_device_for_codec(VAV4L2_CODEC_H264, reversed, 2),
                  "/dev/video4", "h264-capabilities-ignore-node-order");
    }

    printf("== missing fourcc ==\n");
    expect_eq(vav4l2_pick_device_for_codec(VAV4L2_CODEC_H264, none, 1),
              NULL, "h264-missing");
    expect_eq(vav4l2_pick_device_for_codec(VAV4L2_CODEC_AV1, none, 1),
              NULL, "av1-missing");
    expect_eq(vav4l2_pick_device_for_codec(VAV4L2_CODEC_VP8, none, 1),
              NULL, "vp8-missing");
    expect_eq(vav4l2_pick_device_for_codec(VAV4L2_CODEC_MPEG2, none, 1),
              NULL, "mpeg2-missing");

    if (vav4l2_codec_coded_fourcc(VAV4L2_CODEC_H264, &fcc) != 0 ||
        fcc != V4L2_PIX_FMT_H264_SLICE) {
        fprintf(stderr, "FAIL coded fourcc H.264\n");
        g_fail++;
    } else {
        vav4l2_fourcc_to_str(fcc, tag);
        printf("OK coded-fourcc H.264 %s\n", tag);
    }
    if (vav4l2_codec_coded_fourcc(VAV4L2_CODEC_VP8, &fcc) != 0 ||
        fcc != V4L2_PIX_FMT_VP8_FRAME) {
        fprintf(stderr, "FAIL coded fourcc VP8\n");
        g_fail++;
    } else {
        vav4l2_fourcc_to_str(fcc, tag);
        printf("OK coded-fourcc VP8 %s\n", tag);
    }
    if (vav4l2_codec_coded_fourcc(VAV4L2_CODEC_MPEG2, &fcc) != 0 ||
        fcc != V4L2_PIX_FMT_MPEG2_SLICE) {
        fprintf(stderr, "FAIL coded fourcc MPEG-2\n");
        g_fail++;
    } else {
        vav4l2_fourcc_to_str(fcc, tag);
        printf("OK coded-fourcc MPEG-2 %s\n", tag);
    }
    return g_fail ? 1 : 0;
}

static int run_live(void)
{
    char paths[5][64] = {{0}};
    vav4l2_scan_decoder_paths_ex(paths[0], paths[1], paths[2], paths[3], paths[4], 64);
    const enum vav4l2_codec codecs[] = {VAV4L2_CODEC_H264, VAV4L2_CODEC_HEVC, VAV4L2_CODEC_AV1, VAV4L2_CODEC_VP8, VAV4L2_CODEC_MPEG2};
    unsigned found = 0;
    for (unsigned i = 0; i < 5; i++) {
        uint32_t fcc;
        print_live_node(stdout, codecs[i], paths[i]);
        if (!paths[i][0]) { printf("SKIP %s: no usable node\n", vav4l2_codec_name(codecs[i])); continue; }
        found++;
        if (vav4l2_codec_coded_fourcc(codecs[i], &fcc) < 0 || !node_has_fourcc(paths[i], fcc)) g_fail++;
        else printf("OK %s %s\n", vav4l2_codec_name(codecs[i]), paths[i]);
    }
    return g_fail ? 1 : found ? 0 : 77;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--live") == 0)
        return run_live();
    return run_unit();
}
