/*
 * v4l2request — VA-API to V4L2 Request API bridge
 * Internal data structures and function declarations
 */

#ifndef VA_REQUEST_H
#define VA_REQUEST_H

#include <stdint.h>
#include <pthread.h>
#include <poll.h>
#include <va/va.h>
#include <va/va_backend.h>
#include <va/va_dec_vp8.h>
#include <va/va_dec_av1.h>
#include <va/va_vpp.h>
#include <va/va_enc_jpeg.h>
#include <va/va_drmcommon.h>
#include <linux/v4l2-controls.h>
#include <linux/videodev2.h>
#include "compat/compat.h"

/* Packed 10-bit formats are stable UAPI; older distro headers omit them. */
#ifndef V4L2_PIX_FMT_NV15
#define V4L2_PIX_FMT_NV15 v4l2_fourcc('N', 'V', '1', '5')
#endif
#ifndef V4L2_PIX_FMT_NV20
#define V4L2_PIX_FMT_NV20 v4l2_fourcc('N', 'V', '2', '0')
#endif

struct vareq_prime_import;
struct vareq_h264_field;
struct gbm_bo;   /* opaque — only gbm.c includes <gbm.h> */

/* Codec identifiers */
enum vareq_codec {
    VAREQ_CODEC_H264,
    VAREQ_CODEC_HEVC,
    VAREQ_CODEC_AV1,
    VAREQ_CODEC_VP8,
    VAREQ_CODEC_MPEG2,
    VAREQ_CODEC_JPEG_ENC,
    VAREQ_CODEC_VPP,
};

/* Per-config state */
struct vareq_config {
    VAConfigID config_id;
    VAProfile profile;
    VAEntrypoint entrypoint;
    enum vareq_codec codec;
    unsigned int rt_format;
    const char *device_path;
    struct vareq_config *next;
};

/* Single source of truth for which backing holds the freshest pixels:
 * CPU = cpu_ptr (VPP dst, vaPutImage), MEMFD = memfd snapshot,
 * BO = gbm bo (lazy path: memfd is stale until ensure_memfd refills it). */
enum vareq_last_writer {
    VAREQ_WRITER_NONE = 0,
    VAREQ_WRITER_CPU = 1,
    VAREQ_WRITER_MEMFD = 2,
    VAREQ_WRITER_BO = 3,
};
#define vareq_memfd_stale(s_) ((s_)->last_writer == VAREQ_WRITER_BO)

extern int vareq_debug;

/* Per-surface state */
struct vareq_surface {
    struct vareq_driver_data *driver_data;
    VASurfaceID surface_id;
    unsigned int width;
    unsigned int height;
    unsigned int format;     /* VA fourcc (NV12/P010/YUY2/ARGB) */
    unsigned int rt_format;
    VASurfaceStatus status;
    int has_pic;             /* holds a decoded picture snapshot (bo/memfd) */
    int buf_index;           /* V4L2 capture buffer index, -1 if not allocated */
    int memfd_fd;          /* memfd snapshot fd (CPU-readback vehicle) */
    uint32_t memfd_size;     /* current ftruncate size of memfd_fd (grow-only) */
    void *memfd_map;         /* persistent RW mapping of memfd_fd (grow-only) */
    uint32_t memfd_map_size; /* mapped length of memfd_map */
    void *memfd_retired;     /* superseded mapping still held by derived images */
    uint32_t memfd_retired_size;
    uint32_t memfd_borrows;  /* live derived images borrowing surface backing */
    uint64_t timestamp;      /* V4L2 timestamp for reference tracking */
    uint16_t h264_frame_num;
    uint8_t h264_frame_num_valid;
    uint32_t order_hint;     /* AV1 OrderHint of the frame last decoded here */
    uint8_t av1_level1;      /* AV1 KEY / level-1 ARF (hidden skip=0,0) */
    uint32_t stride;         /* negotiated capture geometry, set at decode */
    uint32_t aligned_h;
    uint32_t cap_fourcc;     /* V4L2 capture fourcc of attached decode buf */
    void *cap_view;          /* mmap of capture_buf_ptr[buf_index]; EXPBUF GetImage */
    struct vareq_context *capture_ctx; /* non-owning capture-slot owner */
    void *cpu_ptr;           /* software backing for upload / encode / VPP src */
    uint32_t cpu_size;
    uint32_t cpu_stride;
    struct gbm_bo *gbm_bo;   /* driver-owned display copy (linear, R8) */
    uint32_t gbm_stride;
    int gbm_dirty;
    uint8_t last_writer;     /* vareq_last_writer: who wrote pixels last.
                              * memfd staleness is derived: writer == BO. */
    int gbm_imported;        /* external linear NV12 BO; refresh at each VA read */
    int import_fd;           /* owned CLOEXEC duplicate for DMA-BUF CPU sync */
    struct vareq_prime_import *prime;
    int prime_capture_valid; /* CPU readback must not discard the native DPB reference */
    int gbm_writable, gbm_external_fd;
};

