/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "va_request.h"
const int vareq_has_vp8 = 0;
void vareq_vp8_fill_frame(struct v4l2_ctrl_vp8_frame *frame,
                           const VAPictureParameterBufferVP8 *pic,
                           const VASliceParameterBufferVP8 *slice,
                           const VAProbabilityDataBufferVP8 *prob,
                           const VAIQMatrixBufferVP8 *iq,
                           struct vareq_driver_data *dd) { (void)frame; (void)pic; (void)slice; (void)prob; (void)iq; (void)dd; }
VAStatus vareq_vp8_translate(struct vareq_context *ctx,
                              struct vareq_buffer **buffers,
                              int num_buffers) { (void)ctx; (void)buffers; (void)num_buffers; return VA_STATUS_ERROR_UNIMPLEMENTED; }
