// SPDX-License-Identifier: MIT
#include "v4l2_decoder.h"

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

struct venus_v4l2_decoder {
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
    unsigned int source_changes;
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

static int decoder_error(struct venus_v4l2_decoder *decoder,
                         const char *operation)
{
    snprintf(decoder->last_operation, sizeof(decoder->last_operation),
             "%s", operation);
    return -errno;
}

static int set_output_format(
    struct venus_v4l2_decoder *decoder,
    const struct venus_v4l2_decoder_config *config)
{
    struct v4l2_format format = {
        .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
    };

    format.fmt.pix_mp.width = config->width;
    format.fmt.pix_mp.height = config->height;
    format.fmt.pix_mp.pixelformat = config->coded_format;
    format.fmt.pix_mp.field = V4L2_FIELD_NONE;
    format.fmt.pix_mp.num_planes = 1;
    format.fmt.pix_mp.plane_fmt[0].sizeimage =
        (uint32_t)config->output_buffer_size;

    if (xioctl(decoder->fd, VIDIOC_S_FMT, &format) < 0)
        return decoder_error(decoder, "VIDIOC_S_FMT(OUTPUT)");

    if (format.fmt.pix_mp.pixelformat != config->coded_format ||
        format.fmt.pix_mp.num_planes != 1 ||
        format.fmt.pix_mp.plane_fmt[0].sizeimage == 0)
        return -EINVAL;

    decoder->output_format = format.fmt.pix_mp;
    return 0;
}

static int set_capture_format(
    struct venus_v4l2_decoder *decoder,
    const struct venus_v4l2_decoder_config *config)
{
    struct v4l2_format format = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
    };

    format.fmt.pix_mp.width = config->width;
    format.fmt.pix_mp.height = config->height;
    format.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    format.fmt.pix_mp.field = V4L2_FIELD_NONE;
    format.fmt.pix_mp.num_planes = 1;

    if (xioctl(decoder->fd, VIDIOC_S_FMT, &format) < 0)
        return decoder_error(decoder, "VIDIOC_S_FMT(CAPTURE)");

    if (format.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_NV12 ||
        format.fmt.pix_mp.num_planes != 1 ||
        format.fmt.pix_mp.plane_fmt[0].sizeimage == 0)
        return -EINVAL;

    decoder->capture_format = format.fmt.pix_mp;
    return 0;
}

