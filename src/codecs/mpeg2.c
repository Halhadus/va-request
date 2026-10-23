/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "va_request.h"
const int vareq_has_mpeg2 = 0;
void vareq_mpeg2_fill_sequence(struct v4l2_ctrl_mpeg2_sequence *seq,
                                const VAPictureParameterBufferMPEG2 *pic,
                                VAProfile profile) { (void)seq; (void)pic; (void)profile; }
void vareq_mpeg2_fill_picture(struct v4l2_ctrl_mpeg2_picture *vpic,
                               const VAPictureParameterBufferMPEG2 *pic,
                               struct vareq_driver_data *dd) { (void)vpic; (void)pic; (void)dd; }
void vareq_mpeg2_fill_quant(struct v4l2_ctrl_mpeg2_quantisation *q,
                             const VAIQMatrixBufferMPEG2 *iq) { (void)q; (void)iq; }
VAStatus vareq_mpeg2_translate(struct vareq_context *ctx,
                                struct vareq_buffer **buffers,
                                int num_buffers) { (void)ctx; (void)buffers; (void)num_buffers; return VA_STATUS_ERROR_UNIMPLEMENTED; }
