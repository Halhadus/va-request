#ifndef VAV4L2_TEST_H264422_ANNEXB_H
#define VAV4L2_TEST_H264422_ANNEXB_H
#include <stddef.h>
#include <limits.h>

static inline unsigned h264422_prefix(const unsigned char *data, size_t size, size_t pos)
{
    if (size - pos < 3 || data[pos] || data[pos + 1]) return 0;
    if (data[pos + 2] == 1) return 3;
    return size - pos >= 4 && !data[pos + 2] && data[pos + 3] == 1 ? 4 : 0;
}

/* Preserve the last NAL through EOF and exclude either start-code length. */
static inline int h264422_slices(unsigned char *data, size_t size,
                                 unsigned char **slices, int *lengths, unsigned capacity)
{
    unsigned count = 0;
    for (size_t pos = 0; pos < size && count < capacity;) {
        unsigned prefix = h264422_prefix(data, size, pos);
        if (!prefix) { pos++; continue; }
        size_t payload = pos + prefix, end = payload;
        while (end < size && !h264422_prefix(data, size, end)) end++;
        if (payload < end && ((data[payload] & 31) == 1 || (data[payload] & 31) == 5)) {
            if (end - payload > INT_MAX) return -1;
            slices[count] = data + payload; lengths[count++] = (int)(end - payload);
        }
        pos = end;
    }
    return (int)count;
}
#endif
