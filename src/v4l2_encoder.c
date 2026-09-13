// SPDX-License-Identifier: MIT
#include "v4l2_encoder.h"

#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

struct mapped_buffer {
    void *data;
    size_t length;
    bool queued;
};

struct venus_v4l2_encoder {
    int fd;
    struct mapped_buffer *output;
    struct mapped_buffer *capture;
    unsigned int output_count;
    unsigned int capture_count;
    struct v4l2_pix_format_mplane output_format;
    struct v4l2_pix_format_mplane capture_format;
    bool output_streaming;
    bool capture_streaming;
    bool stop_sent;
    char last_operation[64];
};

static int xioctl(int fd, unsigned long request, void *argument)
{
    int result;

    do {
        result = ioctl(fd, request, argument);
    } while (result < 0 && errno == EINTR);

    return result;
}

static int encoder_error(struct venus_v4l2_encoder *encoder,
                         const char *operation)
{
    snprintf(encoder->last_operation, sizeof(encoder->last_operation),
             "%s", operation);
    return -errno;
}

static int verify_device(struct venus_v4l2_encoder *encoder)
{
    struct v4l2_capability capability = { 0 };
    uint32_t caps;

    if (xioctl(encoder->fd, VIDIOC_QUERYCAP, &capability) < 0)
        return encoder_error(encoder, "VIDIOC_QUERYCAP");

    caps = capability.capabilities & V4L2_CAP_DEVICE_CAPS
               ? capability.device_caps
               : capability.capabilities;

    if (strcmp((const char *)capability.driver, "qcom-venus") != 0 ||
        !strstr((const char *)capability.card, "encoder") ||
        !(caps & V4L2_CAP_VIDEO_M2M_MPLANE) ||
        !(caps & V4L2_CAP_STREAMING)) {
        errno = ENODEV;
        return encoder_error(encoder, "encoder capability contract");
    }

    return 0;
}

static int set_output_format(
    struct venus_v4l2_encoder *encoder,
    const struct venus_v4l2_encoder_config *config)
{
    struct v4l2_format format = {
        .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
    };

    format.fmt.pix_mp.width = config->width;
    format.fmt.pix_mp.height = config->height;
    format.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    format.fmt.pix_mp.field = V4L2_FIELD_NONE;
    format.fmt.pix_mp.num_planes = 1;

    if (xioctl(encoder->fd, VIDIOC_S_FMT, &format) < 0)
        return encoder_error(encoder, "VIDIOC_S_FMT(OUTPUT)");

    if (format.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_NV12 ||
        format.fmt.pix_mp.num_planes != 1 ||
        format.fmt.pix_mp.plane_fmt[0].sizeimage == 0) {
        errno = EINVAL;
        return encoder_error(encoder, "raw output format contract");
    }

    encoder->output_format = format.fmt.pix_mp;
    return 0;
}

static int set_capture_format(
    struct venus_v4l2_encoder *encoder,
    const struct venus_v4l2_encoder_config *config)
{
    struct v4l2_format format = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
    };

    format.fmt.pix_mp.width = config->width;
    format.fmt.pix_mp.height = config->height;
    format.fmt.pix_mp.pixelformat = config->coded_format;
    format.fmt.pix_mp.field = V4L2_FIELD_NONE;
    format.fmt.pix_mp.num_planes = 1;
    format.fmt.pix_mp.plane_fmt[0].sizeimage =
        (uint32_t)config->capture_buffer_size;

    if (xioctl(encoder->fd, VIDIOC_S_FMT, &format) < 0)
        return encoder_error(encoder, "VIDIOC_S_FMT(CAPTURE)");

    if (format.fmt.pix_mp.pixelformat != config->coded_format ||
        format.fmt.pix_mp.num_planes != 1 ||
        format.fmt.pix_mp.plane_fmt[0].sizeimage == 0) {
        errno = EINVAL;
        return encoder_error(encoder, "coded capture format contract");
    }

    encoder->capture_format = format.fmt.pix_mp;
    return 0;
}

