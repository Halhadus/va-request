/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "va_request.h"
const int vareq_has_hevc = 0;
VAStatus vareq_hevc_fill_pps(struct v4l2_ctrl_hevc_pps *pps,
                             const VAPictureParameterBufferHEVC *pic) { (void)pps; (void)pic; return VA_STATUS_ERROR_UNIMPLEMENTED; }
void vareq_hevc_fill_decode_params(struct v4l2_ctrl_hevc_decode_params *dec,
                                    const VAPictureParameterBufferHEVC *pic,
                                    struct vareq_driver_data *dd) { (void)dec; (void)pic; (void)dd; }
VAStatus vareq_hevc_parse_headers(const uint8_t *data, size_t size, const VAPictureParameterBufferHEVC *pic,
    struct v4l2_ctrl_hevc_pps *pps, struct v4l2_ctrl_hevc_decode_params *dec) { (void)data; (void)size; (void)pic; (void)pps; (void)dec; return VA_STATUS_ERROR_UNIMPLEMENTED; }
VAStatus vareq_hevc_translate(struct vareq_context *ctx,
                               struct vareq_buffer **buffers,
                               int num_buffers) { (void)ctx; (void)buffers; (void)num_buffers; return VA_STATUS_ERROR_UNIMPLEMENTED; }
