/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "va_request.h"
const int vareq_has_h264 = 0;
void h264_fill_sps(struct v4l2_ctrl_h264_sps *sps,
                   const VAPictureParameterBufferH264 *pic,
                   VAProfile profile) { (void)sps; (void)pic; (void)profile; }
void h264_fill_pps(struct v4l2_ctrl_h264_pps *pps,
                   const VAPictureParameterBufferH264 *pic,
                   const VASliceParameterBufferH264 *slice) { (void)pps; (void)pic; (void)slice; }
void h264_fill_decode_params(struct v4l2_ctrl_h264_decode_params *dec,
                             const VAPictureParameterBufferH264 *pic,
                             struct vareq_driver_data *dd,
                             const VASliceParameterBufferH264 *slice) { (void)dec; (void)pic; (void)dd; (void)slice; }
VAStatus vareq_h264_parse_header(const uint8_t *data, size_t size, const VAPictureParameterBufferH264 *pic,
    const VASliceParameterBufferH264 *slice, struct v4l2_ctrl_h264_pps *pps, struct v4l2_ctrl_h264_decode_params *dec) { (void)data; (void)size; (void)pic; (void)slice; (void)pps; (void)dec; return VA_STATUS_ERROR_UNIMPLEMENTED; }
VAStatus vareq_h264_validate_headers(const uint8_t *data, size_t size, const VAPictureParameterBufferH264 *pic,
    struct vareq_buffer **buffers, int count, struct v4l2_ctrl_h264_pps *pps, struct v4l2_ctrl_h264_decode_params *dec) { (void)data; (void)size; (void)pic; (void)buffers; (void)count; (void)pps; (void)dec; return VA_STATUS_ERROR_UNIMPLEMENTED; }
void h264_fill_scaling_matrix(struct v4l2_ctrl_h264_scaling_matrix *sm,
                              const VAIQMatrixBufferH264 *iq) { (void)sm; (void)iq; }
int vareq_h264_field_pending(const struct vareq_context *ctx, VASurfaceID surface) { (void)ctx; (void)surface; return 0; }
void vareq_h264_discard_field(struct vareq_context *ctx, int failed) { (void)ctx; (void)failed; }
VAStatus vareq_h264_translate(struct vareq_context *ctx,
                               struct vareq_buffer **buffers,
                               int num_buffers) { (void)ctx; (void)buffers; (void)num_buffers; return VA_STATUS_ERROR_UNIMPLEMENTED; }
