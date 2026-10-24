/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/media.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include "probe.h"
#include "compat/compat.h"
static uint32_t missing;
static int disabled, fail_menu, calls;
static int media_owner, media_cursor, media_roles[1024];
static unsigned video_major, video_minor;
static int raw_inventory, jpeg_selected[1024], jpeg_reject, readonly;
static uint32_t jpeg_input;
int __wrap_ioctl(int fd, unsigned long request, ...)
{
    (void)fd; va_list ap; va_start(ap, request); void *arg = va_arg(ap, void *); va_end(ap); calls++;
    if (raw_inventory && request == VIDIOC_QUERYCAP) {
        struct v4l2_capability *cap = arg; cap->capabilities = V4L2_CAP_DEVICE_CAPS;
        cap->device_caps = V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING; strcpy((char *)cap->card, "generic-scaler"); return 0;
    }
    if (raw_inventory && request == VIDIOC_ENUM_FMT) {
        struct v4l2_fmtdesc *fmt = arg;
        if (raw_inventory == 2) {
            if (fmt->index) { errno = EINVAL; return -1; }
            fmt->pixelformat = fmt->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE ? V4L2_PIX_FMT_JPEG : jpeg_selected[fd] ? jpeg_input : V4L2_PIX_FMT_RGB565; return 0;
        }
        if (fmt->index >= VAV4L2_MAX_FORMATS) { errno = EINVAL; return -1; }
        fmt->pixelformat = !fmt->index ? V4L2_PIX_FMT_RGB565 : fmt->index == VAV4L2_MAX_FORMATS - 1 ? V4L2_PIX_FMT_RGBX32 : v4l2_fourcc('Z', 'Z', fmt->index, 'X'); return 0;
    }
    if (raw_inventory && request == VIDIOC_TRY_FMT) return 0;
    if (raw_inventory == 2 && request == VIDIOC_G_FMT) {
        struct v4l2_format *fmt = arg;
        fmt->fmt.pix_mp.width = fmt->fmt.pix_mp.height = 128;
        fmt->fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264; fmt->fmt.pix_mp.num_planes = 1; return 0;
    }
    if (raw_inventory == 2 && request == VIDIOC_S_FMT) {
        struct v4l2_format *fmt = arg;
        if (jpeg_reject) { errno = EINVAL; return -1; }
        assert(fmt->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE && fmt->fmt.pix_mp.pixelformat == V4L2_PIX_FMT_JPEG);
        jpeg_selected[fd] = 1; return 0;
    }
    if (request == MEDIA_IOC_G_TOPOLOGY) {
        struct media_v2_topology *top = arg;
        if (!top->ptr_interfaces) { top->num_interfaces = 1; return 0; }
        struct media_v2_interface *interface = (void *)(uintptr_t)top->ptr_interfaces;
        interface->intf_type = MEDIA_INTF_T_V4L_VIDEO; interface->devnode.major = video_major;
        interface->devnode.minor = video_minor + (media_roles[fd] == 9 ? 1 : 0); return 0;
    }
    if (request == VIDIOC_QUERY_EXT_CTRL) {
        struct v4l2_query_ext_ctrl *q = arg;
        if (q->id == missing) { errno = EINVAL; return -1; }
        q->flags = (disabled ? V4L2_CTRL_FLAG_DISABLED : 0) | (readonly ? V4L2_CTRL_FLAG_READ_ONLY : 0); return 0;
    }
    if (request == VIDIOC_QUERYCTRL) {
        struct v4l2_queryctrl *q = arg; q->minimum = 0; q->maximum = 4; q->type = V4L2_CTRL_TYPE_MENU; return 0;
    }
    if (request == VIDIOC_QUERYMENU) { if (fail_menu) { errno = EINVAL; return -1; } return 0; }
    errno = EINVAL; return -1;
}
int __wrap_scandir(const char *path, struct dirent ***out, int (*filter)(const struct dirent *), int (*compare)(const struct dirent **, const struct dirent **))
{
    assert(!strcmp(path, "/dev"));
    const char *names[] = {"video2048", "video127", "video2", "video2-extra", "video", "media0"};
    struct dirent **entries = calloc(6, sizeof(*entries)); assert(entries); int count = 0;
    for (unsigned i = 0; i < 6; i++) {
        struct dirent *entry = calloc(1, sizeof(*entry)); assert(entry); strcpy(entry->d_name, names[i]);
        if (filter(entry)) entries[count++] = entry; else free(entry);
    }
    for (int i = 0; i < count; i++) for (int j = i + 1; j < count; j++) if (compare((const struct dirent **)&entries[i], (const struct dirent **)&entries[j]) > 0) {
        struct dirent *entry = entries[i]; entries[i] = entries[j]; entries[j] = entry;
    }
    *out = entries; return count;
}
int __wrap_scandir64(const char *path, struct dirent64 ***out, int (*filter)(const struct dirent64 *), int (*compare)(const struct dirent64 **, const struct dirent64 **))
{
    assert(!strcmp(path, "/dev"));
    const char *names[] = {"video2048", "video127", "video2", "video2-extra", "video", "media0"};
    struct dirent64 **entries = calloc(6, sizeof(*entries)); assert(entries); int count = 0;
    for (unsigned i = 0; i < 6; i++) {
        struct dirent64 *entry = calloc(1, sizeof(*entry)); assert(entry); strcpy(entry->d_name, names[i]);
        if (filter(entry)) entries[count++] = entry; else free(entry);
    }
    for (int i = 0; i < count; i++) for (int j = i + 1; j < count; j++) if (compare((const struct dirent64 **)&entries[i], (const struct dirent64 **)&entries[j]) > 0) {
        struct dirent64 *entry = entries[i]; entries[i] = entries[j]; entries[j] = entry;
    }
    *out = entries; return count;
}

