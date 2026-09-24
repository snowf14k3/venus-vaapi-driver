// SPDX-License-Identifier: MIT
#include "v4l2_encoder.h"
#include "v4l2_probe.h"

#include <errno.h>
#include <limits.h>
#include <linux/videodev2.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct encode_run {
    FILE *output;
    size_t packets;
    size_t bytes;
};

static int write_packet(const struct venus_v4l2_packet *packet,
                        void *opaque)
{
    struct encode_run *run = opaque;

    if (fwrite(packet->data, 1, packet->size, run->output) != packet->size)
        return -EIO;

    run->packets++;
    run->bytes += packet->size;
    return 0;
}

static int parse_positive(const char *text, unsigned int *value)
{
    char *end = NULL;
    unsigned long parsed;

    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno || !end || *end != '\0' || parsed == 0 ||
        parsed > UINT_MAX)
        return -EINVAL;

    *value = (unsigned int)parsed;
    return 0;
}

static int64_t monotonic_milliseconds(void)
{
    struct timespec timestamp;

    if (clock_gettime(CLOCK_MONOTONIC, &timestamp) != 0)
        return -1;
    return (int64_t)timestamp.tv_sec * 1000 +
           timestamp.tv_nsec / 1000000;
}

int main(int argc, char **argv)
{
    struct venus_capabilities capabilities;
    struct venus_v4l2_encoder_config config;
    struct venus_v4l2_encoder *encoder = NULL;
    struct venus_v4l2_error open_error = { 0 };
    struct encode_run run = { 0 };
    const char *device;
    uint8_t *frame = NULL;
    FILE *input = NULL;
    size_t frame_size;
    unsigned int width;
    unsigned int height;
    unsigned int frames_per_second;
    unsigned int frame_count;
    unsigned int bitrate;
    unsigned int frame_index;
    bool eos = false;
    int64_t deadline;
    int status = 0;

    if (argc != 8 && argc != 9) {
        fprintf(stderr,
                "usage: %s WIDTH HEIGHT FPS FRAMES BITRATE INPUT OUTPUT [DEVICE]\n",
                argv[0]);
        return 2;
    }

    if (parse_positive(argv[1], &width) < 0 ||
        parse_positive(argv[2], &height) < 0 ||
        parse_positive(argv[3], &frames_per_second) < 0 ||
        parse_positive(argv[4], &frame_count) < 0 ||
        parse_positive(argv[5], &bitrate) < 0 ||
        width % 2 || height % 2 ||
        (size_t)width > SIZE_MAX / height) {
        fprintf(stderr, "invalid numeric argument\n");
        return 2;
    }

    frame_size = (size_t)width * height;
    if (frame_size > SIZE_MAX - frame_size / 2) {
        fprintf(stderr, "frame size overflow\n");
        return 2;
    }
    frame_size += frame_size / 2;

    if (argc == 9) {
        device = argv[8];
    } else {
        status = venus_v4l2_probe(&capabilities);
        if (status < 0 || capabilities.encoder_path[0] == '\0') {
            fprintf(stderr, "qcom-venus encoder was not found\n");
            return 1;
        }
        device = capabilities.encoder_path;
    }

    input = fopen(argv[6], "rb");
    if (!input) {
        fprintf(stderr, "open %s: %s\n", argv[6], strerror(errno));
        return 1;
    }
    run.output = fopen(argv[7], "wb");
    if (!run.output) {
        fprintf(stderr, "open %s: %s\n", argv[7], strerror(errno));
        fclose(input);
        return 1;
    }

    frame = malloc(frame_size);
    if (!frame) {
        status = -ENOMEM;
        goto finish;
    }

    config = (struct venus_v4l2_encoder_config) {
        .device = device,
        .coded_format = V4L2_PIX_FMT_H264,
        .width = width,
        .height = height,
        .frames_per_second = frames_per_second,
        .bitrate = bitrate,
        .rate_control_enabled = true,
        .bitrate_mode = V4L2_MPEG_VIDEO_BITRATE_MODE_VBR,
        .i_qp = 26,
        .p_qp = 28,
        .min_qp = 1,
        .max_qp = 51,
        .gop_size = frames_per_second,
        .h264_profile = V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE,
        .h264_level = V4L2_MPEG_VIDEO_H264_LEVEL_4_1,
        .h264_entropy_mode =
            V4L2_MPEG_VIDEO_H264_ENTROPY_MODE_CAVLC,
        .h264_transform_8x8 = 0,
        .capture_buffer_size = 1024 * 1024,
        .output_buffers = 4,
        .capture_buffers = 16,
    };

    status = venus_v4l2_encoder_open(
        &config, &encoder, &open_error);
    if (status < 0) {
        fprintf(stderr, "open encoder: operation=%s error=%s (%d)\n",
                open_error.operation[0] ? open_error.operation : "none",
                strerror(-status), -status);
        goto finish;
    }

    printf("device=%s\n", device);
    printf("frame_size=%zu\n", frame_size);
    printf("output_size=%u\n",
           venus_v4l2_encoder_output_size(encoder));
    printf("capture_size=%u\n",
           venus_v4l2_encoder_capture_size(encoder));
    printf("output_buffers=%u\n",
           venus_v4l2_encoder_output_count(encoder));
    printf("capture_buffers=%u\n",
           venus_v4l2_encoder_capture_count(encoder));

    for (frame_index = 0; frame_index < frame_count; frame_index++) {
        if (fread(frame, 1, frame_size, input) != frame_size) {
            status = -EIO;
            goto finish;
        }

        status = venus_v4l2_encoder_submit(
            encoder, frame, frame_size,
            1u + (uint64_t)frame_index * 1000000u / frames_per_second,
            write_packet, &run);
        if (status < 0)
            goto finish;
    }

    if (fgetc(input) != EOF) {
        status = -EFBIG;
        goto finish;
    }

    status = venus_v4l2_encoder_stop(encoder);
    if (status < 0)
        goto finish;

    deadline = monotonic_milliseconds() + 30000;
    while (!eos && monotonic_milliseconds() < deadline) {
        status = venus_v4l2_encoder_pump(
            encoder, 1000, write_packet, &run, &eos);
        if (status == -ETIMEDOUT || status == -EAGAIN)
            continue;
        if (status < 0)
            goto finish;
    }
    if (!eos || run.packets == 0)
        status = -ETIMEDOUT;

finish:
    printf("encoded_packets=%zu\n", run.packets);
    printf("encoded_bytes=%zu\n", run.bytes);
    printf("eos=%s\n", eos ? "yes" : "no");

    if (status < 0) {
        const char *operation =
            venus_v4l2_encoder_last_operation(encoder);

        if (status == -ETIMEDOUT && strcmp(operation, "none") == 0)
            operation = "wait(encoded packets)";
        fprintf(stderr, "FAIL operation=%s error=%s (%d)\n",
                operation, strerror(-status), -status);
    } else {
        puts("PASS: V4L2 stateful H.264 encode completed");
    }

    venus_v4l2_encoder_close(encoder);
    free(frame);
    if (run.output && fclose(run.output) != 0 && status == 0)
        status = -EIO;
    if (input)
        fclose(input);
    return status < 0 ? 1 : 0;
}