/* Parameter buffer */
struct vareq_buffer {
    VABufferID buffer_id;
    VABufferType type;
    unsigned int size;
    unsigned int num_elements;
    unsigned int allocated_elements;
    VAStatus completion_status; /* last synchronous coded-buffer operation */
    void *data;
    int mmapped;             /* data from mmap (derive_image) vs malloc */
    struct vareq_surface *borrow_surf; /* mmapped==2: surface whose mapping
                                         * is borrowed (NULL = cpu_ptr borrow) */
    uint32_t fourcc;         /* VAImageBufferType: fourcc of the image */
    uint32_t pitch;          /* VAImageBufferType: allocation stride */
    uint32_t image_width, image_height; /* VAImage visible geometry */
    uint32_t image_alloc_height; /* chroma origin; zero means image_height */
    struct vareq_buffer *next;
};

/* Number of output/capture buffer slots */
#define VAREQ_NUM_OUTPUT_BUFS  4
/* Hard ceiling on capture buffer indexes — sizes every per-context
 * array. Keep >= the largest requested count below. */
#define VAREQ_MAX_CAPTURE_BUFS 40
#define VAREQ_NUM_CAPTURE_BUFS 24
/* AV1 uses a deeper pool at ordinary resolutions. Large native capture
 * allocations need a smaller initial pool to limit contiguous-memory use. */
#define VAREQ_NUM_CAPTURE_BUFS_AV1 40
#define VAREQ_NUM_CAPTURE_BUFS_AV1_4K 12

/* Slice-data buffers a single picture may carry (MPEG-2 hits one per MB
 * row; sized to match pending_buffers). */
#define VAREQ_MAX_SLICE_DATAS 256

/* Collected decode-buffer roles from one vaRenderPicture batch
 * (vareq_collect_decode_buffers). */
struct vareq_collected {
    void *pic;              /* VAPictureParameterBufferType (last one wins) */
    void *iq;               /* VAIQMatrixBufferType (last one wins) */
    void *prob;             /* VAProbabilityBufferType (VP8) */
    void *slice_params[32]; /* VASliceParameterBufferType, in order */
    int n_slice_params;
    int n_slice_params_seen; /* unclamped count (overflow vs the 32-slot table) */
    const uint8_t *slice_datas[VAREQ_MAX_SLICE_DATAS];
    uint32_t slice_sizes[VAREQ_MAX_SLICE_DATAS];
    int n_slice_datas;
    const uint8_t *largest;     /* biggest SliceData buffer (VP8/AV1 style) */
    uint32_t largest_size;
};


/* Surface table size; IDs are recycled via a free stack so a long-lived
 * process can never index past the table. */
#define VAREQ_MAX_SURFACES 4096

/* Persistent M2M queue state (VPP / JPEG encode): when the negotiated
 * setup is unchanged across pictures, S_FMT / S_CTRL / REQBUFS / QUERYBUF
 * and the plane mappings are skipped — steady state is STREAMON + QBUF +
 * DQBUF + STREAMOFF only (~18 -> ~7 ioctls, 4 mmap -> 0 per frame). A
 * setup change or any steady-state failure triggers a full teardown and
 * renegotiation on the next picture. */
struct vareq_m2m_state {
    uint8_t  valid;
    uint8_t  streaming;       /* OUTPUT bit 0, CAPTURE bit 1 */
    uint8_t  out_memory, cap_memory; /* zero preserves legacy MMAP setup */
    uint8_t  selection_active; /* VPP crop/compose must be reset on full-frame reuse */
    uint32_t key[8];         /* codec-specific setup key */
    void    *out_map[3];     /* mapped OUTPUT planes (JPEG: Y/UV or Y/U/V) */
    size_t   out_len[3];
    int      out_planes;     /* negotiated OUTPUT plane count */
    void    *cap_map;        /* mapped CAPTURE plane */
    size_t   cap_len;
    struct v4l2_format ofmt; /* negotiated OUTPUT format (plane layout) */
    struct v4l2_format cfmt; /* negotiated CAPTURE format (VPP plane layout) */
};

/* Per-context state (one decode session) */
struct vareq_context {
    struct vareq_compat_backend compat_backend;
    VAContextID context_id;
    VAConfigID config_id;
    enum vareq_codec codec;
    VAProfile profile;
    VAEntrypoint entrypoint;
    unsigned int rt_format;
    char *device_path;       /* owned strdup of the config's device path */
    struct vareq_driver_data *driver_data;  /* back-pointer for surface lookup */
    int width;
    int height;

    int v4l2_fd;                 /* V4L2 video device fd */
    int media_fd;                /* V4L2 media device fd (for request alloc) */
    int request_fd;              /* Current V4L2 request */
    unsigned vpp_controls;
    unsigned vpp_behaviors;