DIR *__wrap_opendir(const char *path) { assert(!strcmp(path, "/dev")); media_cursor = 0; return (DIR *)(uintptr_t)1; }
int __wrap_closedir(DIR *directory) { assert(directory == (DIR *)(uintptr_t)1); return 0; }
struct dirent *__wrap_readdir(DIR *directory)
{
    assert(directory == (DIR *)(uintptr_t)1); static struct dirent entry;
    if (media_cursor >= 2) return NULL;
    strcpy(entry.d_name, media_cursor++ ? "media4" : "media9"); return &entry;
}
struct dirent64 *__wrap_readdir64(DIR *directory)
{
    assert(directory == (DIR *)(uintptr_t)1); static struct dirent64 entry;
    if (media_cursor >= 2) return NULL;
    strcpy(entry.d_name, media_cursor++ ? "media4" : "media9"); return &entry;
}
int __wrap_open(const char *path, int flags, ...)
{
    assert(flags == (O_RDWR | O_CLOEXEC) || flags == (O_RDWR | O_NONBLOCK | O_CLOEXEC));
    int role = !strcmp(path, "/dev/media9") ? 9 : !strcmp(path, "/dev/media4") ? 4 : !strncmp(path, "/dev/video", 10) ? 42 : 0; assert(role);
    int fd = fcntl(media_owner, F_DUPFD_CLOEXEC, 0); assert(fd >= 0 && fd < 1024); media_roles[fd] = role; jpeg_selected[fd] = 0; return fd;
}
int __wrap_open64(const char *path, int flags, ...) { return __wrap_open(path, flags); }
int __real_open(const char *path, int flags, ...);
static void test_media_interface(void)
{
    struct stat node; assert(stat("/dev/null", &node) == 0);
    video_major = major(node.st_rdev); video_minor = minor(node.st_rdev);
    media_owner = __real_open("/dev/null", O_RDWR | O_CLOEXEC); assert(media_owner >= 0);
    char result[64]; assert(vav4l2_find_media_path("/dev/null", result, sizeof(result)) == 0 && !strcmp(result, "/dev/media4"));
    assert(vav4l2_find_media_path("/dev/null", result, 2) == -1 && !result[0]); close(media_owner);
}

static void test_full_format_inventory(void)
{
    media_owner = __real_open("/dev/null", O_RDWR | O_CLOEXEC); assert(media_owner >= 0); raw_inventory = 1;
    struct vav4l2_driver_data *driver = calloc(1, sizeof(*driver)); assert(driver);
    vav4l2_scan_vpp_devices(driver); assert(driver->num_vpp_devices == 3);
    for (unsigned i = 0; i < driver->num_vpp_devices; i++) {
        struct vav4l2_vpp_device *node = &driver->vpp_devices[i];
        assert(node->num_input == VAV4L2_MAX_FORMATS && node->num_output == VAV4L2_MAX_FORMATS);
        assert(node->input[VAV4L2_MAX_FORMATS - 1] == V4L2_PIX_FMT_RGBX32 && node->output[VAV4L2_MAX_FORMATS - 1] == V4L2_PIX_FMT_RGBX32);
        assert(vav4l2_vpp_device_score(node, V4L2_PIX_FMT_RGBX32, V4L2_PIX_FMT_RGB565, 0, 0, 0, 0, 0) >= 0);
    }
    free(driver); raw_inventory = 0; close(media_owner);
}

