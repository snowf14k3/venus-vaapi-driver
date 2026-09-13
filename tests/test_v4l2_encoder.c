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
