// SPDX-License-Identifier: MIT
#include "annexb_split.h"

#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

struct split_result {
    const uint8_t *units[4];
    size_t sizes[4];
    size_t count;
};

static int collect(const uint8_t *data, size_t size, void *opaque)
{
    struct split_result *result = opaque;

    assert(result->count < 4);
    result->units[result->count] = data;
    result->sizes[result->count] = size;
    result->count++;
    return 0;
}

int main(void)
{
    static const uint8_t stream[] = {
        0, 0, 0, 1, 0x67, 0x01,
        0, 0, 1, 0x68, 0x02,
        0, 0, 0, 1, 0x09, 0x10,
        0, 0, 1, 0x65, 0x03,
        0, 0, 1, 0x09, 0x10,
        0, 0, 0, 1, 0x41, 0x04,
    };
    static const uint8_t no_aud[] = {
        0, 0, 0, 1, 0x67, 0x01,
        0, 0, 0, 1, 0x65, 0x02,
    };
    static const uint8_t invalid[] = { 0x67, 0x01 };
    struct split_result result = { 0 };
    size_t units = 0;

    assert(venus_annexb_for_each_access_unit(
               stream, sizeof(stream), collect, &result, &units) == 0);
    assert(units == 2);
    assert(result.count == 2);
    assert(result.units[0] == stream);
    assert(result.sizes[0] == 22);
    assert(result.units[1] == stream + 22);
    assert(result.sizes[1] == sizeof(stream) - 22);

    result = (struct split_result) { 0 };
    assert(venus_annexb_for_each_access_unit(
               no_aud, sizeof(no_aud), collect, &result, &units) == 0);
    assert(units == 1);
    assert(result.count == 1);
    assert(result.sizes[0] == sizeof(no_aud));

    assert(venus_annexb_for_each_access_unit(
               invalid, sizeof(invalid), collect, &result, NULL) ==
           -EINVAL);

    puts("PASS: Annex-B AUD access-unit splitting");
    return 0;
}