static void test_jpeg_discovery(void)
{
    media_owner = __real_open("/dev/null", O_RDWR | O_CLOEXEC); assert(media_owner >= 0);
    raw_inventory = 2; setenv("VAV4L2_PROBE_NOCACHE", "1", 1);
    const uint32_t formats[] = {V4L2_PIX_FMT_NV12, V4L2_PIX_FMT_NV12M, V4L2_PIX_FMT_YUV420M, V4L2_PIX_FMT_YUYV};
    char paths[7][64];
    for (unsigned i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
        jpeg_input = formats[i];
        assert(vav4l2_scan_all_cached(paths[0], paths[1], paths[2], paths[3], paths[4], paths[5], paths[6], 64) == 0);
        assert(i == 3 ? !paths[5][0] : !strcmp(paths[5], "/dev/video2"));
    }
    jpeg_input = V4L2_PIX_FMT_NV12;
    for (unsigned fault = 0; fault < 4; fault++) {
        missing = fault == 0 ? V4L2_CID_JPEG_COMPRESSION_QUALITY : 0;
        disabled = fault == 1; readonly = fault == 2; jpeg_reject = fault == 3;
        assert(vav4l2_scan_all_cached(paths[0], paths[1], paths[2], paths[3], paths[4], paths[5], paths[6], 64) == 0 && !paths[5][0]);
    }
    missing = disabled = readonly = jpeg_reject = raw_inventory = 0; close(media_owner);
}

int main(void)
{
    test_media_interface();
    char paths[4][64]; assert(vav4l2_list_video_nodes(paths, 4) == 3);
    assert(!strcmp(paths[0], "/dev/video2") && !strcmp(paths[1], "/dev/video127") && !strcmp(paths[2], "/dev/video2048"));
    assert(vav4l2_list_video_nodes(paths, 2) == -1 && errno == EOVERFLOW);
    struct v4l2_capability cap = {.capabilities = V4L2_CAP_DEVICE_CAPS | V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING};
    assert(!vav4l2_node_is_m2m(&cap)); cap.device_caps = V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING; assert(vav4l2_node_is_m2m(&cap));
    cap.device_caps = V4L2_CAP_VIDEO_M2M_MPLANE; assert(!vav4l2_node_is_m2m(&cap));
    assert(vav4l2_codec_controls_usable(42, VAV4L2_CODEC_H264));
    missing = V4L2_CID_STATELESS_H264_PPS; assert(!vav4l2_codec_controls_usable(42, VAV4L2_CODEC_H264)); missing = 0;
    disabled = 1; assert(!vav4l2_codec_controls_usable(42, VAV4L2_CODEC_HEVC)); disabled = 0;
    missing = V4L2_CID_STATELESS_HEVC_SLICE_PARAMS; assert(vav4l2_codec_controls_usable(42, VAV4L2_CODEC_HEVC));
    const uint32_t hevc_required[] = {V4L2_CID_STATELESS_HEVC_SPS, V4L2_CID_STATELESS_HEVC_PPS, V4L2_CID_STATELESS_HEVC_SCALING_MATRIX, V4L2_CID_STATELESS_HEVC_DECODE_PARAMS};
    for (unsigned i = 0; i < sizeof(hevc_required) / sizeof(hevc_required[0]); i++) { missing = hevc_required[i]; assert(!vav4l2_codec_controls_usable(42, VAV4L2_CODEC_HEVC)); }
    missing = 0;
    fail_menu = 1; assert(!vav4l2_codec_controls_usable(42, VAV4L2_CODEC_H264)); fail_menu = 0;
    calls = 0; assert(!vav4l2_codec_controls_usable(-1, VAV4L2_CODEC_H264) && !calls);
    assert(!vav4l2_codec_controls_usable(42, (enum vav4l2_codec)999));
    uint32_t fmt = 0; assert(vav4l2_codec_coded_fourcc(VAV4L2_CODEC_HEVC, &fmt) == 0 && fmt == V4L2_PIX_FMT_HEVC_SLICE);
    assert(!vav4l2_capture_fourcc_from_rt(VA_RT_FORMAT_RGB32));
    assert(!vav4l2_capture_fourcc_from_sps(4, 1) && !vav4l2_capture_fourcc_from_sps(0, 3));
    assert(!vav4l2_va_fourcc_for_capture(v4l2_fourcc('N','E','W','!')) && !vav4l2_drm_fourcc_for_capture(0));
    assert(!vav4l2_capture_plane_size(0, 128, 128));
    test_full_format_inventory();
    test_jpeg_discovery();
    puts("discovery: capabilities, removed controls, high node indices and unknown formats passed"); return 0;
}
