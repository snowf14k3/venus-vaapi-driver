// SPDX-License-Identifier: MIT
#ifndef VENUS_V4L2_ENCODER_H
#define VENUS_V4L2_ENCODER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "v4l2_common.h"

struct venus_v4l2_encoder;

struct venus_v4l2_encoder_config {
    const char *device;
    uint32_t coded_format;
    uint32_t width;
    uint32_t height;
    uint32_t frames_per_second;
    uint32_t bitrate;
    bool rate_control_enabled;
    uint32_t bitrate_mode;
    uint32_t h264_i_qp;
    uint32_t h264_p_qp;
    uint32_t h264_min_qp;
    uint32_t h264_max_qp;
    uint32_t gop_size;
    uint32_t h264_profile;
    uint32_t h264_level;
    uint32_t h264_entropy_mode;
    uint32_t h264_transform_8x8;
    size_t capture_buffer_size;
    unsigned int output_buffers;
    unsigned int capture_buffers;
};

struct venus_v4l2_packet {
    const uint8_t *data;
    size_t size;
    uint64_t tag;
    uint32_t flags;
};

typedef int (*venus_v4l2_packet_callback)(
    const struct venus_v4l2_packet *packet, void *opaque);

int venus_v4l2_encoder_open(
    const struct venus_v4l2_encoder_config *config,
    struct venus_v4l2_encoder **encoder,
    struct venus_v4l2_error *error);
int venus_v4l2_encoder_submit(struct venus_v4l2_encoder *encoder,
                              const uint8_t *data, size_t size,
                              uint64_t tag,
                              venus_v4l2_packet_callback callback,
                              void *opaque);
int venus_v4l2_encoder_pack_nv12(
    uint8_t *destination, size_t destination_size,
    uint32_t destination_stride, uint32_t destination_scanlines,
    const uint8_t *source, uint32_t width, uint32_t height,
    size_t *packed_size);
int venus_v4l2_encoder_compressed_size(
    uint32_t width, uint32_t height, size_t *size);
int venus_v4l2_encoder_force_keyframe(
    struct venus_v4l2_encoder *encoder);
int venus_v4l2_encoder_pump(struct venus_v4l2_encoder *encoder,
                            int timeout_ms,
                            venus_v4l2_packet_callback callback,
                            void *opaque, bool *end_of_stream);
int venus_v4l2_encoder_stop(struct venus_v4l2_encoder *encoder);
void venus_v4l2_encoder_close(struct venus_v4l2_encoder *encoder);

const char *venus_v4l2_encoder_last_operation(
    const struct venus_v4l2_encoder *encoder);
unsigned int venus_v4l2_encoder_output_count(
    const struct venus_v4l2_encoder *encoder);
unsigned int venus_v4l2_encoder_capture_count(
    const struct venus_v4l2_encoder *encoder);
uint32_t venus_v4l2_encoder_output_size(
    const struct venus_v4l2_encoder *encoder);
uint32_t venus_v4l2_encoder_capture_size(
    const struct venus_v4l2_encoder *encoder);

#endif
