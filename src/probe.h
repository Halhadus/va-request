/*
 * Pick a V4L2 stateless decoder node by OUTPUT_MPLANE fourcc.
 * Matching is path-agnostic so /dev/videoN can move after reboot.
 */

#ifndef VA_V4L2_PROBE_H
#define VA_V4L2_PROBE_H

#include <stdint.h>
#include "va_v4l2.h"

#define VAV4L2_PROBE_MAX_FOURCCS VAV4L2_MAX_FORMATS
#define VAV4L2_PROBE_MAX_NODES   64

/* request_api: 0 = not probed (eligible), 1 = usable, 2 = skip. */
#define VAV4L2_REQAPI_UNKNOWN 0
#define VAV4L2_REQAPI_YES     1
#define VAV4L2_REQAPI_NO      2

struct vav4l2_node_fmts {
    const char *path;
    const uint32_t *fourccs;
    unsigned n_fourccs;
    int request_api;
    uint64_t h264_max_area;
    uint32_t h264_profiles;
    struct vav4l2_compat_identity identity;
};

int vav4l2_codec_coded_fourcc(enum vav4l2_codec codec, uint32_t *fourcc_out);
const char *vav4l2_codec_name(enum vav4l2_codec codec);
void vav4l2_fourcc_to_str(uint32_t fourcc, char out[5]);

/* Matching OUTPUT fourcc and a usable request API are mandatory. H.264
 * candidates use compatibility preference, then advertised extended profiles and frame area.
 * Returns NULL if none match. Does not open devices. */
const char *vav4l2_pick_device_for_codec(enum vav4l2_codec codec,
                                         const struct vav4l2_node_fmts *nodes,
                                         unsigned n_nodes);

/* Match media graph interface dev_t to the exact video node. */
int vav4l2_find_media_path(const char *video_path, char *out, unsigned out_len);
/* 1 if MEDIA_IOC_REQUEST_ALLOC works on the sibling media node. */
int vav4l2_video_has_request_api(const char *video_path);

int vav4l2_enum_output_fourccs(int fd, uint32_t *out, unsigned max);
int vav4l2_query_h264_profiles(const char *path, uint32_t *profiles);
int vav4l2_query_profiles(const char *path, uint32_t control, uint32_t *profiles);
int vav4l2_vpp_formats_usable(const uint32_t *input, unsigned ni, const uint32_t *output, unsigned no);
void vav4l2_scan_vpp_devices(struct vav4l2_driver_data *dd);

/* Fresh, bounded scan of all numeric /dev/video* names, irrespective of index. */
int vav4l2_list_video_nodes(char (*paths)[64], unsigned capacity);
int vav4l2_codec_enabled(enum vav4l2_codec codec);
int vav4l2_codec_controls_usable(int fd, enum vav4l2_codec codec);
int vav4l2_node_is_m2m(const struct v4l2_capability *cap);
int vav4l2_scan_decoder_paths_ex(char *h264, char *hevc, char *av1, char *vp8, char *mpeg2, unsigned len);
int vav4l2_enum_capture_fourccs(int fd, uint32_t *out, unsigned max);
/* Boot/topology-keyed cache; selected nodes are revalidated before reuse. */
int vav4l2_scan_all_cached(char *h264, char *hevc, char *av1, char *vp8, char *mpeg2, char *jpeg, char *vpp, unsigned len);
#endif
