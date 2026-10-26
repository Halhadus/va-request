/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>
#include <msm_drm.h>
#include "va_v4l2.h"
static unsigned allocations, closes, mode, fail_export, fail_close;
static uint64_t span;
static drmVersion version;
drmVersionPtr __wrap_drmGetVersion(int fd) { assert(fd == 42); version.name = mode == 1 ? "other" : "msm"; version.name_len = strlen(version.name); return &version; }
void __wrap_drmFreeVersion(drmVersionPtr v) { assert(v == &version); }
int __wrap_drmIoctl(int fd, unsigned long request, void *arg)
{
    assert(fd == 42);
    if (request == DRM_IOCTL_GEM_CLOSE) { assert(((struct drm_gem_close *)arg)->handle == 7); closes++; if (fail_close) { errno = EIO; return -1; } return 0; }
    assert(request == DRM_IOCTL_MSM_GEM_NEW); struct drm_msm_gem_new *a = arg;
    allocations++; span = a->size;
    if (mode == 2 && allocations == 1) { errno = EINVAL; return -1; }
    if (mode == 3) { errno = ENOMEM; return -1; }
    if (mode == 4) { errno = EACCES; return -1; }
    a->handle = 7; return 0;
}
int __wrap_drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *out)
{
    assert(fd == 42 && handle == 7 && flags == (DRM_CLOEXEC | DRM_RDWR));
    if (fail_export) { errno = EMFILE; return -1; }
    *out = memfd_create("drm-storage", MFD_CLOEXEC); assert(*out >= 0 && !ftruncate(*out, span)); return 0;
}
int main(void)
{
    int fd = vav4l2_alloc_drm_storage(42, 64, 3, 0); assert(fd >= 0 && allocations == 1 && closes == 1); close(fd);
    allocations = closes = 0; mode = 1;
    assert(vav4l2_alloc_drm_storage(42, 64, 3, 0) < 0 && errno == ENOTSUP && !allocations && !closes);
    mode = 2; fd = vav4l2_alloc_drm_storage(42, 64, 3, 0); assert(fd >= 0 && allocations == 2 && closes == 1); close(fd);
    allocations = closes = 0;
    assert(vav4l2_alloc_drm_storage(42, 64, 3, 1) < 0 && errno == ENOTSUP && allocations == 1 && !closes);
    for (mode = 3; mode <= 4; mode++) {
        allocations = closes = 0;
        assert(vav4l2_alloc_drm_storage(42, 64, 3, 0) < 0 && allocations == 1 && !closes && errno == (mode == 3 ? ENOMEM : EACCES));
    }
    allocations = closes = mode = 0; fail_export = 1;
    assert(vav4l2_alloc_drm_storage(42, 64, 3, 0) < 0 && errno == EMFILE && closes == 1);
    fail_export = 0; fail_close = 1;
    assert(vav4l2_alloc_drm_storage(42, 64, 3, 0) < 0 && errno == EIO && closes == 2);
    return 0;
}
