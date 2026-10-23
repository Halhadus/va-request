/*
 * v4l2request — locate decoder nodes by OUTPUT_MPLANE fourcc
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#include <linux/media.h>
#include <sys/stat.h>
#include <ctype.h>
#include <inttypes.h>
#include <sys/sysmacros.h>
#include <time.h>

#include "probe.h"

void vareq_fourcc_to_str(uint32_t fourcc, char out[5])
{
    out[0] = (char)(fourcc & 0xff);
    out[1] = (char)((fourcc >> 8) & 0xff);
    out[2] = (char)((fourcc >> 16) & 0xff);
    out[3] = (char)((fourcc >> 24) & 0xff);
    out[4] = 0;
}

const char *vareq_pick_device_for_codec(enum vareq_codec codec,
                                         const struct vareq_node_fmts *nodes,
                                         unsigned n_nodes)
{
    uint32_t want;
    unsigned i, j;
    const struct vareq_node_fmts *best = NULL;
    unsigned best_profiles = 0;
    int best_score = -1;

    if (!nodes || vareq_codec_coded_fourcc(codec, &want) < 0)
        return NULL;

    for (i = 0; i < n_nodes; i++) {
        if (!nodes[i].path || !nodes[i].fourccs)
            continue;
        if (nodes[i].request_api == VAREQ_REQAPI_NO)
            continue;
        for (j = 0; j < nodes[i].n_fourccs; j++) {
            if (nodes[i].fourccs[j] != want)
                continue;
            struct vareq_compat_codec job = {.direction = VAREQ_COMPAT_DECODE, .coded_format = want};
            int score = vareq_compat_codec_score(&nodes[i].identity, &job);
            if (score < 0) continue;
            unsigned profiles = codec == VAREQ_CODEC_H264 ?
                !!(nodes[i].h264_profiles & (1u << V4L2_MPEG_VIDEO_H264_PROFILE_HIGH_10)) +
                2 * !!(nodes[i].h264_profiles & (1u << V4L2_MPEG_VIDEO_H264_PROFILE_HIGH_422)) : 0;
            if (!best || score > best_score || (score == best_score && (profiles > best_profiles ||
                (codec == VAREQ_CODEC_H264 && profiles == best_profiles && nodes[i].h264_max_area > best->h264_max_area)))) {
                best = &nodes[i];
                best_profiles = profiles; best_score = score;
            }
            break;
        }
    }
    return best ? best->path : NULL;
}

int vareq_find_media_path(const char *video_path, char *out, unsigned out_len)
{
    if (!video_path || !out || !out_len) return -1;
    out[0] = 0;
    struct stat node;
    if (stat(video_path, &node) < 0 || !S_ISCHR(node.st_mode)) return -1;
    DIR *directory = opendir("/dev"); if (!directory) return -1;
    struct dirent *entry; int found = -1;
    while ((entry = readdir(directory))) {
        if (strncmp(entry->d_name, "media", 5) || !isdigit((unsigned char)entry->d_name[5])) continue;
        const char *suffix = entry->d_name + 5;
        while (isdigit((unsigned char)*suffix)) suffix++;
        if (*suffix) continue;
        char path[64];
        if (snprintf(path, sizeof(path), "/dev/%s", entry->d_name) >= (int)sizeof(path)) continue;
        int fd = open(path, O_RDWR | O_CLOEXEC); if (fd < 0) continue;
        struct media_v2_topology top = {0};
        if (ioctl(fd, MEDIA_IOC_G_TOPOLOGY, &top) < 0 || !top.num_interfaces || top.num_interfaces > 4096) { close(fd); continue; }
        unsigned capacity = top.num_interfaces;
        struct media_v2_interface *interfaces = calloc(capacity, sizeof(*interfaces));
        if (!interfaces) { close(fd); break; }
        top.ptr_interfaces = (uintptr_t)interfaces;
        if (ioctl(fd, MEDIA_IOC_G_TOPOLOGY, &top) == 0 && top.num_interfaces <= capacity) {
            for (unsigned i = 0; i < top.num_interfaces; i++) if (interfaces[i].intf_type == MEDIA_INTF_T_V4L_VIDEO &&
                interfaces[i].devnode.major == major(node.st_rdev) && interfaces[i].devnode.minor == minor(node.st_rdev)) {
                if (strlen(path) < out_len) { memcpy(out, path, strlen(path) + 1); found = 0; }
                break;
            }
        }
        free(interfaces); close(fd); if (!found) break;
    }
    closedir(directory); return found;
}

static int queue_has_requests(int fd)
{
    struct v4l2_create_buffers buffers = {.format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, .memory = V4L2_MEMORY_MMAP};
    return ioctl(fd, VIDIOC_G_FMT, &buffers.format) == 0 && ioctl(fd, VIDIOC_CREATE_BUFS, &buffers) == 0 && (buffers.capabilities & V4L2_BUF_CAP_SUPPORTS_REQUESTS);
}

int vareq_video_has_request_api(const char *video_path)
{
    char media_path[128];
    int mfd, reqfd = -1;

    int video = open(video_path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (video < 0) return 0;
    int usable = queue_has_requests(video);
    close(video); if (!usable) return 0;
    if (vareq_find_media_path(video_path, media_path, sizeof(media_path)) < 0)
        return 0;
    mfd = open(media_path, O_RDWR | O_CLOEXEC);
    if (mfd < 0)
        return 0;
    if (ioctl(mfd, MEDIA_IOC_REQUEST_ALLOC, &reqfd) < 0) {
        close(mfd);
        return 0;
    }
    if (reqfd >= 0)
        close(reqfd);
    close(mfd);
    return 1;
}

static int fourcc_is_coded(uint32_t fcc)
{
    return fcc == V4L2_PIX_FMT_H264_SLICE ||
           fcc == V4L2_PIX_FMT_HEVC_SLICE ||
           fcc == V4L2_PIX_FMT_AV1_FRAME ||
           fcc == V4L2_PIX_FMT_VP8_FRAME ||
           fcc == V4L2_PIX_FMT_MPEG2_SLICE;
}

int vareq_enum_output_fourccs(int fd, uint32_t *out, unsigned max)
{
    struct v4l2_fmtdesc fmt;
    unsigned n = 0;

    if (fd < 0 || !out || max == 0)
        return -1;

    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    for (fmt.index = 0; n < max; fmt.index++) {
        if (ioctl(fd, VIDIOC_ENUM_FMT, &fmt) < 0)
            break;
        out[n++] = fmt.pixelformat;
    }
    return (int)n;
}

static void copy_path(char *dst, unsigned dst_len, const char *src)
{
    if (!dst || dst_len == 0)
        return;
    if (!src) {
        dst[0] = 0;
        return;
    }
    size_t count = strnlen(src, dst_len - 1);
    memcpy(dst, src, count); dst[count] = 0;
}

static int fourcc_in(const uint32_t *list, unsigned n, uint32_t fcc);

struct scan_node {
    char path[64];
    struct vareq_compat_identity identity;
    int nout, ncap;
    uint32_t out_fcc[VAREQ_PROBE_MAX_FOURCCS];
    uint32_t cap_fcc[VAREQ_PROBE_MAX_FOURCCS];
    int has_jpeg_cap;
    int has_jpeg_encoder;
    int has_rotate;
    unsigned controls;
    uint64_t h264_max_area;
    uint32_t h264_profiles;
};

static int node_has_coded_output(const struct scan_node *nd)
{
    int k;

    for (k = 0; k < nd->nout; k++)
        if (fourcc_is_coded(nd->out_fcc[k]))
            return 1;
    return 0;
}

/* Query actual H.264 capability controls; node numbers and driver names
 * are not capabilities. CAPTURE formats alone depend on the current SPS. */
