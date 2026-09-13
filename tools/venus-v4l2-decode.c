// SPDX-License-Identifier: MIT
#include "annexb_split.h"
#include "v4l2_decoder.h"
#include "v4l2_probe.h"

#include <errno.h>
#include <limits.h>
#include <time.h>
#include <linux/videodev2.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct access_unit_stats {
    size_t count;
    size_t maximum;
};

struct decode_run {
    struct venus_v4l2_decoder *decoder;
    FILE *output;
    size_t frames;
    size_t bytes;
    uint64_t next_tag;
    int error;
};

static int inspect_access_unit(const uint8_t *data, size_t size,
                               void *opaque)
{
    struct access_unit_stats *stats = opaque;

    (void)data;
    stats->count++;
    if (size > stats->maximum)
        stats->maximum = size;
    return 0;
}

static int write_frame(const struct venus_v4l2_frame *frame, void *opaque)
{
    struct decode_run *run = opaque;

    if (fwrite(frame->data, 1, frame->size, run->output) != frame->size) {
        run->error = -EIO;
        return run->error;
    }

    run->frames++;
    run->bytes += frame->size;
    return 0;
}

static int submit_access_unit(const uint8_t *data, size_t size,
                              void *opaque)
{
    struct decode_run *run = opaque;
    int status;

    status = venus_v4l2_decoder_submit(
        run->decoder, data, size, run->next_tag++,
        write_frame, run);
    if (status < 0)
        run->error = status;

    return status;
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

static int read_file(const char *path, uint8_t **data, size_t *size)
{
    FILE *file;
    long length;
    uint8_t *contents;

    file = fopen(path, "rb");
    if (!file)
        return -errno;

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return -EIO;
    }

    length = ftell(file);
    if (length <= 0 || length > 64 * 1024 * 1024) {
        fclose(file);
        return -EFBIG;
    }

    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return -EIO;
    }

    contents = malloc((size_t)length);
    if (!contents) {
        fclose(file);
        return -ENOMEM;
    }

    if (fread(contents, 1, (size_t)length, file) != (size_t)length) {
        free(contents);
        fclose(file);
        return -EIO;
    }

    fclose(file);
    *data = contents;
    *size = (size_t)length;
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
    struct venus_v4l2_decoder_config config;
    struct venus_v4l2_decoder *decoder = NULL;
    struct venus_v4l2_error open_error = { 0 };
    struct access_unit_stats stats = { 0 };
    struct decode_run run = { 0 };
    const char *device;
    const char *input_path;
    const char *output_path;
    uint8_t *input = NULL;
    size_t input_size = 0;
    size_t units = 0;
    unsigned int width;
    unsigned int height;
    unsigned int expected_frames;
    bool eos = false;
    int64_t deadline;
    int status;

    if (argc != 6 && argc != 7) {
        fprintf(stderr,
                "usage: %s WIDTH HEIGHT FRAMES INPUT OUTPUT [DEVICE]\n",
                argv[0]);
        return 2;
    }

    if (parse_positive(argv[1], &width) < 0 ||
        parse_positive(argv[2], &height) < 0 ||
        parse_positive(argv[3], &expected_frames) < 0) {
        fprintf(stderr, "width, height and frames must be positive\n");
        return 2;
    }

    input_path = argv[4];
    output_path = argv[5];

    if (argc == 7) {
        device = argv[6];
    } else {
        status = venus_v4l2_probe(&capabilities);
        if (status < 0 || capabilities.decoder_path[0] == '\0') {
            fprintf(stderr, "qcom-venus decoder was not found\n");
            return 1;
        }
        device = capabilities.decoder_path;
    }

    status = read_file(input_path, &input, &input_size);
    if (status < 0) {
        fprintf(stderr, "read %s: %s\n", input_path,
                strerror(-status));
        return 1;
    }

    status = venus_annexb_for_each_access_unit(
        input, input_size, inspect_access_unit, &stats, &units);
    if (status < 0) {
        fprintf(stderr, "split Annex-B input: %s\n", strerror(-status));
        free(input);
        return 1;
    }

    config = (struct venus_v4l2_decoder_config) {
        .device = device,
        .coded_format = V4L2_PIX_FMT_H264,
        .width = width,
        .height = height,
        .output_buffer_size =
            stats.maximum < 1024 * 1024 ? 1024 * 1024 : stats.maximum,
        .output_buffers = 4,
        .capture_buffers = 16,
    };

    status = venus_v4l2_decoder_open(
        &config, &decoder, &open_error);
    if (status < 0) {
        fprintf(stderr, "open decoder: operation=%s error=%s (%d)\n",
                open_error.operation[0] ? open_error.operation : "none",
                strerror(-status), -status);
        free(input);
        return 1;
    }

    run.decoder = decoder;
    run.output = fopen(output_path, "wb");
    if (!run.output) {
        fprintf(stderr, "open %s: %s\n", output_path, strerror(errno));
        venus_v4l2_decoder_close(decoder);
        free(input);
        return 1;
    }

    printf("device=%s\n", device);
    printf("input_bytes=%zu\n", input_size);
    printf("access_units=%zu\n", units);
    printf("output_buffers=%u\n",
           venus_v4l2_decoder_output_count(decoder));
    printf("capture_buffers=%u\n",
           venus_v4l2_decoder_capture_count(decoder));
    printf("capture_format=%ux%u size=%u\n",
           venus_v4l2_decoder_capture_width(decoder),
           venus_v4l2_decoder_capture_height(decoder),
           venus_v4l2_decoder_capture_size(decoder));

    status = venus_annexb_for_each_access_unit(
        input, input_size, submit_access_unit, &run, NULL);
    if (status < 0)
        goto finish;

    status = venus_v4l2_decoder_stop(decoder);
    if (status < 0)
        goto finish;

    deadline = monotonic_milliseconds() + 30000;
    while (run.frames < expected_frames && !eos &&
           monotonic_milliseconds() < deadline) {
        status = venus_v4l2_decoder_pump(
            decoder, 1000, write_frame, &run, &eos);
        if (status == -ETIMEDOUT || status == -EAGAIN)
            continue;
        if (status < 0)
            goto finish;
    }

    if (run.frames != expected_frames)
        status = -ETIMEDOUT;

finish:
    if (fclose(run.output) != 0 && status == 0)
        status = -EIO;

    printf("decoded_frames=%zu\n", run.frames);
    printf("decoded_bytes=%zu\n", run.bytes);
    printf("source_changes=%u\n",
           venus_v4l2_decoder_source_changes(decoder));
    printf("eos=%s\n", eos ? "yes" : "no");

    if (status < 0) {
        fprintf(stderr, "FAIL operation=%s error=%s (%d)\n",
                venus_v4l2_decoder_last_operation(decoder),
                strerror(-status), -status);
    } else {
        puts("PASS: V4L2 stateful H.264 decode completed");
    }

    venus_v4l2_decoder_close(decoder);
    free(input);
    return status < 0 ? 1 : 0;
}