    VASurfaceID *render_targets;
    int num_render_targets;

    struct vareq_surface *current_surface;
    VASurfaceID current_surface_id;

    /* Collected buffers for current picture. MPEG-2 can be one slice
     * per MB row plus param/IQ buffers, so 32 is not enough. */
    struct vareq_buffer *pending_buffers[256];
    int num_pending_buffers;

    struct vareq_buffer *buffers;  /* Attached parameter buffers */
    struct vareq_context *next;

    /* Output (bitstream) buffer management */
    int output_bufs_allocd;
    uint32_t output_buf_size;
    uint32_t output_plane_len[VAREQ_NUM_OUTPUT_BUFS]; /* QUERYBUF memo */
    void  *output_buf_ptr[VAREQ_NUM_OUTPUT_BUFS];
    int free_out_bufs[VAREQ_NUM_OUTPUT_BUFS];
    int n_free_out;

    /* Capture (decoded frame) buffer management */
    unsigned capture_memory; /* zero is native MMAP; DMABUF may be client or pool owned */
    int capture_internal; /* independent pool;surface snapshots never retain CPU DMA mappings */
    struct { int active, fd; uint32_t length; VASurfaceID owner; } capture_dma[VAREQ_MAX_CAPTURE_BUFS];
    int capture_bufs_allocd;
    int streamed;         /* STREAMON done (HEVC defers it past SPS) */
    void *capture_buf_ptr[VAREQ_MAX_CAPTURE_BUFS];
    uint32_t capture_buf_size;
    int capture_buf_anon; /* calloc stub when ioctl hook is installed */
    int free_cap_bufs[VAREQ_MAX_CAPTURE_BUFS];
    int n_free_cap;
    uint32_t cap_width;        /* driver-chosen capture geometry */
    uint32_t cap_height;       /* aligned height (e.g. 1088 for 1080p) */
    uint32_t cap_stride;       /* bytes per row, may exceed width */
    uint32_t cap_sizeimage;    /* per-buffer plane size */
    uint32_t cap_pixelformat;  /* V4L2 capture fourcc (NV12/NV15/NV16/NV20) */

    /* Frame counter for V4L2 timestamps (DPB reference matching) */
    uint64_t frame_count;
    struct vareq_h264_field *h264_field; /* owned first PAFF field;no queued kernel work */

    /* Hot-path dedup: shadow copy of the last submitted GLOBAL
     * (sequence-level) control payload. Codec translates memcmp against
     * it and skip the global ioctl when unchanged. Contexts are
     * single-codec, so one slot suffices. All translates run under
     * g_vareq_lock. */
    uint8_t g_ctrl_valid;
    /* 1088: this UAPI's padded v4l2_ctrl_h264_sps is 1048 bytes */
    uint8_t g_ctrl_payload[1088];

    /* AV1 refresh_frame_flags inference heuristics: VA does not expose
     * the bitmask. style: 0 = unknown, 1 = libaom-style (free slots from
     * index 1), 2 = SVT-AV1 RA pyramid. */
    struct {
        uint8_t style;
        uint8_t gop;           /* first hidden ARF order_hint (mini-GOP) */
        uint8_t l0_toggle;     /* SVT L0 slots 0-1-2 */
        uint8_t l1_toggle;     /* SVT L1 slots 3-4 */
        uint8_t have_first_arf;
        uint32_t key_oh;       /* order_hint of the current GOP's KEY */
        uint32_t l0_oh;        /* last SVT L0 ARF order_hint */
        uint32_t prev_l0_oh;   /* previous L0 ARF (mini-GOP length) */
        /* Kernel-DPB model for the AV1 copy-out release: which capture
         * buffer each reference slot holds, and which surface owns a
         * given buffer (to clear buf_index when it is released). */
        int model_active;      /* slot model owns releases (parsed truth) */
        int slot_buf[V4L2_AV1_TOTAL_REFS_PER_FRAME];
        VASurfaceID buf_owner[VAREQ_MAX_CAPTURE_BUFS];
    } av1;

    /* Persistent M2M queues for encoding and raw processing */
    struct vareq_m2m_state vpp_q;
    struct vareq_m2m_state jpeg_q;
};

/* Driver global state */
#define VAREQ_VPP_MAX_DEVICES 16
#define VAREQ_MAX_FORMATS 64
#define VAREQ_VPP_ROTATE 1u
#define VAREQ_VPP_HFLIP 2u
#define VAREQ_VPP_VFLIP 4u
struct vareq_vpp_device {
    char path[64];
    struct vareq_compat_identity identity;
    uint32_t input[VAREQ_MAX_FORMATS], output[VAREQ_MAX_FORMATS];
    unsigned num_input, num_output, controls;
    uint32_t min_width[2], min_height[2], max_width[2], max_height[2];
};

