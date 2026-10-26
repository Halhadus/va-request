/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef VAV4L2_GPU_COPY_H
#define VAV4L2_GPU_COPY_H
#include "va_v4l2.h"
struct vav4l2_gpu_plane { int fd; unsigned offset, pitch, columns, rows; };
int vav4l2_gpu_copy_planes(const VADRMPRIMESurfaceDescriptor *, unsigned, unsigned, struct vav4l2_gpu_plane [2]);
int vav4l2_gpu_copy_capture(struct vav4l2_context *, struct vav4l2_surface *, int);
void vav4l2_gpu_copy_finish(struct vav4l2_driver_data *);
#endif