static void probe_h264_caps(int fd, struct scan_node *node)
{
    struct v4l2_queryctrl ctrl = { .id = V4L2_CID_MPEG_VIDEO_H264_PROFILE };
    struct v4l2_frmsizeenum size = { .pixel_format = V4L2_PIX_FMT_H264_SLICE };

    node->h264_max_area = 0;
    node->h264_profiles = 0;
    if (!fourcc_in(node->out_fcc, (unsigned)node->nout, V4L2_PIX_FMT_H264_SLICE))
        return;
    if (ioctl(fd, VIDIOC_QUERYCTRL, &ctrl) == 0 && !(ctrl.flags & V4L2_CTRL_FLAG_DISABLED) &&
        ctrl.minimum >= 0 && ctrl.maximum < 32) {
        for (int i = ctrl.minimum; i <= ctrl.maximum; i++) {
            struct v4l2_querymenu menu = { .id = ctrl.id, .index = (unsigned)i };
            if (ioctl(fd, VIDIOC_QUERYMENU, &menu) == 0)
                node->h264_profiles |= 1u << i;
        }
    }
    for (size.index = 0; size.index < 64; size.index++) {
        uint64_t area;
        if (ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &size) < 0)
            break;
        if (size.type == V4L2_FRMSIZE_TYPE_DISCRETE)
            area = (uint64_t)size.discrete.width * size.discrete.height;
        else if (size.type == V4L2_FRMSIZE_TYPE_STEPWISE || size.type == V4L2_FRMSIZE_TYPE_CONTINUOUS)
            area = (uint64_t)size.stepwise.max_width * size.stepwise.max_height;
        else
            break;
        if (area > node->h264_max_area)
            node->h264_max_area = area;
        if (size.type != V4L2_FRMSIZE_TYPE_DISCRETE)
            break;
    }
}