static int set_control(struct venus_v4l2_encoder *encoder,
                       uint32_t id, int32_t value,
                       const char *operation)
{
    struct v4l2_control control = {
        .id = id,
        .value = value,
    };

    if (xioctl(encoder->fd, VIDIOC_S_CTRL, &control) < 0)
        return encoder_error(encoder, operation);
    return 0;
}

static int set_parameters(
    struct venus_v4l2_encoder *encoder,
    const struct venus_v4l2_encoder_config *config)
{
    struct v4l2_streamparm parameters = {
        .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
    };
    int status;

    parameters.parm.output.timeperframe.numerator = 1;
    parameters.parm.output.timeperframe.denominator =
        config->frames_per_second;
    if (xioctl(encoder->fd, VIDIOC_S_PARM, &parameters) < 0)
        return encoder_error(encoder, "VIDIOC_S_PARM(OUTPUT)");

    status = set_control(
        encoder, V4L2_CID_MPEG_VIDEO_BITRATE,
        (int32_t)config->bitrate, "S_CTRL(BITRATE)");
    if (status < 0)
        return status;

    status = set_control(
        encoder, V4L2_CID_MPEG_VIDEO_GOP_SIZE,
        (int32_t)config->gop_size, "S_CTRL(GOP_SIZE)");
    if (status < 0)
        return status;

    status = set_control(
        encoder, V4L2_CID_MPEG_VIDEO_B_FRAMES,
        0, "S_CTRL(B_FRAMES)");
    if (status < 0)
        return status;

    status = set_control(
        encoder, V4L2_CID_MPEG_VIDEO_H264_PROFILE,
        (int32_t)config->h264_profile, "S_CTRL(H264_PROFILE)");
    if (status < 0)
        return status;

    return set_control(
        encoder, V4L2_CID_MPEG_VIDEO_H264_LEVEL,
        V4L2_MPEG_VIDEO_H264_LEVEL_4_1, "S_CTRL(H264_LEVEL)");
}

static int request_and_map(struct venus_v4l2_encoder *encoder,
                           enum v4l2_buf_type type,
                           unsigned int requested,
                           struct mapped_buffer **mapped,
                           unsigned int *mapped_count)
{
    struct v4l2_requestbuffers request = {
        .count = requested,
        .type = type,
        .memory = V4L2_MEMORY_MMAP,
    };
    struct mapped_buffer *buffers;
    unsigned int index;

    if (xioctl(encoder->fd, VIDIOC_REQBUFS, &request) < 0)
        return encoder_error(
            encoder, type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE
                         ? "VIDIOC_REQBUFS(OUTPUT)"
                         : "VIDIOC_REQBUFS(CAPTURE)");
    if (request.count == 0)
        return -ENOMEM;

    buffers = calloc(request.count, sizeof(*buffers));
    if (!buffers)
        return -ENOMEM;

    for (index = 0; index < request.count; index++) {
        struct v4l2_plane planes[VIDEO_MAX_PLANES] = { 0 };
        struct v4l2_buffer buffer = {
            .index = index,
            .type = type,
            .memory = V4L2_MEMORY_MMAP,
            .length = 1,
            .m.planes = planes,
        };

        if (xioctl(encoder->fd, VIDIOC_QUERYBUF, &buffer) < 0) {
            encoder_error(
                encoder, type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE
                             ? "VIDIOC_QUERYBUF(OUTPUT)"
                             : "VIDIOC_QUERYBUF(CAPTURE)");
            goto fail;
        }
        if (buffer.length != 1 || planes[0].length == 0) {
            errno = EINVAL;
            encoder_error(encoder, "VIDIOC_QUERYBUF(plane contract)");
            goto fail;
        }

        buffers[index].length = planes[0].length;
        buffers[index].data =
            mmap(NULL, planes[0].length, PROT_READ | PROT_WRITE,
                 MAP_SHARED, encoder->fd, planes[0].m.mem_offset);
        if (buffers[index].data == MAP_FAILED) {
            buffers[index].data = NULL;
            encoder_error(
                encoder, type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE
                             ? "mmap(OUTPUT)"
                             : "mmap(CAPTURE)");
            goto fail;
        }
    }

    *mapped = buffers;
    *mapped_count = request.count;
    return 0;

fail:
    {
        int status = -errno;

        while (index > 0) {
            index--;
            if (buffers[index].data)
                munmap(buffers[index].data, buffers[index].length);
        }
        free(buffers);
        return status;
    }
}

