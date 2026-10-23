/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef VAREQ_TEST_DRIVER_IDENTITY_H
#define VAREQ_TEST_DRIVER_IDENTITY_H
#include <string.h>
static inline int vareq_test_driver_identity(const char *vendor)
{
    return vendor && !strncmp(vendor, "va-request ", sizeof("va-request ") - 1);
}
#endif