int vareq_query_profiles(const char *path, uint32_t control, uint32_t *profiles)
{
    if (!path || !profiles) return -1;
    int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    struct v4l2_queryctrl ctrl = {.id = control};
    if (ioctl(fd, VIDIOC_QUERYCTRL, &ctrl) < 0 || (ctrl.flags & V4L2_CTRL_FLAG_DISABLED) ||
        ctrl.minimum < 0 || ctrl.maximum >= 32 || ctrl.minimum > ctrl.maximum) { close(fd); return -1; }
    uint32_t mask = 0;
    for (int i = ctrl.minimum; i <= ctrl.maximum; i++) {
        struct v4l2_querymenu menu = {.id = ctrl.id, .index = i};
        if (ioctl(fd, VIDIOC_QUERYMENU, &menu) == 0) mask |= 1u << i;
    }
    close(fd); *profiles = mask;
    return 0;
}

int vareq_query_h264_profiles(const char *path, uint32_t *profiles)
{
    return vareq_query_profiles(path, V4L2_CID_MPEG_VIDEO_H264_PROFILE, profiles);
}

static int jpeg_encoder_usable(int fd)
{
    if (!vareq_codec_enabled(VAREQ_CODEC_JPEG_ENC)) return 0;
    struct v4l2_format fmt = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE};
    if (ioctl(fd, VIDIOC_G_FMT, &fmt) < 0) return 0;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_JPEG;
    fmt.fmt.pix_mp.num_planes = 1;
    if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0 || fmt.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_JPEG || fmt.fmt.pix_mp.num_planes != 1) return 0;
    if (!vareq_codec_controls_usable(fd, VAREQ_CODEC_JPEG_ENC)) return 0;
    /* Encoder OUTPUT menus depend on the selected coded CAPTURE format. */
    uint32_t formats[VAREQ_PROBE_MAX_FOURCCS];
    int count = vareq_enum_output_fourccs(fd, formats, VAREQ_PROBE_MAX_FOURCCS);
    return count > 0 && (fourcc_in(formats, count, V4L2_PIX_FMT_NV12) || fourcc_in(formats, count, V4L2_PIX_FMT_NV12M) || fourcc_in(formats, count, V4L2_PIX_FMT_YUV420M));
}

/* Discover numeric video nodes and collect formats, controls and identity. */
static int video_entry(const struct dirent *entry)
{
    const unsigned char *p = (const unsigned char *)entry->d_name;
    if (strncmp((const char *)p, "video", 5) || !isdigit(p[5])) return 0;
    for (p += 5; *p; p++) if (!isdigit(*p)) return 0;
    return 1;
}
int vareq_list_video_nodes(char (*paths)[64], unsigned capacity)
{
    if (!paths || !capacity) { errno = EINVAL; return -1; }
    struct dirent **entries = NULL;
    int count = scandir("/dev", &entries, video_entry, versionsort);
    if (count < 0) return -1;
    int truncated = 0;
    for (int i = 0; i < count; i++) {
        size_t len = strlen(entries[i]->d_name);
        if (len > 58) truncated = 1;
        else if ((unsigned)i < capacity) { memcpy(paths[i], "/dev/", 5); memcpy(paths[i] + 5, entries[i]->d_name, len + 1); }
        free(entries[i]);
    }
    free(entries);
    if (truncated || (unsigned)count > capacity) { errno = EOVERFLOW; return -1; }
    return count;
}