static void release_buffers(struct venus_v4l2_encoder *encoder,
                            enum v4l2_buf_type type,
                            struct mapped_buffer **mapped,
                            unsigned int *mapped_count)
{
    struct v4l2_requestbuffers request = {
        .count = 0,
        .type = type,
        .memory = V4L2_MEMORY_MMAP,
    };
    unsigned int index;

    if (*mapped) {
        for (index = 0; index < *mapped_count; index++) {
            if ((*mapped)[index].data)
                munmap((*mapped)[index].data,
                       (*mapped)[index].length);
        }
        free(*mapped);
        *mapped = NULL;
        *mapped_count = 0;
    }

    if (encoder->fd >= 0)
        xioctl(encoder->fd, VIDIOC_REQBUFS, &request);
}

static int queue_capture(struct venus_v4l2_encoder *encoder,
                         unsigned int index)
{
    struct v4l2_plane planes[VIDEO_MAX_PLANES] = { 0 };
    struct v4l2_buffer buffer = {
        .index = index,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
        .memory = V4L2_MEMORY_MMAP,
        .length = 1,
        .m.planes = planes,
    };

    planes[0].length = (uint32_t)encoder->capture[index].length;
    if (xioctl(encoder->fd, VIDIOC_QBUF, &buffer) < 0)
        return encoder_error(encoder, "VIDIOC_QBUF(CAPTURE)");

    encoder->capture[index].queued = true;
    return 0;
}

static int stream_on(struct venus_v4l2_encoder *encoder,
                     enum v4l2_buf_type type)
{
    if (xioctl(encoder->fd, VIDIOC_STREAMON, &type) < 0)
        return encoder_error(
            encoder, type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE
                         ? "VIDIOC_STREAMON(OUTPUT)"
                         : "VIDIOC_STREAMON(CAPTURE)");

    if (type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
        encoder->output_streaming = true;
    else
        encoder->capture_streaming = true;
    return 0;
}

static void stream_off(struct venus_v4l2_encoder *encoder,
                       enum v4l2_buf_type type)
{
    if (encoder->fd >= 0)
        xioctl(encoder->fd, VIDIOC_STREAMOFF, &type);
}

int venus_v4l2_encoder_open(
    const struct venus_v4l2_encoder_config *config,
    struct venus_v4l2_encoder **result,
    struct venus_v4l2_error *error)
{
    struct venus_v4l2_encoder *encoder;
    unsigned int index;
    int status;

    if (error)
        memset(error, 0, sizeof(*error));
    if (result)
        *result = NULL;

    if (!config || !result || !config->device ||
        config->width == 0 || config->height == 0 ||
        config->frames_per_second == 0 || config->bitrate == 0 ||
        config->gop_size == 0 ||
        config->capture_buffer_size == 0 ||
        config->capture_buffer_size > UINT32_MAX ||
        config->output_buffers == 0 ||
        config->capture_buffers == 0)
        return -EINVAL;

    encoder = calloc(1, sizeof(*encoder));
    if (!encoder)
        return -ENOMEM;
    encoder->fd = -1;

