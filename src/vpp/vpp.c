/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "va_request.h"
const int vareq_has_vpp = 0;
int vareq_vpp_device_score_job(const struct vareq_vpp_device *d, const struct vareq_compat_job *request) { (void)d; (void)request; return 0; }
int vareq_vpp_device_score(const struct vareq_vpp_device *d, uint32_t src, uint32_t dst,
                          int rotate, int hflip, int vflip, int crop_offset, int compose) { (void)d; (void)src; (void)dst; (void)rotate; (void)hflip; (void)vflip; (void)crop_offset; (void)compose; return 0; }
void vareq_vpp_transform_controls(unsigned behaviors, int *rotate, int *hflip, int *vflip) { (void)behaviors; (void)rotate; (void)hflip; (void)vflip; }
VAStatus vareq_vpp_query_filters(VAProcFilterType *filters, unsigned int *n) { (void)filters; (void)n; return VA_STATUS_ERROR_UNIMPLEMENTED; }
VAStatus vareq_vpp_query_filter_caps(VAProcFilterType type, void *caps,
                                      unsigned int *n) { (void)type; (void)caps; (void)n; return VA_STATUS_ERROR_UNIMPLEMENTED; }
VAStatus vareq_vpp_query_pipeline_caps(VAProcPipelineCaps *caps) { (void)caps; return VA_STATUS_ERROR_UNIMPLEMENTED; }
VAStatus vareq_vpp_query_device_caps(struct vareq_driver_data *dd, VAProcPipelineCaps *caps) { (void)dd; (void)caps; return VA_STATUS_ERROR_UNIMPLEMENTED; }
VAStatus vareq_vpp_run(struct vareq_context *ctx, struct vareq_buffer **buffers, int num_buffers) { (void)ctx; (void)buffers; (void)num_buffers; return VA_STATUS_ERROR_UNIMPLEMENTED; }