static int scan_nodes(struct scan_node *nodes, int max_nodes)
{
    int n = 0;
    char paths[VAREQ_PROBE_MAX_NODES][64];
    int count = vareq_list_video_nodes(paths, VAREQ_PROBE_MAX_NODES);
    if (count < 0) return 0;

    for (int i = 0; i < count && n < max_nodes; i++) {
        struct v4l2_capability cap;
        struct v4l2_queryctrl qc;
        int fd;

        memset(&nodes[n], 0, sizeof(nodes[n]));
        copy_path(nodes[n].path, sizeof(nodes[n].path), paths[i]);
        fd = open(nodes[n].path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;
        memset(&cap, 0, sizeof(cap));
        if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0 ||
            !vareq_node_is_m2m(&cap)) {
            close(fd);
            continue;
        }
        nodes[n].nout = vareq_enum_output_fourccs(fd, nodes[n].out_fcc,
                                                   VAREQ_PROBE_MAX_FOURCCS);
        for (unsigned codec = VAREQ_CODEC_H264; codec <= VAREQ_CODEC_MPEG2; codec++) {
            uint32_t fcc;
            if (vareq_codec_coded_fourcc(codec, &fcc) < 0) continue;
            for (int j = 0; j < nodes[n].nout; j++) if (nodes[n].out_fcc[j] == fcc && !vareq_codec_controls_usable(fd, codec)) nodes[n].out_fcc[j] = 0;
        }
        nodes[n].ncap = vareq_enum_capture_fourccs(fd, nodes[n].cap_fcc,
                                                    VAREQ_PROBE_MAX_FOURCCS);
        vareq_compat_identity(&cap, &nodes[n].identity);
        nodes[n].controls = 0;
        const uint32_t ids[] = {V4L2_CID_ROTATE, V4L2_CID_HFLIP, V4L2_CID_VFLIP};
        for (unsigned j = 0; j < 3; j++) {
            memset(&qc, 0, sizeof(qc)); qc.id = ids[j];
            if (ioctl(fd, VIDIOC_QUERYCTRL, &qc) == 0 && !(qc.flags & (V4L2_CTRL_FLAG_DISABLED | V4L2_CTRL_FLAG_READ_ONLY)))
                nodes[n].controls |= 1u << j;
        }
        nodes[n].has_rotate = !!(nodes[n].controls & VAREQ_VPP_ROTATE);
        probe_h264_caps(fd, &nodes[n]);
        nodes[n].has_jpeg_cap = fourcc_in(nodes[n].cap_fcc, nodes[n].ncap, V4L2_PIX_FMT_JPEG);
        nodes[n].has_jpeg_encoder = nodes[n].has_jpeg_cap && jpeg_encoder_usable(fd);
        close(fd);

        if (nodes[n].nout <= 0 && nodes[n].ncap <= 0)
            continue;
        n++;
    }
    return n;
}

int vareq_vpp_formats_usable(const uint32_t *input, unsigned ni, const uint32_t *output, unsigned no)
{
    const uint32_t raw[] = {V4L2_PIX_FMT_NV12, V4L2_PIX_FMT_NV21, V4L2_PIX_FMT_YUV420, V4L2_PIX_FMT_NV16, V4L2_PIX_FMT_NV61, V4L2_PIX_FMT_YUYV, V4L2_PIX_FMT_UYVY, V4L2_PIX_FMT_VYUY, V4L2_PIX_FMT_YVYU,
        V4L2_PIX_FMT_ARGB32, V4L2_PIX_FMT_ABGR32, V4L2_PIX_FMT_XBGR32, V4L2_PIX_FMT_RGBA32, V4L2_PIX_FMT_RGBX32, V4L2_PIX_FMT_RGB565, V4L2_PIX_FMT_YUV422P};
    int in = 0, out = 0;
    for (unsigned i = 0; i < sizeof(raw) / sizeof(raw[0]); i++) {
        in |= fourcc_in(input, ni, raw[i]); out |= fourcc_in(output, no, raw[i]);
    }
    return in && out;
}

void vareq_scan_vpp_devices(struct vareq_driver_data *dd)
{
    if (dd->vpp_devices_scanned) return;
    struct scan_node nodes[VAREQ_PROBE_MAX_NODES];
    int n = scan_nodes(nodes, VAREQ_PROBE_MAX_NODES);
    dd->num_vpp_devices = 0;
    for (int i = 0; i < n && dd->num_vpp_devices < VAREQ_VPP_MAX_DEVICES; i++) {
        struct scan_node *s = &nodes[i];
        if (node_has_coded_output(s) || s->has_jpeg_cap ||
            !vareq_vpp_formats_usable(s->out_fcc, s->nout, s->cap_fcc, s->ncap)) continue;
        struct vareq_vpp_device *d = &dd->vpp_devices[dd->num_vpp_devices++];
        copy_path(d->path, sizeof(d->path), s->path);
        d->identity = s->identity; d->controls = s->controls;
        d->num_input = s->nout; d->num_output = s->ncap;
        memcpy(d->input, s->out_fcc, s->nout * sizeof(uint32_t));
        memcpy(d->output, s->cap_fcc, s->ncap * sizeof(uint32_t));
        int fd = open(d->path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) { dd->num_vpp_devices--; continue; }
        int valid = 1;
        for (unsigned cap = 0; cap < 2; cap++) for (unsigned maximum = 0; maximum < 2; maximum++) {
            struct v4l2_format f = {.type = cap ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE : V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE};
            f.fmt.pix_mp.pixelformat = cap ? d->output[0] : d->input[0];
            f.fmt.pix_mp.width = f.fmt.pix_mp.height = maximum ? 65535 : 1;
            f.fmt.pix_mp.field = V4L2_FIELD_NONE;
            if (ioctl(fd, VIDIOC_TRY_FMT, &f) < 0 || !f.fmt.pix_mp.width || !f.fmt.pix_mp.height) { valid = 0; continue; }
            (maximum ? d->max_width : d->min_width)[cap] = f.fmt.pix_mp.width;
            (maximum ? d->max_height : d->min_height)[cap] = f.fmt.pix_mp.height;
        }
        close(fd);
        if (!valid) dd->num_vpp_devices--;
    }
    dd->vpp_devices_scanned = 1;
}

