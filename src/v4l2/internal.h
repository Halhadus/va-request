/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef VAV4L2_V4L2_INTERNAL_H
#define VAV4L2_V4L2_INTERNAL_H

#include "va_v4l2.h"

int vav4l2_release_queue(int fd, enum v4l2_buf_type type, unsigned memory);
void vav4l2_release_capture_surfaces(struct vav4l2_context *ctx);
void vav4l2_unmap_capture_buffers(struct vav4l2_context *ctx);

#endif