struct vareq_gbm_state {
    struct gbm_device *device;
    int fd, drm_fd, failed;
};
struct vareq_driver_data {
    struct vareq_gbm_state gbm;
    struct vareq_config *configs;
    VAConfigID next_config_id;

    /* Simple surface table */
    struct vareq_surface *surfaces[VAREQ_MAX_SURFACES];
    VASurfaceID next_surface_id;
    VASurfaceID free_surface_ids[VAREQ_MAX_SURFACES];
    int n_free_surface_ids;

    struct vareq_context *contexts;
    VAContextID next_context_id;

    VABufferID next_buffer_id;
    struct vareq_buffer *orphan_buffers;

    /* Resolved at init by OUTPUT_MPLANE fourcc (empty if missing). */
    char dev_h264[64];
    char dev_hevc[64];
    char dev_av1[64];
    char dev_vp8[64];
    char dev_mpeg2[64];
    char dev_jpeg_enc[64];
    char dev_vpp[64];
    uint32_t h264_profiles;
    int h264_profiles_known;
    uint32_t hevc_profiles;
    int hevc_profiles_known;
    struct vareq_vpp_device vpp_devices[VAREQ_VPP_MAX_DEVICES];
    unsigned num_vpp_devices;
    int vpp_devices_scanned;
    uint32_t vpp_input_formats[16], vpp_output_formats[16];
    VAProcPipelineCaps vpp_caps;
    int vpp_caps_ready;
};

/* Bounds-checked surface lookup. Every surfaces[id] access must go through
 * this — the table is fixed-size and IDs come from the client. */
static inline struct vareq_surface *
vareq_surface_by_id(struct vareq_driver_data *dd, VASurfaceID id)
{
    if (!dd || id == VA_INVALID_ID || (unsigned)id >= VAREQ_MAX_SURFACES)
        return NULL;
    return dd->surfaces[id];
}

static inline uint64_t vareq_surface_ts(struct vareq_driver_data *dd, VASurfaceID id)
{
    struct vareq_surface *s = vareq_surface_by_id(dd, id);

    return s ? s->timestamp : 0;
}

/* Bounded, duplicate-free pool pushes. A leaked index is better than a
 * write past the end of free_*_bufs[], which used to corrupt the context. */
static inline void vareq_out_pool_push(struct vareq_context *ctx, int idx)
{
    int i;

    if (!ctx || idx < 0 || idx >= VAREQ_NUM_OUTPUT_BUFS)
        return;
    if (ctx->n_free_out >= VAREQ_NUM_OUTPUT_BUFS)
        return;
    for (i = 0; i < ctx->n_free_out; i++)
        if (ctx->free_out_bufs[i] == idx)
            return;
    ctx->free_out_bufs[ctx->n_free_out++] = idx;
}

static inline void vareq_cap_pool_push(struct vareq_context *ctx, int idx)
{
    int i;

    if (!ctx || idx < 0 || idx >= VAREQ_MAX_CAPTURE_BUFS)
        return;
    if (ctx->n_free_cap >= VAREQ_MAX_CAPTURE_BUFS)
        return;
    for (i = 0; i < ctx->n_free_cap; i++)
        if (ctx->free_cap_bufs[i] == idx)
            return;
    ctx->free_cap_bufs[ctx->n_free_cap++] = idx;
}

/* Bounded, duplicate-free surface-ID recycle push. Same contract as the
 * pool pushes: a leaked ID beats a write past the table. */
static inline void vareq_surface_id_push(struct vareq_driver_data *dd,
                                          VASurfaceID id)
{
    int i;

    if (!dd || id == VA_INVALID_ID || (unsigned)id >= VAREQ_MAX_SURFACES)
        return;
    if (dd->n_free_surface_ids >= VAREQ_MAX_SURFACES)
        return;
    for (i = 0; i < dd->n_free_surface_ids; i++)
        if (dd->free_surface_ids[i] == id)
            return;
    dd->free_surface_ids[dd->n_free_surface_ids++] = id;
}

static inline int vareq_surface_capture_matches(const struct vareq_context *ctx,
                                                  const struct vareq_surface *s)
{
    if (!ctx || !s || s->buf_index < 0 || s->buf_index >= ctx->capture_bufs_allocd ||
        s->buf_index >= VAREQ_MAX_CAPTURE_BUFS)
        return 0;
    if (s->capture_ctx)
        return s->capture_ctx == ctx;
    if (s->cap_view)
        return s->cap_view == ctx->capture_buf_ptr[s->buf_index];
    for (int i = 0; i < ctx->num_render_targets; i++)
        if (ctx->render_targets[i] == s->surface_id)
            return 1;
    return 0;
}

