// SPDX-License-Identifier: MIT
#include "v4l2_decoder.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

int main(void)
{
    struct venus_v4l2_decoder_config config = { 0 };
    struct venus_v4l2_decoder *decoder =
        (struct venus_v4l2_decoder *)(uintptr_t)1;
    struct venus_v4l2_error error;

    memset(&error, 0x5a, sizeof(error));
    assert(venus_v4l2_decoder_open(
               &config, &decoder, &error) == -EINVAL);
    assert(decoder == NULL);
    assert(error.code == 0);
    assert(error.operation[0] == '\0');

    assert(venus_v4l2_decoder_submit(
               NULL, NULL, 0, 0, NULL, NULL) == -EINVAL);
    assert(venus_v4l2_decoder_pump(
               NULL, 0, NULL, NULL, NULL) == -EINVAL);
    assert(venus_v4l2_decoder_stop(NULL) == -EINVAL);

    assert(strcmp(venus_v4l2_decoder_last_operation(NULL), "none") == 0);
    assert(venus_v4l2_decoder_output_count(NULL) == 0);
    assert(venus_v4l2_decoder_capture_count(NULL) == 0);
    assert(venus_v4l2_decoder_capture_width(NULL) == 0);
    assert(venus_v4l2_decoder_capture_height(NULL) == 0);
    assert(venus_v4l2_decoder_capture_size(NULL) == 0);
    assert(venus_v4l2_decoder_source_changes(NULL) == 0);

    venus_v4l2_decoder_close(NULL);
    return 0;
}
