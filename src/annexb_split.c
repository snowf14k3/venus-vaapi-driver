// SPDX-License-Identifier: MIT
#include "annexb_split.h"

#include <errno.h>
#include <stdbool.h>

static bool start_code_at(const uint8_t *data, size_t size, size_t offset,
                          size_t *start_code_size)
{
    if (offset + 3 <= size && data[offset] == 0 &&
        data[offset + 1] == 0 && data[offset + 2] == 1) {
        *start_code_size = 3;
        return true;
    }

    if (offset + 4 <= size && data[offset] == 0 &&
        data[offset + 1] == 0 && data[offset + 2] == 0 &&
        data[offset + 3] == 1) {
        *start_code_size = 4;
        return true;
    }

    return false;
}

int venus_annexb_for_each_access_unit(const uint8_t *data, size_t size,
                                      venus_access_unit_callback callback,
                                      void *opaque, size_t *num_units)
{
    size_t offset;
    size_t unit_start = 0;
    size_t units = 0;
    bool saw_start_code = false;
    bool saw_aud = false;

    if (!data || size == 0 || !callback)
        return -EINVAL;

    for (offset = 0; offset + 3 <= size; offset++) {
        size_t start_code_size;
        uint8_t nal_type;
        int result;

        if (!start_code_at(data, size, offset, &start_code_size))
            continue;
        if (offset + start_code_size >= size)
            return -EINVAL;

        saw_start_code = true;
        nal_type = data[offset + start_code_size] & 0x1f;
        if (nal_type != 9)
            continue;

        if (saw_aud) {
            if (offset <= unit_start)
                return -EINVAL;
            result = callback(data + unit_start, offset - unit_start,
                              opaque);
            if (result < 0)
                return result;
            units++;
            unit_start = offset;
        } else {
            saw_aud = true;
        }

        offset += start_code_size;
    }

    if (!saw_start_code)
        return -EINVAL;

    if (unit_start >= size)
        return -EINVAL;

    {
        int result = callback(data + unit_start, size - unit_start, opaque);

        if (result < 0)
            return result;
    }
    units++;

    if (num_units)
        *num_units = units;

    return 0;
}
