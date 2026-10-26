/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* MSM allocation adapted from Radxa libva-v4l2 (MIT); see NOTICE. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <xf86drm.h>
#include <msm_drm.h>
#include "va_v4l2.h"
#ifndef MSM_BO_CACHED_COHERENT
#define MSM_BO_CACHED_COHERENT 0x080000
#endif
/* The fd belongs to the selected display DRM device, not a video node.
 * Leave non-MSM displays to GBM and the existing storage providers. */
int vav4l2_alloc_drm_storage(int fd, unsigned bytes, unsigned rows, int coherent)
{
    uint64_t size = (uint64_t)bytes * rows;
    if (fd < 0) { errno = ENOTSUP; return -1; }
    if (!size || size > UINT32_MAX) { errno = EINVAL; return -1; }
    drmVersionPtr version = drmGetVersion(fd);
    if (!version) return -1;
    int supported = version->name && version->name_len == 3 && !memcmp(version->name, "msm", 3);
    drmFreeVersion(version);
    if (!supported) { errno = ENOTSUP; return -1; }
    struct drm_msm_gem_new allocation = {.size = size, .flags = MSM_BO_CACHED_COHERENT};
    if (drmIoctl(fd, DRM_IOCTL_MSM_GEM_NEW, &allocation) < 0) {
        if (errno != EINVAL && errno != EOPNOTSUPP) return -1;
        if (coherent) { errno = ENOTSUP; return -1; }
        allocation.flags = MSM_BO_WC;
        if (drmIoctl(fd, DRM_IOCTL_MSM_GEM_NEW, &allocation) < 0) return -1;
    }
    int prime = -1;
    int result = drmPrimeHandleToFD(fd, allocation.handle, DRM_CLOEXEC | DRM_RDWR, &prime), saved = errno;
    struct drm_gem_close close_handle = {.handle = allocation.handle};
    if (drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &close_handle) < 0) {
        saved = errno; if (prime >= 0) close(prime); errno = saved; return -1;
    }
    if (result < 0) { errno = saved; return -1; }
    uint32_t actual;
    if (vav4l2_export_fd_validate(prime, size, &actual) < 0) { saved = errno; close(prime); errno = saved; return -1; }
    return prime;
}