/* Fill one vareq_node_fmts decoder candidate out of a walked node. */
static void fill_decoder_node(struct vareq_node_fmts *dst,
                              const struct scan_node *src)
{
    dst->identity = src->identity;
    dst->path = src->path;
    dst->fourccs = src->out_fcc;
    dst->n_fourccs = (unsigned)src->nout;
    dst->h264_max_area = src->h264_max_area;
    dst->h264_profiles = src->h264_profiles;
    /* REQUEST_ALLOC at most once per candidate node per boot — the cache
     * makes sure later processes never repeat it. */
    dst->request_api = vareq_video_has_request_api(src->path)
        ? VAREQ_REQAPI_YES : VAREQ_REQAPI_NO;
}

int vareq_scan_decoder_paths_ex(char *h264_out, char *hevc_out, char *av1_out,
                                 char *vp8_out, char *mpeg2_out,
                                 unsigned out_len)
{
    struct vareq_node_fmts nodes[VAREQ_PROBE_MAX_NODES];
    struct scan_node raw[VAREQ_PROBE_MAX_NODES];
    int n_raw, i, n_nodes = 0;
    const char *p;

    if (h264_out && out_len)
        h264_out[0] = 0;
    if (hevc_out && out_len)
        hevc_out[0] = 0;
    if (av1_out && out_len)
        av1_out[0] = 0;
    if (vp8_out && out_len)
        vp8_out[0] = 0;
    if (mpeg2_out && out_len)
        mpeg2_out[0] = 0;

    n_raw = scan_nodes(raw, VAREQ_PROBE_MAX_NODES);
    for (i = 0; i < n_raw && n_nodes < VAREQ_PROBE_MAX_NODES; i++) {
        if (raw[i].has_jpeg_cap || !node_has_coded_output(&raw[i]))
            continue; /* JPEG encoders and raw converters are not decoders */
        fill_decoder_node(&nodes[n_nodes], &raw[i]);
        if (nodes[n_nodes].request_api == VAREQ_REQAPI_NO)
            fprintf(stderr,
                    "v4l2request: skip %s (coded fourcc, no media request)\n",
                    raw[i].path);
        n_nodes++;
    }

    p = vareq_pick_device_for_codec(VAREQ_CODEC_H264, nodes, n_nodes);
    copy_path(h264_out, out_len, p);
    p = vareq_pick_device_for_codec(VAREQ_CODEC_HEVC, nodes, n_nodes);
    copy_path(hevc_out, out_len, p);
    p = vareq_pick_device_for_codec(VAREQ_CODEC_AV1, nodes, n_nodes);
    copy_path(av1_out, out_len, p);
    p = vareq_pick_device_for_codec(VAREQ_CODEC_VP8, nodes, n_nodes);
    copy_path(vp8_out, out_len, p);
    p = vareq_pick_device_for_codec(VAREQ_CODEC_MPEG2, nodes, n_nodes);
    copy_path(mpeg2_out, out_len, p);

    return 0;
}

int vareq_enum_capture_fourccs(int fd, uint32_t *out, unsigned max)
{
    struct v4l2_fmtdesc fmt;
    unsigned n = 0;

    if (fd < 0 || !out || max == 0)
        return -1;

    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    for (fmt.index = 0; n < max; fmt.index++) {
        if (ioctl(fd, VIDIOC_ENUM_FMT, &fmt) < 0)
            break;
        out[n++] = fmt.pixelformat;
    }
    return (int)n;
}

