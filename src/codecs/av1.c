/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "va_request.h"
const int vareq_has_av1 = 0;
VAStatus vareq_av1_fill_tile_info(struct v4l2_av1_tile_info *info,
                                  const VADecPictureParameterBufferAV1 *pic) { (void)info; (void)pic; return VA_STATUS_ERROR_UNIMPLEMENTED; }
VAStatus vareq_av1_prepare_tiles(const VADecPictureParameterBufferAV1 *pic,
                                  const VASliceParameterBufferAV1 * const *tiles,
                                  unsigned n, const uint8_t *data, uint32_t size,
                                  uint32_t capacity, struct v4l2_av1_tile_info *info,
                                  struct v4l2_ctrl_av1_tile_group_entry *entries,
                                  uint32_t *start, uint32_t *length) { (void)pic; (void)tiles; (void)n; (void)data; (void)size; (void)capacity; (void)info; (void)entries; (void)start; (void)length; return VA_STATUS_ERROR_UNIMPLEMENTED; }
void vareq_av1_dpb_model_reset(struct vareq_context *ctx) { (void)ctx; }
VAStatus vareq_av1_translate(struct vareq_context *ctx,
                              struct vareq_buffer **buffers,
                              int num_buffers) { (void)ctx; (void)buffers; (void)num_buffers; return VA_STATUS_ERROR_UNIMPLEMENTED; }