/* A hardware reference must retain decoded CAPTURE storage in this context. */
VAStatus vareq_validate_reference(const struct vareq_context *ctx, VASurfaceID id);
int vareq_h264_field_pending(const struct vareq_context *ctx, VASurfaceID surface);
void vareq_h264_discard_field(struct vareq_context *ctx, int failed);

/* Optional decoder evidence;does not redirect stderr or change errno. */
void vareq_decode_trace(const struct vareq_context *ctx, const char *stage, int status, uint64_t argument);

/* Device helpers (device.c) */
int vareq_open_device(const char *path);
int vareq_codec_backend_init(struct vareq_context *ctx);
void vareq_close_context_device(struct vareq_context *ctx);
int vareq_codec_format(struct vareq_context *ctx, struct v4l2_format *format);
int vareq_codec_control(struct vareq_context *ctx, struct v4l2_control *control);
int vareq_codec_prepare_controls(struct vareq_context *ctx, int request_fd, struct v4l2_ext_controls *controls);
int vareq_codec_controls(struct vareq_context *ctx, int request_fd, struct v4l2_ext_controls *controls);
int vareq_setup_codec_output(struct vareq_context *ctx, uint32_t codec_format, int width, int height);
int vareq_setup_codec_capture(struct vareq_context *ctx, int width, int height, uint32_t pixelformat, int count);
int vareq_open_media_for_device(const char *video_path);
int vareq_request_alloc(int media_fd);
int vareq_setup_output_queue(int fd, uint32_t codec_format, int width, int height);
int vareq_capture_pool_allowed(const struct vareq_context *ctx);
int vareq_capture_pool_setup(struct vareq_context *ctx, unsigned width, unsigned height, uint32_t fourcc, unsigned count);
int vareq_capture_pool_bind(struct vareq_context *ctx, int index);
int vareq_capture_pool_complete(struct vareq_context *ctx, struct vareq_surface *s, int index);
int vareq_surface_store_capture(struct vareq_context *ctx, struct vareq_surface *s, int index, void *src, uint32_t size, int live_view);
int vareq_capture_dma_setup(struct vareq_context *ctx, unsigned width, unsigned height, uint32_t fourcc, unsigned count);
int vareq_capture_dma_bind(struct vareq_context *ctx, int index);
int vareq_capture_dma_queue(struct vareq_context *ctx, int index);
int vareq_capture_dma_dequeue(struct vareq_context *ctx, int *error);
int vareq_capture_dma_complete(struct vareq_context *ctx, struct vareq_surface *s, int index);
void vareq_capture_dma_release(struct vareq_context *ctx);
int vareq_setup_capture_queue(int fd, int width, int height, uint32_t pixelformat);
int vareq_setup_capture_queue_count(int fd, int width, int height,
                                     uint32_t pixelformat, int count);
int vareq_streamon(int fd, enum v4l2_buf_type type);
int vareq_streamoff(int fd, enum v4l2_buf_type type);
void vareq_release_context_device(struct vareq_context *ctx);
int vareq_get_capture_geometry(int fd, uint32_t *w, uint32_t *h,
                                uint32_t *stride, uint32_t *sizeimage);
int vareq_ensure_capture(struct vareq_context *ctx, int width, int height,
                          uint32_t pixelformat);
int vareq_queue_output(struct vareq_context *ctx, int buf_index,
                        uint32_t size, uint64_t timestamp);
int vareq_queue_capture(int fd, int buf_index, int request_fd);
/*
 * Submit one synchronous decode: pops a capture buffer, QBUFs the given
 * OUTPUT buffer (caller already memcpy'd the bitstream into it), queues the
 * request, waits for the decoded frame and recycles the bitstream buffer.
 * Returns the decoded capture buffer index, or -1 with both pools fully
 * restored (STREAMOFF reset) — the caller must NOT push back out_buf_idx
 * when this fails.
 */
int vareq_decode_submit(struct vareq_context *ctx, int out_buf_idx,
                         uint32_t bytesused, uint64_t timestamp);
int vareq_h264_field_supported(struct vareq_context *ctx);
/* A held first field completes OUTPUT/request only. The second field reuses
 * that CAPTURE index without QBUFing it again, then dequeues the full picture. */
int vareq_decode_submit_field(struct vareq_context *ctx, int out_buf_idx,
    uint32_t bytesused, uint64_t timestamp, int capture_index, int hold);
/* STREAMOFF both queues and rebuild the free pools from scratch. Used to
 * recover from decode timeouts so a wedged job is never left in the kernel
 * (rkvdec returns every queued buffer in ERROR state on STREAMOFF). */
void vareq_decode_reset(struct vareq_context *ctx);
int vareq_surface_alloc_export_fd(struct vareq_surface *s);
int vareq_surface_grow_memfd(struct vareq_surface *s, uint32_t size);
/* Persistent per-surface mapping of memfd_fd (RW shared, grow-only; the
 * memfd itself only ever grows). Callers must NOT munmap the result —
 * the surface owns it and releases it at destroy. NULL on failure. */
