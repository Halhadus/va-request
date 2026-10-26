/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Ondrej Jirman <megi@xff.cz> */
#ifndef VAV4L2_REQUEST_SLICES_H
#define VAV4L2_REQUEST_SLICES_H
#include <stdbool.h>
#include "va_v4l2.h"
struct h264_slice_info {
    uint32_t nal_ref_idc;
    uint32_t nal_unit_type;
    bool idr;
    uint32_t idr_pic_id;
    uint32_t slice_type;
    uint32_t pic_parameter_set_id;
    uint32_t pic_order_cnt_lsb;
    int32_t delta_pic_order_cnt_bottom;
    int32_t delta_pic_order_cnt0;
    int32_t delta_pic_order_cnt1;
    uint32_t pic_order_cnt_bit_size;
    uint32_t dec_ref_pic_marking_bit_size;
    uint32_t header_bit_size;
    bool num_ref_idx_override;
    uint32_t redundant_pic_cnt;
    bool valid;
    /* Extension and data-partition NALs need another submission contract. */
    bool unsupported;
    uint32_t unsupported_nal_unit_type;
};

struct hevc_slice_info {
    uint32_t nal_unit_type;
    uint32_t temporal_id_plus1;
    bool no_output_of_prior_pics;
    uint32_t short_term_ref_pic_set_size;
    uint32_t long_term_ref_pic_set_size;
    uint32_t num_delta_pocs_of_ref_rps_idx;
    uint32_t num_entry_point_offsets;
    uint32_t data_byte_offset;
    bool valid;
};

struct vav4l2_request_slice {
    size_t offset, size;
    struct v4l2_ctrl_h264_slice_params h264;
    struct v4l2_ctrl_h264_pred_weights weights;
    struct h264_slice_info h264_info;
    struct v4l2_ctrl_hevc_slice_params hevc;
    struct hevc_slice_info hevc_info;
    uint32_t *entries;
    unsigned num_entries;
};
VAStatus vav4l2_h264_request_slice(struct vav4l2_context *, const VAPictureParameterBufferH264 *,
    const VASliceParameterBufferH264 *, const struct v4l2_ctrl_h264_decode_params *, const uint8_t *, size_t, struct vav4l2_request_slice *);
VAStatus vav4l2_hevc_request_slice(struct vav4l2_context *, const VAPictureParameterBufferHEVC *,
    const VASliceParameterBufferHEVC *, const struct v4l2_ctrl_hevc_decode_params *, const uint8_t *, size_t, unsigned, struct vav4l2_request_slice *);

struct vav4l2_slice_plan;
VAStatus vav4l2_request_slice_plan(struct vav4l2_context *, struct vav4l2_buffer **, int,
    const uint8_t *, size_t, const void *, const void *, struct vav4l2_slice_plan **);
VAStatus vav4l2_request_slice_submit(struct vav4l2_context *, struct vav4l2_slice_plan *,
    struct v4l2_ext_control *, unsigned);
VAStatus vav4l2_request_slice_decode(struct vav4l2_context *, struct vav4l2_slice_plan *,
    void *, void *, void *, void *, int, unsigned, unsigned, uint32_t);
void vav4l2_request_slice_free(struct vav4l2_slice_plan *);
#endif
