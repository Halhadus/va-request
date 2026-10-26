/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Ondrej Jirman <megi@xff.cz> */
#ifndef VAV4L2_VP9_REQUEST_H
#define VAV4L2_VP9_REQUEST_H
#include <stdbool.h>
#include "va_v4l2.h"
struct vav4l2_vp9_state {
    /*
     * Loop-filter deltas and segmentation feature data persist across VP9
     * frames (the bitstream only re-sends them when updated, and they are
     * reset to defaults on key/intra-only/error-resilient frames). VA-API
     * does not expose the accumulated values, so the driver tracks them
     * here, mirroring the software decoder's past-independence handling.
     */
    int8_t lf_ref_deltas[4];
    int8_t lf_mode_deltas[2];
    bool seg_abs_delta;
    bool seg_feature_enabled[8][4];
    int16_t seg_feature_data[8][4];
    bool color_range_full;
};

VAStatus vav4l2_vp9_request_parse(struct vav4l2_context *, const VADecPictureParameterBufferVP9 *,
    const uint8_t *, size_t, struct vav4l2_vp9_state *, struct v4l2_ctrl_vp9_frame *, struct v4l2_ctrl_vp9_compressed_hdr *);
#endif