void *vareq_surface_map_memfd(struct vareq_surface *s, uint32_t need);
/* Lazy CPU backing: decode-only surfaces never pay the multi-MB calloc;
 * the first CPU writer (put_image, VPP dst, upload) allocates it. */
int vareq_surface_ensure_cpu(struct vareq_surface *s);
int vareq_surface_pull_capture(struct vareq_context *ctx,
                                struct vareq_surface *surf, int buf_index);
int vareq_bind_capture_export(struct vareq_context *ctx);
int vareq_export_fd_validate(int fd, uint64_t required, uint32_t *size);
/* GBM-backed NV12 copy surfaces (gbm.c). Default decoder
 * export uses native EXPBUF; GBM covers CPU-written pixels and the
 * VAREQ_EXPBUF_EXPORT=0 copy path. */
int vareq_alloc_shared_storage(unsigned bytes, unsigned rows);
int vareq_alloc_coherent_storage(unsigned bytes, unsigned rows);
void vareq_allocator_cleanup(void);
int vareq_gbm_surface_ensure(struct vareq_surface *s);
void vareq_gbm_driver_cleanup(struct vareq_driver_data *dd);
struct vareq_prime_layout {
    uint32_t drm_format, planes, row[3], rows[3], plane_format[3];
};
int vareq_prime_layout(uint32_t fourcc, unsigned width, unsigned height, struct vareq_prime_layout *layout);
int vareq_dmabuf_wait(int fd, int write);
void vareq_prime_device_written(struct vareq_surface *s, int success);
int vareq_dmabuf_sync(int fd, uint64_t flags);
VAStatus vareq_prime_import(struct vareq_surface *s, const VADRMPRIMESurfaceDescriptor *desc);
void vareq_prime_destroy(struct vareq_surface *s);
int vareq_prime_read(struct vareq_surface *s);
int vareq_prime_write(struct vareq_surface *s);
VAStatus vareq_prime_export_backing(struct vareq_surface *s, uint32_t flags, void *descriptor, int overwrite);
VAStatus vareq_prime_export(struct vareq_surface *s, uint32_t flags, void *descriptor);
int vareq_prime_capture(struct vareq_surface *s, const void *src, uint32_t stride, uint32_t alh, uint32_t fcc, uint32_t size);
int vareq_gbm_read_cpu(struct vareq_surface *s);
int vareq_gbm_enable_write(struct vareq_surface *s);
VAStatus vareq_gbm_surface_import(struct vareq_surface *s, const VADRMPRIMESurfaceDescriptor *desc);
int vareq_import_refresh(struct vareq_surface *s);
void vareq_gbm_cleanup(void);
void vareq_format_cleanup(void);
int vareq_gbm_surface_sync(struct vareq_surface *s);
/* Lazy memfd: refill the memfd snapshot from the bo when a CPU-readback
 * caller (vaGetImage / vaDeriveImage / VPP source) needs it after a
 * bo-only pull_capture. */
int vareq_surface_ensure_memfd(struct vareq_surface *s);
int vareq_gbm_surface_upload(struct vareq_surface *s, const void *src,
                              uint32_t src_stride, uint32_t src_alh);
void vareq_gbm_surface_destroy(struct vareq_surface *s);
VAStatus vareq_surface_fill_prime_gbm(const struct vareq_surface *surf,
                                       uint32_t flags, void *descriptor);
int vareq_capture_expbuf(struct vareq_context *ctx, int buf_index);
int vareq_claim_capture_for_export(struct vareq_context *ctx,
                                   struct vareq_surface *surf);
int vareq_expbuf_export_wanted(void);
void vareq_explog(const char *fmt, ...);
typedef int (*vareq_ioctl_fn)(int fd, unsigned long request, void *arg);
void vareq_set_ioctl_hook(vareq_ioctl_fn fn);
int vareq_dequeue_buffer(int fd, enum v4l2_buf_type type, int *flag_error);
int vareq_mmap_output_buffers(int fd, int count, void **ptrs, uint32_t *size_out);
int vareq_set_request_controls(int request_fd, int v4l2_fd, struct v4l2_ext_controls *ctrls);
int vareq_set_global_controls(int v4l2_fd, struct v4l2_ext_controls *ctrls);
int vareq_submit_request(int request_fd);
/* Full teardown of a persistent M2M queue (STREAMOFF + REQBUFS(0) both
 * directions + drop the plane mappings). Safe no-op on an unused queue;
 * call before the context's device fd goes away. */
int vareq_m2m_finish_output(struct vareq_context *ctx, const struct vareq_m2m_state *q);
void vareq_m2m_teardown(struct vareq_context *ctx, struct vareq_m2m_state *q);
/* One-pass decode-buffer collection shared by every codec translate. */
void vareq_collect_decode_buffers(struct vareq_buffer **buffers, int n,
                                   struct vareq_collected *cb);
