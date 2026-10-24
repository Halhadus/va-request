/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef VAV4L2_TEST_DRIVER_IDENTITY_H
#define VAV4L2_TEST_DRIVER_IDENTITY_H
#include <string.h>
static inline int vav4l2_test_driver_identity(const char *vendor)
{
    return vendor && !strncmp(vendor, "libva-v4l2 ", sizeof("libva-v4l2 ") - 1);
}
#endif
