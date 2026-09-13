// SPDX-License-Identifier: MIT
#include "v4l2_encoder.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

int main(void)
{
    struct venus_v4l2_encoder_config config = { 0 };
    struct venus_v4l2_encoder *encoder =
        (struct venus_v4l2_encoder *)(uintptr_t)1;
    struct venus_v4l2_error error;
    uint8_t source[4 * 4 * 3 / 2];
    uint8_t destination[384];
    size_t packed_size;
    size_t index;

    memset(source, 0x5a, sizeof(source));
    memset(destination, 0xff, sizeof(destination));
    assert(venus_v4l2_encoder_pack_nv12(
               destination, sizeof(destination),
               8, 32, source, 4, 4,
               &packed_size) == 0);
    assert(packed_size == sizeof(destination));
    for (index = 0; index < 4; index++) {
        assert(memcmp(destination + index * 8,
                      source + index * 4, 4) == 0);
        assert(destination[index * 8 + 4] == 0);
    }
    assert(memcmp(destination + 8 * 32,
                  source + 4 * 4, 4) == 0);
    assert(memcmp(destination + 8 * 33,
                  source + 4 * 5, 4) == 0);

    memset(&error, 0x5a, sizeof(error));
    assert(venus_v4l2_encoder_open(
               &config, &encoder, &error) == -EINVAL);
    assert(encoder == NULL);
    assert(error.code == 0);
    assert(error.operation[0] == '\0');

    assert(venus_v4l2_encoder_submit(
               NULL, NULL, 0, 0, NULL, NULL) == -EINVAL);
    assert(venus_v4l2_encoder_pump(
               NULL, 0, NULL, NULL, NULL) == -EINVAL);
    assert(venus_v4l2_encoder_stop(NULL) == -EINVAL);
    assert(strcmp(venus_v4l2_encoder_last_operation(NULL), "none") == 0);
    assert(venus_v4l2_encoder_output_count(NULL) == 0);
    assert(venus_v4l2_encoder_capture_count(NULL) == 0);
    assert(venus_v4l2_encoder_output_size(NULL) == 0);
    assert(venus_v4l2_encoder_capture_size(NULL) == 0);

    venus_v4l2_encoder_close(NULL);
    return 0;
}
