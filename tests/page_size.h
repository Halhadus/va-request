/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef VAV4L2_TEST_PAGE_SIZE_H
#define VAV4L2_TEST_PAGE_SIZE_H
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
static inline unsigned vav4l2_test_page_size(void)
{
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0 || (unsigned long)page > UINT_MAX) { fputs("invalid system page size\n", stderr); exit(2); }
    return page;
}
static inline unsigned vav4l2_test_page_align(unsigned size)
{
    unsigned page = vav4l2_test_page_size(), padding = size % page ? page - size % page : 0;
    if (size > UINT_MAX - padding) { fputs("page-aligned fixture size overflow\n", stderr); exit(2); }
    return size + padding;
}
#endif
