// SPDX-License-Identifier: MIT
#ifndef VENUS_V4L2_DECODER_H
#define VENUS_V4L2_DECODER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "v4l2_common.h"

struct venus_v4l2_decoder;

struct venus_v4l2_decoder_config {
    const char *device;
    uint32_t coded_format;
    uint32_t width;
    uint32_t height;
    size_t output_buffer_size;
    unsigned int output_buffers;
    unsigned int capture_buffers;
};

struct venus_v4l2_frame {
    const uint8_t *data;
    size_t size;
    uint64_t tag;
    uint32_t flags;
    uint32_t width;
    uint32_t height;
    uint32_t bytes_per_line;
};

typedef int (*venus_v4l2_frame_callback)(
    const struct venus_v4l2_frame *frame, void *opaque);

int venus_v4l2_decoder_open(
    const struct venus_v4l2_decoder_config *config,
    struct venus_v4l2_decoder **decoder,
    struct venus_v4l2_error *error);
int venus_v4l2_decoder_submit(struct venus_v4l2_decoder *decoder,
                              const uint8_t *data, size_t size,
                              uint64_t tag,
                              venus_v4l2_frame_callback callback,
                              void *opaque);
int venus_v4l2_decoder_pump(struct venus_v4l2_decoder *decoder,
                            int timeout_ms,
                            venus_v4l2_frame_callback callback,
                            void *opaque, bool *end_of_stream);
int venus_v4l2_decoder_stop(struct venus_v4l2_decoder *decoder);
void venus_v4l2_decoder_close(struct venus_v4l2_decoder *decoder);

const char *venus_v4l2_decoder_last_operation(
    const struct venus_v4l2_decoder *decoder);
unsigned int venus_v4l2_decoder_output_count(
    const struct venus_v4l2_decoder *decoder);
unsigned int venus_v4l2_decoder_capture_count(
    const struct venus_v4l2_decoder *decoder);
uint32_t venus_v4l2_decoder_capture_width(
    const struct venus_v4l2_decoder *decoder);
uint32_t venus_v4l2_decoder_capture_height(
    const struct venus_v4l2_decoder *decoder);
uint32_t venus_v4l2_decoder_capture_size(
    const struct venus_v4l2_decoder *decoder);
unsigned int venus_v4l2_decoder_source_changes(
    const struct venus_v4l2_decoder *decoder);

#endif