/* Single-object VADRMPRIMESurfaceDescriptor layout shared by the memfd
 * and GBM-bo export paths: 2 planes at `pitch`, chroma starting at row
 * `chroma_row`; SEPARATE_LAYERS splits them into two single-plane
 * layers (luma_fmt/chroma_fmt) instead of one `combined_fmt` layer. */
void vareq_fill_prime_layers(VADRMPRIMESurfaceDescriptor *desc,
                              int fd, uint32_t object_size,
                              uint64_t modifier, uint32_t va_fourcc,
                              uint32_t width, uint32_t height,
                              uint32_t pitch, uint32_t chroma_row,
                              uint32_t combined_fmt, uint32_t luma_fmt,
                              uint32_t chroma_fmt, uint32_t flags);
/* poll() wrapper that retries EINTR (device.c) */
int vareq_poll_intr(struct pollfd *fds, nfds_t n, int timeout);
/* EINTR-retrying ioctl (device.c); VPP/JPEG route through it so the test
 * ioctl hook covers them too. */
int vareq_ioctl_once(int fd, unsigned long request, void *arg);
int vareq_xioctl(int fd, unsigned long request, void *arg);

/* H.264 translation (h264.c) */
void vareq_hevc_fill_decode_params(struct v4l2_ctrl_hevc_decode_params *dec,
                                    const VAPictureParameterBufferHEVC *pic,
                                    struct vareq_driver_data *dd);
void h264_fill_sps(struct v4l2_ctrl_h264_sps *sps, const VAPictureParameterBufferH264 *pic,
                    VAProfile profile);
void h264_fill_pps(struct v4l2_ctrl_h264_pps *pps, const VAPictureParameterBufferH264 *pic,
                    const VASliceParameterBufferH264 *slice);
void h264_fill_decode_params(struct v4l2_ctrl_h264_decode_params *dec,
                             const VAPictureParameterBufferH264 *pic,
                             struct vareq_driver_data *dd,
                             const VASliceParameterBufferH264 *slice);
void h264_fill_scaling_matrix(struct v4l2_ctrl_h264_scaling_matrix *sm,
                              const VAIQMatrixBufferH264 *iq);
VAStatus vareq_hevc_fill_pps(struct v4l2_ctrl_hevc_pps *pps, const VAPictureParameterBufferHEVC *pic);
VAStatus vareq_h264_parse_header(const uint8_t *data, size_t size,
                                  const VAPictureParameterBufferH264 *pic,
                                  const VASliceParameterBufferH264 *slice,
                                  struct v4l2_ctrl_h264_pps *pps,
                                  struct v4l2_ctrl_h264_decode_params *dec);
VAStatus vareq_h264_translate(struct vareq_context *ctx,
                               struct vareq_buffer **buffers,
                               int num_buffers);

/* HEVC translation (hevc.c) */
VAStatus vareq_hevc_translate(struct vareq_context *ctx,
                               struct vareq_buffer **buffers,
                               int num_buffers);

/* AV1 translation (av1.c) */
void vareq_av1_dpb_model_reset(struct vareq_context *ctx);
VAStatus vareq_av1_translate(struct vareq_context *ctx,
                              struct vareq_buffer **buffers,
                              int num_buffers);

/* VP8 translation (vp8.c) */
void vareq_vp8_fill_frame(struct v4l2_ctrl_vp8_frame *frame,
                           const VAPictureParameterBufferVP8 *pic,
                           const VASliceParameterBufferVP8 *slice,
                           const VAProbabilityDataBufferVP8 *prob,
                           const VAIQMatrixBufferVP8 *iq,
                           struct vareq_driver_data *dd);
VAStatus vareq_vp8_translate(struct vareq_context *ctx,
                              struct vareq_buffer **buffers,
                              int num_buffers);

/* MPEG-2 translation (mpeg2.c) */
void vareq_mpeg2_fill_sequence(struct v4l2_ctrl_mpeg2_sequence *seq,
                                const VAPictureParameterBufferMPEG2 *pic,
                                VAProfile profile);
void vareq_mpeg2_fill_picture(struct v4l2_ctrl_mpeg2_picture *vpic,
                               const VAPictureParameterBufferMPEG2 *pic,
                               struct vareq_driver_data *dd);
void vareq_mpeg2_fill_quant(struct v4l2_ctrl_mpeg2_quantisation *q,
                             const VAIQMatrixBufferMPEG2 *iq);
VAStatus vareq_mpeg2_translate(struct vareq_context *ctx,
                                struct vareq_buffer **buffers,
                                int num_buffers);

/* JPEG encode / raw VPP */
VAStatus vareq_jpeg_encode(struct vareq_context *ctx,
                            struct vareq_buffer **buffers, int num_buffers);
VAStatus vareq_vpp_run(struct vareq_context *ctx,
                        struct vareq_buffer **buffers, int num_buffers);