static int fourcc_in(const uint32_t *list, unsigned n, uint32_t fcc)
{
    unsigned i;
    for (i = 0; i < n; i++)
        if (list[i] == fcc)
            return 1;
    return 0;
}

/* Reuse discovery only in a private runtime directory. Every cached selection
 * is checked against current formats/controls; topology changes trigger a scan. */
static void probe_cache_path(char *out, unsigned len)
{
    const char *dir = getenv("XDG_RUNTIME_DIR"); struct stat st;
    out[0] = 0;
    if (!dir || dir[0] != '/' || lstat(dir, &st) < 0 || !S_ISDIR(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0077)) return;
    snprintf(out, len, "%s/va-request-probe-v1.cache", dir);
}
static uint64_t topology_key(void)
{
    char paths[VAREQ_PROBE_MAX_NODES][64]; int count = vareq_list_video_nodes(paths, VAREQ_PROBE_MAX_NODES);
    uint64_t hash = UINT64_C(14695981039346656037);
    if (count < 0) return 0;
    struct stat dev;
    if (stat("/dev", &dev) < 0) return 0;
    hash = (hash ^ dev.st_mtim.tv_sec ^ dev.st_mtim.tv_nsec) * UINT64_C(1099511628211);
    for (int i = 0; i < count; i++) {
        struct stat st;
        if (stat(paths[i], &st) < 0) return 0;
        const uint64_t values[] = {st.st_rdev, st.st_ino, st.st_mode, st.st_uid, st.st_gid, st.st_ctim.tv_sec, st.st_ctim.tv_nsec};
        for (const unsigned char *c = (const unsigned char *)paths[i]; *c; c++) hash = (hash ^ *c) * UINT64_C(1099511628211);
        for (unsigned j = 0; j < sizeof(values) / sizeof(values[0]); j++) hash = (hash ^ values[j]) * UINT64_C(1099511628211);
    }
    return hash;
}
static int valid_video_path(const char *path)
{
    if (strncmp(path, "/dev/video", 10) || !isdigit((unsigned char)path[10])) return 0;
    for (const unsigned char *p = (const unsigned char *)path + 10; *p; p++) if (!isdigit(*p)) return 0;
    return 1;
}
static int validate_cached_node(const char *path, enum vareq_codec codec)
{
    if (!path || !*path) return 1;
    if (!valid_video_path(path) || !vareq_codec_enabled(codec)) return 0;
    int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC); if (fd < 0) return 0;
    struct v4l2_capability cap = {0}; uint32_t formats[VAREQ_PROBE_MAX_FOURCCS], capture[VAREQ_PROBE_MAX_FOURCCS], want;
    int valid = ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0 && vareq_node_is_m2m(&cap);
    int count = valid ? vareq_enum_output_fourccs(fd, formats, VAREQ_PROBE_MAX_FOURCCS) : -1;
    if (codec <= VAREQ_CODEC_MPEG2) {
        valid = count > 0 && vareq_codec_coded_fourcc(codec, &want) == 0 && fourcc_in(formats, count, want) && vareq_codec_controls_usable(fd, codec) && queue_has_requests(fd);
    } else {
        int nc = valid ? vareq_enum_capture_fourccs(fd, capture, VAREQ_PROBE_MAX_FOURCCS) : -1;
        valid = nc > 0 && (codec == VAREQ_CODEC_JPEG_ENC ? fourcc_in(capture, nc, V4L2_PIX_FMT_JPEG) && jpeg_encoder_usable(fd) : count > 0 && vareq_vpp_formats_usable(formats, count, capture, nc));
    }
    if (valid && codec != VAREQ_CODEC_VPP) {
        struct vareq_compat_identity identity; vareq_compat_identity(&cap, &identity);
        struct vareq_compat_codec job = {.direction = codec == VAREQ_CODEC_JPEG_ENC ? VAREQ_COMPAT_ENCODE : VAREQ_COMPAT_DECODE,
            .coded_format = V4L2_PIX_FMT_JPEG};
        if (codec != VAREQ_CODEC_JPEG_ENC) valid = vareq_codec_coded_fourcc(codec, &job.coded_format) == 0;
        if (valid) valid = vareq_compat_codec_score(&identity, &job) >= 0;
    }
    close(fd); return valid;
}

