/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "test_config.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include "va_v4l2.h"
#include "probe.h"
extern const struct vav4l2_compat_module vav4l2_test_compat, vav4l2_test_codec_compat;
extern int vav4l2_test_codec_fault, vav4l2_test_codec_setups, vav4l2_test_codec_finishes, vav4l2_test_codec_formats, vav4l2_test_codec_controls;
extern int vav4l2_test_storage_fault, vav4l2_test_rejected_fd, vav4l2_test_coherent;
static int codec_mode, kernel_error, finish_order;
static int second_setup(const struct vav4l2_compat_identity *, const struct vav4l2_compat_codec *, int, void **);
static void first_finish(const struct vav4l2_compat_identity *, const struct vav4l2_compat_codec *, int, void *);
static void second_finish(const struct vav4l2_compat_identity *, const struct vav4l2_compat_codec *, int, void *);
static int injected, fail_first, ioctl_count, allocations, exported_fd = -1, node_count = 1;
const struct vav4l2_compat_module *const *__real_vav4l2_compat_modules(size_t *count);
const struct vav4l2_compat_module *const *__wrap_vav4l2_compat_modules(size_t *count)
{
    static const struct vav4l2_compat_module *const fixture[] = {&vav4l2_test_compat, &vav4l2_test_codec_compat};
    if (!injected) return __real_vav4l2_compat_modules(count);
    if (injected == 2) {
        static struct vav4l2_compat_module first, second;
        static const struct vav4l2_compat_module *const pair[] = {&first, &second};
        first = second = vav4l2_test_codec_compat;
        first.codec_finish = first_finish; second.name = "fixture-codec-second";
        second.codec_setup = second_setup; second.codec_finish = second_finish;
        *count = 2; return pair;
    }
    *count = 2; return fixture;
}
int __real_open(const char *, int, ...);
int __wrap_open(const char *path, int flags, ...)
{
    assert(!strcmp(path, "/dev/video127") || !strcmp(path, "/dev/video2048"));
    assert(flags == (O_RDWR | O_NONBLOCK | O_CLOEXEC));
    return __real_open("/dev/null", O_RDWR | O_CLOEXEC);
}
#if VAV4L2_TEST_LFS64
int __wrap_open64(const char *path, int flags, ...) { return __wrap_open(path, flags); }
#endif
int __wrap_vav4l2_list_video_nodes(char (*paths)[64], unsigned capacity)
{
    assert(capacity >= 2);
    strcpy(paths[0], "/dev/video127"); strcpy(paths[1], "/dev/video2048"); return node_count;
}
static int ioctl_hook(int fd, unsigned long request, void *arg)
{
    (void)fd; ioctl_count++;
    if (request == VIDIOC_QUERYCAP) {
        struct v4l2_capability *cap = arg;
        strcpy((char *)cap->driver, "fixture-driver"); strcpy((char *)cap->card, codec_mode ? "fixture-codec" : "fixture-scaler");
        cap->capabilities = V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING;
    } else if (request == VIDIOC_S_FMT) {
        struct v4l2_format *f = arg;
        if (codec_mode) {
            assert(f->fmt.pix_mp.pixelformat == V4L2_PIX_FMT_H264_SLICE || f->fmt.pix_mp.pixelformat == V4L2_PIX_FMT_JPEG || f->fmt.pix_mp.pixelformat == V4L2_PIX_FMT_NV12);
            assert(f->fmt.pix_mp.plane_fmt[0].sizeimage >= 4096);
            if (kernel_error) { errno = EIO; return -1; }
            return 0;
        }
        assert(f->fmt.pix_mp.pixelformat == V4L2_PIX_FMT_GREY);
        if (++allocations == 1 && fail_first) { errno = EINVAL; return -1; }
        f->fmt.pix_mp.plane_fmt[0].bytesperline = f->fmt.pix_mp.width;
        f->fmt.pix_mp.plane_fmt[0].sizeimage = 4096;
    } else if (codec_mode && request == VIDIOC_S_CTRL) {
        struct v4l2_control *control = arg;
        assert(control->value >= 7);
        if (kernel_error) { errno = EIO; return -1; }
    } else if (codec_mode && request == VIDIOC_S_EXT_CTRLS) {
        struct v4l2_ext_controls *controls = arg;
        assert(controls->count == 1);
        if (controls->controls[0].size) assert(*(unsigned char *)controls->controls[0].ptr == 17);
        else assert(controls->controls[0].value == 17);
        if (kernel_error) { errno = EIO; return -1; }
    } else if (codec_mode && request == VIDIOC_STREAMOFF) {
        if (kernel_error) { errno = EIO; return -1; }
    } else if (request == VIDIOC_REQBUFS) {
        struct v4l2_requestbuffers *r = arg;
        if (!codec_mode) assert(r->memory == V4L2_MEMORY_MMAP && r->count == 1);
    } else if (request == VIDIOC_QUERYBUF) {
        struct v4l2_buffer *b = arg; b->m.planes[0].length = 4096;
    } else if (request == VIDIOC_EXPBUF) {
        struct v4l2_exportbuffer *e = arg;
        e->fd = memfd_create("compat-fixture", MFD_CLOEXEC); assert(e->fd >= 0 && !ftruncate(e->fd, 4096));
        exported_fd = e->fd;
    } else assert(!"compatibility storage must not submit a job");
    return 0;
}
static int matches_all(const struct vav4l2_compat_identity *id) { (void)id; return 1; }
static void registry_checks(void)
{
    struct vav4l2_compat_identity identity = {.driver = "rockchip-rga", .card = "rga2"};
    struct vav4l2_compat result;
    assert(!vav4l2_compat_resolve(&identity, &result) && result.count == 1);
    assert(result.behaviors == (VAV4L2_COMPAT_SWAP_MIRROR_AXES | VAV4L2_COMPAT_STREAM_BEFORE_QUEUE |
        VAV4L2_COMPAT_EXACT_IMPORT_SPAN | VAV4L2_COMPAT_KEEP_DMA_STREAMS));
    assert(!vav4l2_compat_resolve(&result.identity, &result) && result.count == 1);
    strcpy(identity.driver, "other-driver");
    assert(!vav4l2_compat_resolve(&identity, &result) && !result.count && !result.behaviors);
    assert(!vav4l2_compat_resolve_modules(&identity, NULL, 0, &result) && !result.count);
    struct vav4l2_compat_job job = {0}; assert(vav4l2_compat_vpp_score(&result, &job) == 10);
    struct vav4l2_vpp_device node = {.identity = {.driver = "rockchip-rga", .card = "rga2"},
        .input = {V4L2_PIX_FMT_NV12}, .output = {V4L2_PIX_FMT_NV12}, .num_input = 1, .num_output = 1};
    struct vav4l2_compat_job range = {.src = V4L2_PIX_FMT_NV12, .dst = V4L2_PIX_FMT_NV12, .src_quantization = V4L2_QUANTIZATION_FULL_RANGE};
    assert(vav4l2_vpp_device_score_job(&node, &range) < 0);
    strcpy(node.identity.card, "rga3"); assert(vav4l2_vpp_device_score_job(&node, &range) < 0);
    strcpy(node.identity.driver, "other-driver"); assert(vav4l2_vpp_device_score_job(&node, &range) == 10);
    struct v4l2_capability raw; memset(&raw, 'x', sizeof(raw));
    vav4l2_compat_identity(&raw, &identity);
    assert(!identity.driver[16] && !identity.card[32] && !identity.bus_info[32]);
    struct vav4l2_compat_module a = {.api_version = VAV4L2_COMPAT_API_VERSION, .name = "a", .match = matches_all,
        .behaviors = VAV4L2_COMPAT_SWAP_MIRROR_AXES};
    struct vav4l2_compat_module b = a; b.name = "b"; b.behaviors = VAV4L2_COMPAT_KEEP_DMA_STREAMS;
    const struct vav4l2_compat_module *list[] = {&a, &b};
    assert(!vav4l2_compat_resolve_modules(&identity, list, 2, &result) && result.count == 2 && result.behaviors == (a.behaviors | b.behaviors));
    b.api_version++;
    assert(vav4l2_compat_resolve_modules(&identity, list, 2, &result) < 0 && !result.count && !result.behaviors);
    b.match = vav4l2_test_compat.match;
    assert(!vav4l2_compat_resolve_modules(&identity, list, 2, &result) && result.count == 1 && result.behaviors == a.behaviors);
    b.match = matches_all;
    b.api_version = a.api_version; b.name = a.name;
    assert(vav4l2_compat_resolve_modules(&identity, list, 2, &result) < 0 && errno == EEXIST && !result.count);
    struct vav4l2_compat_module modules[VAV4L2_COMPAT_MAX_MATCHES + 1];
    const struct vav4l2_compat_module *many[VAV4L2_COMPAT_MAX_MATCHES + 1]; char names[VAV4L2_COMPAT_MAX_MATCHES + 1][8];
    for (unsigned i = 0; i <= VAV4L2_COMPAT_MAX_MATCHES; i++) {
        snprintf(names[i], sizeof(names[i]), "m%u", i); modules[i] = a; modules[i].name = names[i]; many[i] = &modules[i];
    }
    assert(vav4l2_compat_resolve_modules(&identity, many, VAV4L2_COMPAT_MAX_MATCHES + 1, &result) < 0 && errno == EOVERFLOW && !result.count);
}
static void extension_checks(void)
{
    injected = 1;
    struct vav4l2_vpp_device node = {.identity = {.driver = "fixture-driver", .card = "fixture-scaler"},
        .input = {V4L2_PIX_FMT_NV12}, .output = {V4L2_PIX_FMT_NV12}, .num_input = 1, .num_output = 1};
    assert(vav4l2_vpp_device_score(&node, V4L2_PIX_FMT_NV12, V4L2_PIX_FMT_NV12, 0, 0, 0, 0, 0) == 35);
    assert(vav4l2_vpp_device_score(&node, V4L2_PIX_FMT_NV12, V4L2_PIX_FMT_NV12, 0, 0, 0, 0, 1) < 0);
    assert(vav4l2_vpp_device_score(&node, V4L2_PIX_FMT_NV12, V4L2_PIX_FMT_NV12, 90, 0, 0, 0, 0) < 0);
    assert(vav4l2_vpp_device_score(&node, V4L2_PIX_FMT_NV12, V4L2_PIX_FMT_XBGR32, 0, 0, 0, 0, 0) < 0);
    struct vav4l2_compat compat; assert(!vav4l2_compat_resolve(&node.identity, &compat));
    assert(compat.behaviors == VAV4L2_COMPAT_SWAP_MIRROR_AXES);
    int rotate = 0, hflip = 1, vflip = 0;
    vav4l2_vpp_transform_controls(compat.behaviors, &rotate, &hflip, &vflip); assert(!hflip && vflip && !rotate);
    vav4l2_set_ioctl_hook(ioctl_hook); vav4l2_allocator_cleanup();
    int fd = vav4l2_alloc_shared_storage(64, 32);
    assert(fd >= 0 && fd == exported_fd && allocations == 1 && (fcntl(fd, F_GETFD) & FD_CLOEXEC));
    close(fd); fail_first = 1; allocations = 0; node_count = 2; vav4l2_allocator_cleanup();
    fd = vav4l2_alloc_shared_storage(64, 32);
    assert(fd >= 0 && allocations == 2); close(fd);
    node_count = 1; fail_first = 0; vav4l2_allocator_cleanup();
    for (vav4l2_test_storage_fault = 1; vav4l2_test_storage_fault <= 2; vav4l2_test_storage_fault++) {
        assert(vav4l2_alloc_shared_storage(64, 32) < 0 && fcntl(vav4l2_test_rejected_fd, F_GETFD) < 0);
    }
    vav4l2_test_storage_fault = 0;
    fd = vav4l2_alloc_coherent_storage(64, 32);
    assert(fd >= 0 && vav4l2_test_coherent == 1); close(fd);
    injected = 0; allocations = 0; ioctl_count = 0; vav4l2_allocator_cleanup();
    assert(vav4l2_alloc_shared_storage(64, 32) < 0 && !allocations && ioctl_count == node_count);
    vav4l2_allocator_cleanup(); vav4l2_set_ioctl_hook(NULL);
}
static int second_setup(const struct vav4l2_compat_identity *id, const struct vav4l2_compat_codec *codec, int fd, void **state)
{
    if (vav4l2_test_codec_compat.codec_setup(id, codec, fd, state) < 0) return -1;
    if (vav4l2_test_codec_fault == 6) { errno = EPIPE; return -1; }
    return 0;
}
static void first_finish(const struct vav4l2_compat_identity *id, const struct vav4l2_compat_codec *codec, int fd, void *state)
{
    finish_order = finish_order * 10 + 1; vav4l2_test_codec_compat.codec_finish(id, codec, fd, state);
}
static void second_finish(const struct vav4l2_compat_identity *id, const struct vav4l2_compat_codec *codec, int fd, void *state)
{
    finish_order = finish_order * 10 + 2; vav4l2_test_codec_compat.codec_finish(id, codec, fd, state);
}
static void codec_checks(void)
{
    injected = 1; codec_mode = 1; vav4l2_set_ioctl_hook(ioctl_hook);
    uint32_t formats[] = {V4L2_PIX_FMT_H264_SLICE, V4L2_PIX_FMT_VP8_FRAME};
    struct vav4l2_node_fmts nodes[] = {
        {.path = "/dev/video127", .fourccs = formats, .n_fourccs = 2, .request_api = VAV4L2_REQAPI_YES,
         .identity = {.driver = "fixture-driver", .card = "fixture-codec"}},
        {.path = "/dev/video2048", .fourccs = formats, .n_fourccs = 2, .request_api = VAV4L2_REQAPI_YES},
    };
    assert(!strcmp(vav4l2_pick_device_for_codec(VAV4L2_CODEC_H264, nodes, 2), nodes[0].path));
    assert(!strcmp(vav4l2_pick_device_for_codec(VAV4L2_CODEC_VP8, nodes, 2), nodes[1].path));
    nodes[0].request_api = VAV4L2_REQAPI_NO;
    assert(!strcmp(vav4l2_pick_device_for_codec(VAV4L2_CODEC_H264, nodes, 2), nodes[1].path));
    nodes[0].request_api = VAV4L2_REQAPI_YES;
    struct vav4l2_compat_codec profile = {.direction = VAV4L2_COMPAT_DECODE, .coded_format = V4L2_PIX_FMT_H264_SLICE,
        .va_profile = VAProfileH264High10, .profile_known = 1};
    assert(vav4l2_compat_codec_score(&nodes[0].identity, &profile) < 0);
    profile.profile_known = 0; assert(vav4l2_compat_codec_score(&nodes[0].identity, &profile) == 40);
    for (unsigned encoder = 0; encoder < 2; encoder++) {
        struct vav4l2_context ctx = {.codec = encoder ? VAV4L2_CODEC_JPEG_ENC : VAV4L2_CODEC_H264, .profile = encoder ? VAProfileJPEGBaseline : VAProfileH264High};
        ctx.v4l2_fd = __real_open("/dev/null", O_RDWR | O_CLOEXEC); assert(ctx.v4l2_fd >= 0);
        assert(!vav4l2_codec_backend_init(&ctx) && ctx.compat_backend.initialized);
        int count = vav4l2_test_codec_formats;
        if (!encoder) assert(vav4l2_setup_codec_output(&ctx, V4L2_PIX_FMT_H264_SLICE, 64, 64) == VAV4L2_NUM_OUTPUT_BUFS);
        else {
            struct v4l2_format f = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE};
            f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_JPEG; f.fmt.pix_mp.width = f.fmt.pix_mp.height = 64; f.fmt.pix_mp.num_planes = 1;
            assert(!vav4l2_codec_format(&ctx, &f));
        }
        assert(vav4l2_test_codec_formats == count + 1);
        struct v4l2_ext_control item = {.id = V4L2_CID_JPEG_COMPRESSION_QUALITY, .value = 10};
        struct v4l2_ext_controls controls = {.count = 1, .controls = &item};
        assert(!vav4l2_codec_controls(&ctx, encoder ? -1 : 71, &controls) && item.value == 17);
        assert(controls.request_fd == (encoder ? 0 : 71));
        unsigned char payload[4] = {0};
        struct v4l2_ext_control compound = {.id = V4L2_CID_STATELESS_H264_SPS, .size = sizeof(payload), .ptr = payload};
        struct v4l2_ext_controls batch = {.count = 1, .controls = &compound};
        assert(!vav4l2_codec_controls(&ctx, 71, &batch) && payload[0] == 17);
        for (vav4l2_test_codec_fault = 3; vav4l2_test_codec_fault <= 5; vav4l2_test_codec_fault++) {
            item.value = 10; unsigned before = ioctl_count;
            assert(vav4l2_codec_controls(&ctx, 71, &controls) < 0 && ioctl_count == (int)before);
            assert(controls.request_fd == 71 && item.id == V4L2_CID_JPEG_COMPRESSION_QUALITY);
        }
        vav4l2_test_codec_fault = 0; kernel_error = 1; item.value = 10;
        assert(vav4l2_codec_controls(&ctx, 71, &controls) < 0 && errno == EIO);
        struct v4l2_format f = {.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE};
        f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264_SLICE; f.fmt.pix_mp.width = f.fmt.pix_mp.height = 64;
        assert(vav4l2_codec_format(&ctx, &f) < 0 && errno == EIO); kernel_error = 0;
        vav4l2_test_codec_fault = 2; unsigned before = ioctl_count;
        assert(vav4l2_codec_format(&ctx, &f) < 0 && ioctl_count == (int)before && f.fmt.pix_mp.pixelformat == V4L2_PIX_FMT_H264_SLICE);
        vav4l2_test_codec_fault = 0; int fd = ctx.v4l2_fd;
        if (encoder) {
            ctx.jpeg_q.valid = 1; ctx.jpeg_q.streaming = 3; kernel_error = 1;
            vav4l2_m2m_teardown(&ctx, &ctx.jpeg_q); kernel_error = 0;
        } else vav4l2_close_context_device(&ctx);
        assert(ctx.v4l2_fd == -1 && fcntl(fd, F_GETFD) < 0);
        vav4l2_close_context_device(&ctx);
    }
    assert(vav4l2_test_codec_setups == 2 && vav4l2_test_codec_finishes == 2);
    struct vav4l2_context failed = {.codec = VAV4L2_CODEC_H264};
    failed.v4l2_fd = __real_open("/dev/null", O_RDWR | O_CLOEXEC); assert(failed.v4l2_fd >= 0);
    vav4l2_test_codec_fault = 1;
    assert(vav4l2_codec_backend_init(&failed) < 0 && errno == EIO && !failed.compat_backend.initialized);
    assert(vav4l2_test_codec_setups == 3 && vav4l2_test_codec_finishes == 3 && fcntl(failed.v4l2_fd, F_GETFD) >= 0);
    vav4l2_close_context_device(&failed); vav4l2_test_codec_fault = 0;
    struct vav4l2_compat_codec codec = {.direction = VAV4L2_COMPAT_ENCODE, .coded_format = V4L2_PIX_FMT_JPEG};
    struct vav4l2_compat_backend generic = {0}; struct vav4l2_compat_identity other = {.driver = "other"};
    assert(!vav4l2_compat_backend_init(&generic, &other, &codec, 8) && !generic.policy.count);
    vav4l2_compat_backend_finish(&generic, 8);
    assert(vav4l2_test_codec_setups == 3 && vav4l2_test_codec_finishes == 3);
    struct vav4l2_compat_module incomplete = vav4l2_test_codec_compat;
    incomplete.codec_finish = NULL;
    const struct vav4l2_compat_module *bad[] = {&incomplete}; struct vav4l2_compat rejected;
    assert(vav4l2_compat_resolve_modules(&nodes[0].identity, bad, 1, &rejected) < 0 && !rejected.count);
    injected = 2;
    for (unsigned failure = 0; failure < 2; failure++) {
        struct vav4l2_context ctx = {.codec = VAV4L2_CODEC_H264};
        ctx.v4l2_fd = __real_open("/dev/null", O_RDWR | O_CLOEXEC); assert(ctx.v4l2_fd >= 0);
        vav4l2_test_codec_fault = failure ? 6 : 0; finish_order = 0;
        int result = vav4l2_codec_backend_init(&ctx);
        if (failure) assert(result < 0 && errno == EPIPE && !ctx.compat_backend.initialized && finish_order == 21);
        else assert(!result && ctx.compat_backend.policy.count == 2 && ctx.compat_backend.state[0] != ctx.compat_backend.state[1]);
        vav4l2_close_context_device(&ctx); assert(finish_order == 21);
    }
    assert(vav4l2_test_codec_setups == vav4l2_test_codec_finishes);
    vav4l2_test_codec_fault = 0; codec_mode = 0; injected = 0; vav4l2_set_ioctl_hook(NULL);
}
int main(void)
{
    registry_checks(); extension_checks(); codec_checks();
    puts("compatibility: isolated policies, extension codec/VPP/storage, safe provider fallback and generic rejection passed");
    return 0;
}