VAStatus vareq_vpp_query_filters(VAProcFilterType *filters, unsigned int *n);
VAStatus vareq_vpp_query_filter_caps(VAProcFilterType type, void *caps,
                                      unsigned int *n);
VAStatus vareq_vpp_query_pipeline_caps(VAProcPipelineCaps *caps);
VAStatus vareq_vpp_query_device_caps(struct vareq_driver_data *dd, VAProcPipelineCaps *caps);
int vareq_vpp_device_score_job(const struct vareq_vpp_device *device, const struct vareq_compat_job *job);
int vareq_vpp_device_score(const struct vareq_vpp_device *device, uint32_t src, uint32_t dst,
                          int rotate, int hflip, int vflip, int crop_offset, int compose);
void vareq_vpp_transform_controls(unsigned behaviors, int *rotate, int *hflip, int *vflip);

/* Pixel format helpers (format.c) */
uint32_t vareq_capture_fourcc_from_rt(unsigned int rt_format);
uint32_t vareq_capture_fourcc_from_sps(int bit_depth_minus8, int chroma_format_idc);
uint32_t vareq_va_fourcc_for_capture(uint32_t v4l2_fourcc);
uint32_t vareq_drm_fourcc_for_capture(uint32_t v4l2_fourcc);
uint32_t vareq_capture_plane_size(uint32_t fourcc, uint32_t stride, uint32_t aligned_h);
struct vareq_image_view {
    void *data;
    uint32_t fourcc, stride, width, height, alloc_height, size;
};
/* Equal-format copies and NV12/I420 conversion; no scaling or color conversion.
 * Rectangles must follow chroma alignment and fit both logical images. */
VAStatus vareq_copy_image_rect(const struct vareq_image_view *dst, unsigned dx, unsigned dy,
                               const struct vareq_image_view *src, unsigned sx, unsigned sy,
                               unsigned width, unsigned height);
uint32_t vareq_va_image_size(uint32_t va_fourcc, uint32_t stride, uint32_t height);
uint32_t vareq_default_image_stride(uint32_t va_fourcc, int width);
size_t vareq_annexb_concat(const uint8_t * const *datas, const uint32_t *sizes,
                            int n, int prefix_len, uint8_t *dst, size_t dst_cap);
VAStatus vareq_packetize_slices(enum vareq_codec codec, struct vareq_buffer **buffers,
                                int n, uint8_t *dst, size_t dst_cap, size_t *bytes);
void vareq_nv15_to_p010(uint8_t *dst, uint32_t dst_stride, uint32_t dst_height,
                         const uint8_t *src, uint32_t src_stride,
                         uint32_t src_aligned_h, int width, int height);
void vareq_copy_nv12(uint8_t *dst, uint32_t dst_stride, uint32_t dst_height,
                      const uint8_t *src, uint32_t src_stride,
                      uint32_t src_aligned_h, int width, int height);
void vareq_nv16_to_yuy2(uint8_t *dst, uint32_t dst_stride,
                         const uint8_t *src, uint32_t src_stride,
                         uint32_t src_aligned_h, int width, int height);
int vareq_nv20_to_yuy2(uint8_t *dst, uint32_t dst_stride,
                         const uint8_t *src, uint32_t src_stride,
                         uint32_t src_aligned_h, int width, int height);
int vareq_nv20_to_y210(uint8_t *dst, uint32_t dst_stride,
                         const uint8_t *src, uint32_t src_stride,
                         uint32_t src_aligned_h, int width, int height);

VAStatus vareq_av1_fill_tile_info(struct v4l2_av1_tile_info *info,
                                  const VADecPictureParameterBufferAV1 *pic);
VAStatus vareq_av1_prepare_tiles(const VADecPictureParameterBufferAV1 *pic,
                                  const VASliceParameterBufferAV1 * const *tiles,
                                  unsigned n, const uint8_t *data, uint32_t size,
                                  uint32_t capacity, struct v4l2_av1_tile_info *info,
                                  struct v4l2_ctrl_av1_tile_group_entry *entries,
                                  uint32_t *start, uint32_t *length);

int vareq_annexb_next(const uint8_t *data, size_t size, size_t *offset, const uint8_t **nal, size_t *length);
VAStatus vareq_h264_validate_headers(const uint8_t *data, size_t size, const VAPictureParameterBufferH264 *pic,
    struct vareq_buffer **buffers, int count, struct v4l2_ctrl_h264_pps *pps, struct v4l2_ctrl_h264_decode_params *dec);
VAStatus vareq_hevc_parse_headers(const uint8_t *data, size_t size, const VAPictureParameterBufferHEVC *pic,
    struct v4l2_ctrl_hevc_pps *pps, struct v4l2_ctrl_hevc_decode_params *dec);

#endif /* VA_REQUEST_H */