static int probe_read_boot_id(char *out, unsigned len)
{
    FILE *f = fopen("/proc/sys/kernel/random/boot_id", "r");
    size_t n;

    if (!f)
        return -1;
    if (!fgets(out, (int)len, f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    n = strlen(out);
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r'))
        out[--n] = 0;
    return n ? 0 : -1;
}

static int probe_cache_load(const char *path, const char *boot,
                            char *h264, char *hevc, char *av1, char *vp8,
                            char *mpeg2, char *jpeg, char *vpp, unsigned len)
{
    char line[160];
    unsigned seen = 0;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    struct stat st;
    if (fd < 0) return -1;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0077) || st.st_size > 2048 || time(NULL) < st.st_mtime || time(NULL) - st.st_mtime > 60) { close(fd); return -1; }
    char *val;
    FILE *f = fdopen(fd, "r");

    if (!f) { close(fd); return -1; }
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return -1;
    }
    line[strcspn(line, "\r\n")] = 0;
    if (strncmp(line, "boot4 ", 6) != 0 || strcmp(line + 6, boot) != 0) {
        fclose(f);
        return -1;
    }
    while (fgets(line, sizeof(line), f)) {
        val = strchr(line, '=');
        if (!val)
            continue;
        *val++ = 0;
        line[strcspn(line, "\r\n")] = 0;
        val[strcspn(val, "\r\n")] = 0;
        if (!strcmp(line, "h264"))
            { if (strlen(val) >= len || (val[0] && !valid_video_path(val))) { fclose(f); return -1; } copy_path(h264, len, val); seen |= 1u << 0; }
        else if (!strcmp(line, "hevc"))
            { if (strlen(val) >= len || (val[0] && !valid_video_path(val))) { fclose(f); return -1; } copy_path(hevc, len, val); seen |= 1u << 1; }
        else if (!strcmp(line, "av1"))
            { if (strlen(val) >= len || (val[0] && !valid_video_path(val))) { fclose(f); return -1; } copy_path(av1, len, val); seen |= 1u << 2; }
        else if (!strcmp(line, "vp8"))
            { if (strlen(val) >= len || (val[0] && !valid_video_path(val))) { fclose(f); return -1; } copy_path(vp8, len, val); seen |= 1u << 3; }
        else if (!strcmp(line, "mpeg2"))
            { if (strlen(val) >= len || (val[0] && !valid_video_path(val))) { fclose(f); return -1; } copy_path(mpeg2, len, val); seen |= 1u << 4; }
        else if (!strcmp(line, "jpeg"))
            { if (strlen(val) >= len || (val[0] && !valid_video_path(val))) { fclose(f); return -1; } copy_path(jpeg, len, val); seen |= 1u << 5; }
        else if (!strcmp(line, "vpp"))
            { if (strlen(val) >= len || (val[0] && !valid_video_path(val))) { fclose(f); return -1; } copy_path(vpp, len, val); seen |= 1u << 6; }
    }
    fclose(f);
    return seen == 127 ? 0 : -1;
}

static void probe_cache_store(const char *path, const char *boot,
                              const char *h264, const char *hevc,
                              const char *av1, const char *vp8,
                              const char *mpeg2, const char *jpeg,
                              const char *vpp)
{
    char tmp[300];
    FILE *f;

    snprintf(tmp, sizeof(tmp), "%s.XXXXXX", path);
    int fd = mkstemp(tmp);
    if (fd < 0) return;
    f = fdopen(fd, "w");
    if (!f) { close(fd); unlink(tmp); return; }
    if (!f)
        return;
    fprintf(f, "boot4 %s\n", boot);
    fprintf(f, "h264=%s\nhevc=%s\nav1=%s\nvp8=%s\nmpeg2=%s\njpeg=%s\nvpp=%s\n",
            h264, hevc, av1, vp8, mpeg2, jpeg, vpp);
    if (fclose(f) == 0)
        rename(tmp, path);
    else
        unlink(tmp);
}