static int request_and_map(struct venus_v4l2_decoder *decoder,
                           enum v4l2_buf_type type, unsigned int requested,
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

    if (xioctl(decoder->fd, VIDIOC_REQBUFS, &request) < 0)
        return decoder_error(
            decoder, type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE
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

        if (xioctl(decoder->fd, VIDIOC_QUERYBUF, &buffer) < 0) {
            decoder_error(
                decoder, type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE
                             ? "VIDIOC_QUERYBUF(OUTPUT)"
                             : "VIDIOC_QUERYBUF(CAPTURE)");
            goto fail;
        }

        if (buffer.length != 1 || planes[0].length == 0) {
            errno = EINVAL;
            decoder_error(decoder, "VIDIOC_QUERYBUF(plane contract)");
            goto fail;
        }

        buffers[index].length = planes[0].length;
        buffers[index].data =
            mmap(NULL, planes[0].length, PROT_READ | PROT_WRITE,
                 MAP_SHARED, decoder->fd, planes[0].m.mem_offset);
        if (buffers[index].data == MAP_FAILED) {
            buffers[index].data = NULL;
            decoder_error(
                decoder, type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE
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

static void release_buffers(struct venus_v4l2_decoder *decoder,
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

    if (decoder->fd >= 0)
        xioctl(decoder->fd, VIDIOC_REQBUFS, &request);
}

static int queue_capture(struct venus_v4l2_decoder *decoder,
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

    planes[0].length = (uint32_t)decoder->capture[index].length;
    if (xioctl(decoder->fd, VIDIOC_QBUF, &buffer) < 0)
        return decoder_error(decoder, "VIDIOC_QBUF(CAPTURE)");

    decoder->capture[index].queued = true;
    return 0;
}

static int stream_on(struct venus_v4l2_decoder *decoder,
                     enum v4l2_buf_type type)
{
    if (xioctl(decoder->fd, VIDIOC_STREAMON, &type) < 0)
        return decoder_error(
            decoder, type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE
                         ? "VIDIOC_STREAMON(OUTPUT)"
                         : "VIDIOC_STREAMON(CAPTURE)");

    if (type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
        decoder->output_streaming = true;
    else
        decoder->capture_streaming = true;

    return 0;
}

static void stream_off(struct venus_v4l2_decoder *decoder,
                       enum v4l2_buf_type type)
{
    if (decoder->fd < 0)
        return;

    xioctl(decoder->fd, VIDIOC_STREAMOFF, &type);
}

static int verify_device(struct venus_v4l2_decoder *decoder)
{
    struct v4l2_capability capability = { 0 };
    uint32_t caps;

    if (xioctl(decoder->fd, VIDIOC_QUERYCAP, &capability) < 0)
        return decoder_error(decoder, "VIDIOC_QUERYCAP");

    caps = capability.capabilities & V4L2_CAP_DEVICE_CAPS
               ? capability.device_caps
               : capability.capabilities;

    if (strcmp((const char *)capability.driver, "qcom-venus") != 0 ||
        !strstr((const char *)capability.card, "decoder") ||
        !(caps & V4L2_CAP_VIDEO_M2M_MPLANE) ||
        !(caps & V4L2_CAP_STREAMING))
        return -ENODEV;

    return 0;
}

int venus_v4l2_decoder_open(
    const struct venus_v4l2_decoder_config *config,
    struct venus_v4l2_decoder **result,
    struct venus_v4l2_error *error)
{
    struct v4l2_event_subscription subscription = {
        .type = V4L2_EVENT_SOURCE_CHANGE,
    };
    struct venus_v4l2_decoder *decoder;
    unsigned int index;
    int status;

    if (error)
        memset(error, 0, sizeof(*error));
    if (result)
        *result = NULL;

    if (!config || !result || !config->device ||
        config->width == 0 || config->height == 0 ||
        config->output_buffer_size == 0 ||
        config->output_buffer_size > UINT32_MAX ||
        config->output_buffers == 0 ||
        config->capture_buffers == 0)
        return -EINVAL;

    decoder = calloc(1, sizeof(*decoder));
    if (!decoder)
        return -ENOMEM;
    decoder->fd = -1;

    decoder->fd =
        open(config->device, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (decoder->fd < 0) {
        status = -errno;
        snprintf(decoder->last_operation,
                 sizeof(decoder->last_operation),
                 "open(decoder device)");
        goto fail;
    }

    status = verify_device(decoder);
    if (status < 0)
        goto fail;

    if (xioctl(decoder->fd, VIDIOC_SUBSCRIBE_EVENT, &subscription) < 0 &&
        errno != EINVAL) {
        status = decoder_error(decoder, "VIDIOC_SUBSCRIBE_EVENT");
        goto fail;
    }

    status = set_output_format(decoder, config);
    if (status < 0)
        goto fail;

    status = set_capture_format(decoder, config);
    if (status < 0)
        goto fail;

    status = request_and_map(
        decoder, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
        config->output_buffers, &decoder->output,
        &decoder->output_count);
    if (status < 0)
        goto fail;

    status = request_and_map(
        decoder, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
        config->capture_buffers, &decoder->capture,
        &decoder->capture_count);
    if (status < 0)
        goto fail;

    for (index = 0; index < decoder->capture_count; index++) {
        status = queue_capture(decoder, index);
        if (status < 0)
            goto fail;
    }

    status = stream_on(
        decoder, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
    if (status < 0)
        goto fail;

    status = stream_on(
        decoder, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
    if (status < 0)
        goto fail;

    *result = decoder;
    return 0;

fail:
    if (error) {
        error->code = -status;
        snprintf(error->operation, sizeof(error->operation), "%s",
                 venus_v4l2_decoder_last_operation(decoder));
    }
    venus_v4l2_decoder_close(decoder);
    return status;
}

static int dequeue_output(struct venus_v4l2_decoder *decoder,
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

        if (xioctl(decoder->fd, VIDIOC_DQBUF, &buffer) < 0) {
            if (errno == EAGAIN)
                return 0;
            return decoder_error(decoder, "VIDIOC_DQBUF(OUTPUT)");
        }

        if (buffer.index >= decoder->output_count)
            return -EIO;

        decoder->output[buffer.index].queued = false;
        *made_progress = true;
    }
}

static int dequeue_capture(struct venus_v4l2_decoder *decoder,
                           venus_v4l2_frame_callback callback,
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
        struct venus_v4l2_frame frame;
        int status;

        if (xioctl(decoder->fd, VIDIOC_DQBUF, &buffer) < 0) {
            if (errno == EAGAIN)
                return 0;
            return decoder_error(decoder, "VIDIOC_DQBUF(CAPTURE)");
        }

        if (buffer.index >= decoder->capture_count ||
            planes[0].bytesused > decoder->capture[buffer.index].length)
            return -EIO;

        decoder->capture[buffer.index].queued = false;
        *made_progress = true;

        frame = (struct venus_v4l2_frame) {
            .data = decoder->capture[buffer.index].data,
            .size = planes[0].bytesused,
            .tag = (uint64_t)buffer.timestamp.tv_sec * 1000000u +
                   (uint64_t)buffer.timestamp.tv_usec,
            .flags = buffer.flags,
            .width = decoder->capture_format.width,
            .height = decoder->capture_format.height,
            .bytes_per_line =
                decoder->capture_format.plane_fmt[0].bytesperline,
        };

        if (frame.size > 0 && callback) {
            status = callback(&frame, opaque);
            if (status < 0)
                return status;
        }

        if (buffer.flags & V4L2_BUF_FLAG_LAST) {
            if (end_of_stream)
                *end_of_stream = true;
            return 0;
        }

        status = queue_capture(decoder, buffer.index);
        if (status < 0)
            return status;
    }
}

static int dequeue_events(struct venus_v4l2_decoder *decoder,
                          bool *made_progress)
{
    for (;;) {
        struct v4l2_event event = { 0 };

        if (xioctl(decoder->fd, VIDIOC_DQEVENT, &event) < 0) {
            if (errno == EAGAIN)
                return 0;
            return decoder_error(decoder, "VIDIOC_DQEVENT");
        }

        *made_progress = true;
        if (event.type == V4L2_EVENT_SOURCE_CHANGE &&
            event.u.src_change.changes & V4L2_EVENT_SRC_CH_RESOLUTION) {
            struct v4l2_format format = {
                .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
            };

            decoder->source_changes++;
            if (xioctl(decoder->fd, VIDIOC_G_FMT, &format) < 0)
                return decoder_error(
                    decoder, "VIDIOC_G_FMT(CAPTURE source-change)");

            if (format.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_NV12 ||
                format.fmt.pix_mp.num_planes != 1 ||
                format.fmt.pix_mp.plane_fmt[0].sizeimage >
                    decoder->capture[0].length)
                return -EOVERFLOW;

            decoder->capture_format = format.fmt.pix_mp;
        }
    }
}

int venus_v4l2_decoder_pump(struct venus_v4l2_decoder *decoder,
                            int timeout_ms,
                            venus_v4l2_frame_callback callback,
                            void *opaque, bool *end_of_stream)
{
    struct pollfd poll_fd;
    bool made_progress = false;
    int status;

    if (!decoder || decoder->fd < 0 || timeout_ms < 0)
        return -EINVAL;

    if (end_of_stream)
        *end_of_stream = false;

    poll_fd = (struct pollfd) {
        .fd = decoder->fd,
        .events = POLLIN | POLLOUT | POLLPRI,
    };

    status = poll(&poll_fd, 1, timeout_ms);
    if (status < 0)
        return decoder_error(decoder, "poll");
    if (status == 0)
        return -ETIMEDOUT;

    if (poll_fd.revents & POLLPRI) {
        status = dequeue_events(decoder, &made_progress);
        if (status < 0)
            return status;
    }

    status = dequeue_output(decoder, &made_progress);
    if (status < 0)
        return status;

    status = dequeue_capture(
        decoder, callback, opaque, end_of_stream, &made_progress);
    if (status < 0)
        return status;

    if ((poll_fd.revents & (POLLERR | POLLHUP | POLLNVAL)) &&
        !made_progress)
        return -EIO;

    return made_progress ? 0 : -EAGAIN;
}

static int find_free_output(struct venus_v4l2_decoder *decoder,
                            venus_v4l2_frame_callback callback,
                            void *opaque)
{
    unsigned int index;

    for (;;) {
        for (index = 0; index < decoder->output_count; index++) {
            if (!decoder->output[index].queued)
                return (int)index;
        }

        {
            int status = venus_v4l2_decoder_pump(
                decoder, 5000, callback, opaque, NULL);

            if (status < 0 && status != -EAGAIN)
                return status;
        }
    }
}

int venus_v4l2_decoder_submit(struct venus_v4l2_decoder *decoder,
                              const uint8_t *data, size_t size,
                              uint64_t tag,
                              venus_v4l2_frame_callback callback,
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

    if (!decoder || !data || size == 0)
        return -EINVAL;

    index = find_free_output(decoder, callback, opaque);
    if (index < 0)
        return index;

    if (size > decoder->output[index].length)
        return -ENOSPC;

    memcpy(decoder->output[index].data, data, size);
    buffer.index = (unsigned int)index;
    buffer.timestamp.tv_sec = (long)(tag / 1000000u);
    buffer.timestamp.tv_usec = (long)(tag % 1000000u);
    planes[0].bytesused = (uint32_t)size;
    planes[0].length = (uint32_t)decoder->output[index].length;

    if (xioctl(decoder->fd, VIDIOC_QBUF, &buffer) < 0)
        return decoder_error(decoder, "VIDIOC_QBUF(OUTPUT)");

    decoder->output[index].queued = true;
    return 0;
}

int venus_v4l2_decoder_stop(struct venus_v4l2_decoder *decoder)
{
    struct v4l2_decoder_cmd command = {
        .cmd = V4L2_DEC_CMD_STOP,
    };

    if (!decoder || decoder->fd < 0)
        return -EINVAL;
    if (decoder->stop_sent)
        return 0;

    if (xioctl(decoder->fd, VIDIOC_DECODER_CMD, &command) < 0)
        return decoder_error(decoder, "VIDIOC_DECODER_CMD(STOP)");

    decoder->stop_sent = true;
    return 0;
}

void venus_v4l2_decoder_close(struct venus_v4l2_decoder *decoder)
{
    if (!decoder)
        return;

    if (decoder->capture_streaming)
        stream_off(
            decoder, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
    if (decoder->output_streaming)
        stream_off(
            decoder, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);

    release_buffers(
        decoder, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
        &decoder->capture, &decoder->capture_count);
    release_buffers(
        decoder, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
        &decoder->output, &decoder->output_count);

    if (decoder->fd >= 0)
        close(decoder->fd);
    free(decoder);
}

const char *venus_v4l2_decoder_last_operation(
    const struct venus_v4l2_decoder *decoder)
{
    if (!decoder || decoder->last_operation[0] == '\0')
        return "none";
    return decoder->last_operation;
}

unsigned int venus_v4l2_decoder_output_count(
    const struct venus_v4l2_decoder *decoder)
{
    return decoder ? decoder->output_count : 0;
}

unsigned int venus_v4l2_decoder_capture_count(
    const struct venus_v4l2_decoder *decoder)
{
    return decoder ? decoder->capture_count : 0;
}

uint32_t venus_v4l2_decoder_capture_width(
    const struct venus_v4l2_decoder *decoder)
{
    return decoder ? decoder->capture_format.width : 0;
}

uint32_t venus_v4l2_decoder_capture_height(
    const struct venus_v4l2_decoder *decoder)
{
    return decoder ? decoder->capture_format.height : 0;
}

uint32_t venus_v4l2_decoder_capture_size(
    const struct venus_v4l2_decoder *decoder)
{
    return decoder ? decoder->capture_format.plane_fmt[0].sizeimage : 0;
}

unsigned int venus_v4l2_decoder_source_changes(
    const struct venus_v4l2_decoder *decoder)
{
    return decoder ? decoder->source_changes : 0;
}