    encoder->fd =
        open(config->device, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (encoder->fd < 0) {
        status = -errno;
        snprintf(encoder->last_operation,
                 sizeof(encoder->last_operation),
                 "open(encoder device)");
        goto fail;
    }

    status = verify_device(encoder);
    if (status < 0)
        goto fail;
    status = set_output_format(encoder, config);
    if (status < 0)
        goto fail;
    status = set_capture_format(encoder, config);
    if (status < 0)
        goto fail;
    status = set_parameters(encoder, config);
    if (status < 0)
        goto fail;

    status = request_and_map(
        encoder, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
        config->output_buffers, &encoder->output,
        &encoder->output_count);
    if (status < 0)
        goto fail;
    status = request_and_map(
        encoder, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
        config->capture_buffers, &encoder->capture,
        &encoder->capture_count);
    if (status < 0)
        goto fail;

    for (index = 0; index < encoder->capture_count; index++) {
        status = queue_capture(encoder, index);
        if (status < 0)
            goto fail;
    }

    status = stream_on(
        encoder, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
    if (status < 0)
        goto fail;
    status = stream_on(
        encoder, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
    if (status < 0)
        goto fail;

    *result = encoder;
    return 0;

fail:
    if (error) {
        error->code = -status;
        snprintf(error->operation, sizeof(error->operation), "%s",
                 venus_v4l2_encoder_last_operation(encoder));
    }
    venus_v4l2_encoder_close(encoder);
    return status;
}

static int dequeue_output(struct venus_v4l2_encoder *encoder,
                          bool *made_progress)
{
    for (;;) {
        struct v4l2_plane planes[VIDEO_MAX_PLANES] = { 0 };
        struct v4l2_buffer buffer = {
            .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
            .memory = V4L2_MEMORY_MMAP,
            .length = 1,
            .m.planes = planes,
        };

        if (xioctl(encoder->fd, VIDIOC_DQBUF, &buffer) < 0) {
            if (errno == EAGAIN)
                return 0;
            return encoder_error(encoder, "VIDIOC_DQBUF(OUTPUT)");
        }
        if (buffer.index >= encoder->output_count)
            return -EIO;

        encoder->output[buffer.index].queued = false;
        *made_progress = true;
    }
}

static int dequeue_capture(struct venus_v4l2_encoder *encoder,
                           venus_v4l2_packet_callback callback,
                           void *opaque, bool *end_of_stream,
                           bool *made_progress)
{
    for (;;) {
        struct v4l2_plane planes[VIDEO_MAX_PLANES] = { 0 };
        struct v4l2_buffer buffer = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
            .memory = V4L2_MEMORY_MMAP,
            .length = 1,
            .m.planes = planes,
        };
        struct venus_v4l2_packet packet;
        int status;

        if (xioctl(encoder->fd, VIDIOC_DQBUF, &buffer) < 0) {
            if (errno == EAGAIN)
                return 0;
            return encoder_error(encoder, "VIDIOC_DQBUF(CAPTURE)");
        }
        if (buffer.index >= encoder->capture_count ||
            planes[0].bytesused > encoder->capture[buffer.index].length)
            return -EIO;

        encoder->capture[buffer.index].queued = false;
        *made_progress = true;
        packet = (struct venus_v4l2_packet) {
            .data = encoder->capture[buffer.index].data,
            .size = planes[0].bytesused,
            .tag = (uint64_t)buffer.timestamp.tv_sec * 1000000u +
                   (uint64_t)buffer.timestamp.tv_usec,
            .flags = buffer.flags,
        };

        if (packet.size > 0 && callback) {
            status = callback(&packet, opaque);
            if (status < 0)
                return status;
        }

        if (buffer.flags & V4L2_BUF_FLAG_LAST) {
            if (end_of_stream)
                *end_of_stream = true;
            return 0;
        }

        status = queue_capture(encoder, buffer.index);
        if (status < 0)
            return status;
    }
}

int venus_v4l2_encoder_pump(struct venus_v4l2_encoder *encoder,
                            int timeout_ms,
                            venus_v4l2_packet_callback callback,
                            void *opaque, bool *end_of_stream)
{
    struct pollfd poll_fd;
    bool made_progress = false;
    int status;

    if (!encoder || encoder->fd < 0 || timeout_ms < 0)
        return -EINVAL;
    if (end_of_stream)
        *end_of_stream = false;

    poll_fd = (struct pollfd) {
        .fd = encoder->fd,
        .events = POLLIN | POLLOUT | POLLPRI,
    };
    do {
        status = poll(&poll_fd, 1, timeout_ms);
    } while (status < 0 && errno == EINTR);
    if (status < 0)
        return encoder_error(encoder, "poll");
    if (status == 0)
        return -ETIMEDOUT;

    status = dequeue_output(encoder, &made_progress);
    if (status < 0)
        return status;
    status = dequeue_capture(
        encoder, callback, opaque, end_of_stream, &made_progress);
    if (status < 0)
        return status;

    if ((poll_fd.revents & (POLLERR | POLLHUP | POLLNVAL)) &&
        !made_progress)
        return -EIO;
    return made_progress ? 0 : -EAGAIN;
}

static int find_free_output(struct venus_v4l2_encoder *encoder,
                            venus_v4l2_packet_callback callback,
                            void *opaque)
{
    unsigned int index;

    for (;;) {
        for (index = 0; index < encoder->output_count; index++) {
            if (!encoder->output[index].queued)
                return (int)index;
        }

        {
            int status = venus_v4l2_encoder_pump(
                encoder, 5000, callback, opaque, NULL);

            if (status < 0 && status != -EAGAIN)
                return status;
        }
    }
}

int venus_v4l2_encoder_submit(struct venus_v4l2_encoder *encoder,
                              const uint8_t *data, size_t size,
                              uint64_t tag,
                              venus_v4l2_packet_callback callback,
                              void *opaque)
{
    struct v4l2_plane planes[VIDEO_MAX_PLANES] = { 0 };
    struct v4l2_buffer buffer = {
        .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
        .memory = V4L2_MEMORY_MMAP,
        .length = 1,
        .m.planes = planes,
    };
    int index;

    if (!encoder || !data || size == 0)
        return -EINVAL;

    index = find_free_output(encoder, callback, opaque);
    if (index < 0)
        return index;
    if (size > encoder->output[index].length)
        return -ENOSPC;

    memcpy(encoder->output[index].data, data, size);
    buffer.index = (unsigned int)index;
    buffer.timestamp.tv_sec = (long)(tag / 1000000u);
    buffer.timestamp.tv_usec = (long)(tag % 1000000u);
    planes[0].bytesused = (uint32_t)size;
    planes[0].length = (uint32_t)encoder->output[index].length;

    if (xioctl(encoder->fd, VIDIOC_QBUF, &buffer) < 0)
        return encoder_error(encoder, "VIDIOC_QBUF(OUTPUT)");

    encoder->output[index].queued = true;
    return 0;
}

int venus_v4l2_encoder_stop(struct venus_v4l2_encoder *encoder)
{
    struct v4l2_encoder_cmd command = {
        .cmd = V4L2_ENC_CMD_STOP,
    };

    if (!encoder || encoder->fd < 0)
        return -EINVAL;
    if (encoder->stop_sent)
        return 0;

    if (xioctl(encoder->fd, VIDIOC_ENCODER_CMD, &command) < 0)
        return encoder_error(encoder, "VIDIOC_ENCODER_CMD(STOP)");

    encoder->stop_sent = true;
    return 0;
}

void venus_v4l2_encoder_close(struct venus_v4l2_encoder *encoder)
{
    if (!encoder)
        return;

    if (encoder->capture_streaming)
        stream_off(
            encoder, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
    if (encoder->output_streaming)
        stream_off(
            encoder, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);

    release_buffers(
        encoder, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
        &encoder->capture, &encoder->capture_count);
    release_buffers(
        encoder, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
        &encoder->output, &encoder->output_count);

    if (encoder->fd >= 0)
        close(encoder->fd);
    free(encoder);
}

const char *venus_v4l2_encoder_last_operation(
    const struct venus_v4l2_encoder *encoder)
{
    if (!encoder || encoder->last_operation[0] == '\0')
        return "none";
    return encoder->last_operation;
}

unsigned int venus_v4l2_encoder_output_count(
    const struct venus_v4l2_encoder *encoder)
{
    return encoder ? encoder->output_count : 0;
}

unsigned int venus_v4l2_encoder_capture_count(
    const struct venus_v4l2_encoder *encoder)
{
    return encoder ? encoder->capture_count : 0;
}

uint32_t venus_v4l2_encoder_output_size(
    const struct venus_v4l2_encoder *encoder)
{
    return encoder ? encoder->output_format.plane_fmt[0].sizeimage : 0;
}

uint32_t venus_v4l2_encoder_capture_size(
    const struct venus_v4l2_encoder *encoder)
{
    return encoder ? encoder->capture_format.plane_fmt[0].sizeimage : 0;
}