/* Select usable decoder, encoder and raw-processing nodes. */
static int scan_all_uncached(char *h264_out, char *hevc_out, char *av1_out,
                             char *vp8_out, char *mpeg2_out,
                             char *jpeg_enc_out, char *vpp_out, unsigned out_len)
{
    struct vareq_node_fmts nodes[VAREQ_PROBE_MAX_NODES];
    struct scan_node raw[VAREQ_PROBE_MAX_NODES];
    int n_raw, i, n_nodes = 0, vpp_rotate = 0, jpeg_score = -1;
    const char *p;

    if (h264_out && out_len) h264_out[0] = 0;
    if (hevc_out && out_len) hevc_out[0] = 0;
    if (av1_out && out_len) av1_out[0] = 0;
    if (vp8_out && out_len) vp8_out[0] = 0;
    if (mpeg2_out && out_len) mpeg2_out[0] = 0;
    if (jpeg_enc_out && out_len) jpeg_enc_out[0] = 0;
    if (vpp_out && out_len) vpp_out[0] = 0;

    n_raw = scan_nodes(raw, VAREQ_PROBE_MAX_NODES);
    for (i = 0; i < n_raw && n_nodes < VAREQ_PROBE_MAX_NODES; i++) {
        if (raw[i].has_jpeg_encoder && jpeg_enc_out && out_len) {
            struct vareq_compat_codec job = {.direction = VAREQ_COMPAT_ENCODE, .coded_format = V4L2_PIX_FMT_JPEG};
            int score = vareq_compat_codec_score(&raw[i].identity, &job);
            if (score >= 0 && score > jpeg_score) { copy_path(jpeg_enc_out, out_len, raw[i].path); jpeg_score = score; }
        }
        if (vareq_codec_enabled(VAREQ_CODEC_VPP) && !raw[i].has_jpeg_cap &&
            !node_has_coded_output(&raw[i]) &&
            vareq_vpp_formats_usable(raw[i].out_fcc, raw[i].nout, raw[i].cap_fcc, raw[i].ncap) &&
            vpp_out && out_len && (!vpp_out[0] || (raw[i].has_rotate && !vpp_rotate))) {
            copy_path(vpp_out, out_len, raw[i].path);
            vpp_rotate = raw[i].has_rotate;
        }
        if (!node_has_coded_output(&raw[i]))
            continue;
        fill_decoder_node(&nodes[n_nodes], &raw[i]);
        n_nodes++;
    }

    p = vareq_pick_device_for_codec(VAREQ_CODEC_H264, nodes, n_nodes);
    copy_path(h264_out, out_len, p);
    p = vareq_pick_device_for_codec(VAREQ_CODEC_HEVC, nodes, n_nodes);
    copy_path(hevc_out, out_len, p);
    p = vareq_pick_device_for_codec(VAREQ_CODEC_AV1, nodes, n_nodes);
    copy_path(av1_out, out_len, p);
    p = vareq_pick_device_for_codec(VAREQ_CODEC_VP8, nodes, n_nodes);
    copy_path(vp8_out, out_len, p);
    p = vareq_pick_device_for_codec(VAREQ_CODEC_MPEG2, nodes, n_nodes);
    copy_path(mpeg2_out, out_len, p);
    return 0;
}

int vareq_scan_all_cached(char *h264_out, char *hevc_out, char *av1_out,
                           char *vp8_out, char *mpeg2_out,
                           char *jpeg_enc_out, char *vpp_out,
                           unsigned out_len)
{
    char path[280];
    char boot[96], id[64];
    uint64_t key = topology_key();

    probe_cache_path(path, sizeof(path));

    if (probe_read_boot_id(id, sizeof(id)) < 0) id[0] = 0;
    snprintf(boot, sizeof(boot), "%s-%016" PRIx64, id, key);
    if (path[0] && id[0] && key && !getenv("VAREQ_PROBE_NOCACHE") &&
        probe_cache_load(path, boot, h264_out, hevc_out, av1_out, vp8_out,
                         mpeg2_out, jpeg_enc_out, vpp_out, out_len) == 0 &&
        validate_cached_node(h264_out, VAREQ_CODEC_H264) && validate_cached_node(hevc_out, VAREQ_CODEC_HEVC) &&
        validate_cached_node(av1_out, VAREQ_CODEC_AV1) && validate_cached_node(vp8_out, VAREQ_CODEC_VP8) &&
        validate_cached_node(mpeg2_out, VAREQ_CODEC_MPEG2) && validate_cached_node(jpeg_enc_out, VAREQ_CODEC_JPEG_ENC) && validate_cached_node(vpp_out, VAREQ_CODEC_VPP)) {
        fprintf(stderr, "v4l2request: probe cache hit (%s)\n", path);
        return 0;
    }

    scan_all_uncached(h264_out, hevc_out, av1_out, vp8_out, mpeg2_out,
                      jpeg_enc_out, vpp_out, out_len);

    if (path[0] && id[0] && key && !getenv("VAREQ_PROBE_NOCACHE"))
        probe_cache_store(path, boot,
                          h264_out && h264_out[0] ? h264_out : "",
                          hevc_out && hevc_out[0] ? hevc_out : "",
                          av1_out && av1_out[0] ? av1_out : "",
                          vp8_out && vp8_out[0] ? vp8_out : "",
                          mpeg2_out && mpeg2_out[0] ? mpeg2_out : "",
                          jpeg_enc_out && jpeg_enc_out[0] ? jpeg_enc_out : "",
                          vpp_out && vpp_out[0] ? vpp_out : "");
    return 0;
}
