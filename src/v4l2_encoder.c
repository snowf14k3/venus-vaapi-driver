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
    int dma_fd;
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
    uint32_t visible_width;
    uint32_t visible_height;
    enum v4l2_memory output_memory;
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

int venus_v4l2_encoder_compressed_size(
    uint32_t width, uint32_t height, size_t *size)
{
    size_t aligned_width;
    size_t aligned_height;
    size_t pixels;
    size_t compressed;

    if (!width || !height || !size)
        return -EINVAL;
    aligned_width = ((size_t)width + 31u) & ~(size_t)31u;
    aligned_height = ((size_t)height + 31u) & ~(size_t)31u;
    if (aligned_width > SIZE_MAX / aligned_height)
        return -EOVERFLOW;
    pixels = aligned_width * aligned_height;
    if (pixels > SIZE_MAX / 3u)
        return -EOVERFLOW;
    compressed = pixels * 3u / 4u;
    if (compressed > SIZE_MAX - 4095u)
        return -EOVERFLOW;
    *size = (compressed + 4095u) & ~(size_t)4095u;
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

    status = set_control(
        encoder, V4L2_CID_MPEG_VIDEO_B_FRAMES,
        0, "S_CTRL(B_FRAMES)");
    if (status < 0)
        return status;

    parameters.parm.output.timeperframe.numerator = 1;
    parameters.parm.output.timeperframe.denominator =
        config->frames_per_second;
    if (xioctl(encoder->fd, VIDIOC_S_PARM, &parameters) < 0)
        return encoder_error(encoder, "VIDIOC_S_PARM(OUTPUT)");

    status = set_control(
        encoder, V4L2_CID_MPEG_VIDEO_HEADER_MODE,
        V4L2_MPEG_VIDEO_HEADER_MODE_JOINED_WITH_1ST_FRAME,
        "S_CTRL(HEADER_MODE)");
    if (status < 0)
        return status;

    status = set_control(
        encoder, V4L2_CID_MPEG_VIDEO_AU_DELIMITER,
        config->h264_aud ? 1 : 0,
        "S_CTRL(H264_AU_DELIMITER)");
    if (status < 0)
        return status;

    if (config->rate_control_enabled) {
        if (config->bitrate_mode !=
            V4L2_MPEG_VIDEO_BITRATE_MODE_VBR) {
            status = set_control(
                encoder, V4L2_CID_MPEG_VIDEO_BITRATE_MODE,
                (int32_t)config->bitrate_mode,
                "S_CTRL(BITRATE_MODE)");
            if (status < 0)
                return status;
        }
        status = set_control(
            encoder, V4L2_CID_MPEG_VIDEO_BITRATE,
            (int32_t)config->bitrate,
            "S_CTRL(BITRATE)");
        if (status < 0)
            return status;
    }

    status = set_control(
        encoder, V4L2_CID_MPEG_VIDEO_H264_I_FRAME_QP,
        (int32_t)config->h264_i_qp,
        "S_CTRL(H264_I_FRAME_QP)");
    if (status < 0)
        return status;
    status = set_control(
        encoder, V4L2_CID_MPEG_VIDEO_H264_P_FRAME_QP,
        (int32_t)config->h264_p_qp,
        "S_CTRL(H264_P_FRAME_QP)");
    if (status < 0)
        return status;

    status = set_control(
        encoder, V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE,
        config->rate_control_enabled ? 1 : 0,
        "S_CTRL(FRAME_RC_ENABLE)");
    if (status < 0)
        return status;

    status = set_control(
        encoder, V4L2_CID_MPEG_VIDEO_GOP_SIZE,
        (int32_t)config->gop_size, "S_CTRL(GOP_SIZE)");
    if (status < 0)
        return status;

    status = set_control(
        encoder, V4L2_CID_MPEG_VIDEO_H264_PROFILE,
        (int32_t)config->h264_profile, "S_CTRL(H264_PROFILE)");
    if (status < 0)
        return status;

    status = set_control(
        encoder, V4L2_CID_MPEG_VIDEO_H264_MIN_QP,
        (int32_t)config->h264_min_qp,
        "S_CTRL(H264_MIN_QP)");
    if (status < 0)
        return status;
    status = set_control(
        encoder, V4L2_CID_MPEG_VIDEO_H264_MAX_QP,
        (int32_t)config->h264_max_qp,
        "S_CTRL(H264_MAX_QP)");
    if (status < 0)
        return status;

    status = set_control(
        encoder, V4L2_CID_MPEG_VIDEO_H264_ENTROPY_MODE,
        (int32_t)config->h264_entropy_mode,
        "S_CTRL(H264_ENTROPY_MODE)");
    if (status < 0)
        return status;

    status = set_control(
        encoder, V4L2_CID_MPEG_VIDEO_H264_8X8_TRANSFORM,
        (int32_t)config->h264_transform_8x8,
        "S_CTRL(H264_8X8_TRANSFORM)");
    if (status < 0)
        return status;

    return set_control(
        encoder, V4L2_CID_MPEG_VIDEO_H264_LEVEL,
        (int32_t)config->h264_level,
        "S_CTRL(H264_LEVEL)");
}

static int request_and_map(struct venus_v4l2_encoder *encoder,
                           enum v4l2_buf_type type,
                           enum v4l2_memory memory,
                           unsigned int requested,
                           struct mapped_buffer **mapped,
                           unsigned int *mapped_count)
{
    struct v4l2_requestbuffers request = {
        .count = requested,
        .type = type,
        .memory = memory,
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

    for (index = 0; index < request.count; index++)
        buffers[index].dma_fd = -1;

    if (memory == V4L2_MEMORY_DMABUF) {
        size_t length =
            encoder->output_format.plane_fmt[0].sizeimage;

        if (type != V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE ||
            length == 0) {
            free(buffers);
            return -EINVAL;
        }
        for (index = 0; index < request.count; index++)
            buffers[index].length = length;
        *mapped = buffers;
        *mapped_count = request.count;
        return 0;
    }

    for (index = 0; index < request.count; index++) {
        struct v4l2_plane planes[VIDEO_MAX_PLANES] = { 0 };
        struct v4l2_buffer buffer = {
            .index = index,
            .type = type,
            .memory = memory,
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
                            enum v4l2_memory memory,
                            struct mapped_buffer **mapped,
                            unsigned int *mapped_count)
{
    struct v4l2_requestbuffers request = {
        .count = 0,
        .type = type,
        .memory = memory,
    };
    unsigned int index;

    if (*mapped) {
        for (index = 0; index < *mapped_count; index++) {
            if ((*mapped)[index].data)
                munmap((*mapped)[index].data,
                       (*mapped)[index].length);
            if ((*mapped)[index].dma_fd >= 0)
                close((*mapped)[index].dma_fd);
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

static void subscribe_eos(struct venus_v4l2_encoder *encoder)
{
    struct v4l2_event_subscription subscription = {
        .type = V4L2_EVENT_EOS,
    };

    xioctl(encoder->fd, VIDIOC_SUBSCRIBE_EVENT, &subscription);
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
        config->frames_per_second == 0 ||
        config->gop_size == 0 ||
        config->capture_buffer_size == 0 ||
        config->capture_buffer_size > UINT32_MAX ||
        config->output_buffers == 0 ||
        config->capture_buffers == 0)
        return -EINVAL;
    if (config->rate_control_enabled) {
        if ((config->bitrate_mode !=
                 V4L2_MPEG_VIDEO_BITRATE_MODE_VBR &&
             config->bitrate_mode !=
                 V4L2_MPEG_VIDEO_BITRATE_MODE_CBR) ||
            config->bitrate == 0)
            return -EINVAL;
    }
    if (config->h264_i_qp < 1 ||
        config->h264_i_qp > 51 ||
        config->h264_p_qp < 1 ||
        config->h264_p_qp > 51 ||
        config->h264_min_qp < 1 ||
        config->h264_min_qp > 51 ||
        config->h264_max_qp < config->h264_min_qp ||
        config->h264_max_qp > 51) {
        return -EINVAL;
    }

    encoder = calloc(1, sizeof(*encoder));
    if (!encoder)
        return -ENOMEM;
    encoder->fd = -1;
    encoder->visible_width = config->width;
    encoder->visible_height = config->height;
    encoder->output_memory = config->output_dmabuf
                                 ? V4L2_MEMORY_DMABUF
                                 : V4L2_MEMORY_MMAP;

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
    status = request_and_map(
        encoder, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
        encoder->output_memory, config->output_buffers,
        &encoder->output,
        &encoder->output_count);
    if (status < 0)
        goto fail;
    status = request_and_map(
        encoder, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
        V4L2_MEMORY_MMAP, config->capture_buffers,
        &encoder->capture,
        &encoder->capture_count);
    if (status < 0)
        goto fail;

    for (index = 0; index < encoder->capture_count; index++) {
        status = queue_capture(encoder, index);
        if (status < 0)
            goto fail;
    }

    subscribe_eos(encoder);
    status = set_parameters(encoder, config);
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
            .memory = encoder->output_memory,
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
        if (encoder->output[buffer.index].dma_fd >= 0) {
            close(encoder->output[buffer.index].dma_fd);
            encoder->output[buffer.index].dma_fd = -1;
        }
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

int venus_v4l2_encoder_pack_nv12(
    uint8_t *destination, size_t destination_size,
    uint32_t destination_stride, uint32_t destination_scanlines,
    const uint8_t *source, uint32_t width, uint32_t height,
    size_t *packed_size)
{
    size_t source_luma_size;
    size_t destination_luma_size;
    size_t destination_chroma_scanlines;
    size_t required;
    unsigned int row;

    if (!destination || !source || !packed_size ||
        width == 0 || height == 0 ||
        (width & 1u) || (height & 1u) ||
        destination_stride < width ||
        destination_scanlines < height)
        return -EINVAL;
    if (width > SIZE_MAX / height ||
        destination_stride >
            SIZE_MAX / destination_scanlines)
        return -EOVERFLOW;

    source_luma_size = (size_t)width * height;
    destination_luma_size =
        (size_t)destination_stride * destination_scanlines;
    destination_chroma_scanlines =
        ((size_t)height / 2u + 15u) / 16u * 16u;
    if (destination_stride >
            SIZE_MAX / destination_chroma_scanlines ||
        destination_luma_size >
            SIZE_MAX -
                (size_t)destination_stride *
                    destination_chroma_scanlines)
        return -EOVERFLOW;

    required =
        destination_luma_size +
        (size_t)destination_stride *
            destination_chroma_scanlines;
    if (required > destination_size)
        return -ENOSPC;

    memset(destination, 0, required);
    for (row = 0; row < height; row++)
        memcpy(destination +
                   (size_t)row * destination_stride,
               source + (size_t)row * width, width);

    for (row = 0; row < height / 2; row++)
        memcpy(destination + destination_luma_size +
                   (size_t)row * destination_stride,
               source + source_luma_size +
                   (size_t)row * width,
               width);

    *packed_size = required;
    return 0;
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
    size_t expected_size;
    size_t packed_size;
    uint32_t stride;
    uint32_t scanlines;
    int index;
    int status;

    if (!encoder || encoder->output_memory != V4L2_MEMORY_MMAP ||
        !data || size == 0 ||
        encoder->visible_width == 0 ||
        encoder->visible_height == 0 ||
        encoder->visible_width >
            SIZE_MAX / encoder->visible_height)
        return -EINVAL;

    expected_size =
        (size_t)encoder->visible_width *
        encoder->visible_height;
    if (expected_size >
        SIZE_MAX - expected_size / 2)
        return -EOVERFLOW;
    expected_size += expected_size / 2;
    if (size != expected_size)
        return -EINVAL;

    index = find_free_output(encoder, callback, opaque);
    if (index < 0)
        return index;

    stride =
        encoder->output_format.plane_fmt[0].bytesperline;
    if (stride == 0)
        stride = encoder->visible_width;
    if (stride == encoder->visible_width) {
        if (expected_size > encoder->output[index].length)
            return -ENOSPC;
        memcpy(encoder->output[index].data, data, expected_size);
        packed_size = expected_size;
    } else {
        scanlines =
            (encoder->visible_height + 31u) / 32u * 32u;
        status = venus_v4l2_encoder_pack_nv12(
            encoder->output[index].data,
            encoder->output[index].length,
            stride, scanlines, data,
            encoder->visible_width,
            encoder->visible_height, &packed_size);
        if (status < 0)
            return status;
    }
    if (packed_size > UINT32_MAX)
        return -EOVERFLOW;

    buffer.index = (unsigned int)index;
    buffer.field = encoder->output_format.field;
    buffer.timestamp.tv_sec = (long)(tag / 1000000u);
    buffer.timestamp.tv_usec = (long)(tag % 1000000u);
    planes[0].bytesused = (uint32_t)packed_size;
    planes[0].data_offset = 0;
    planes[0].length = (uint32_t)encoder->output[index].length;

    if (xioctl(encoder->fd, VIDIOC_QBUF, &buffer) < 0) {
        int saved_errno = errno;

        snprintf(
            encoder->last_operation,
            sizeof(encoder->last_operation),
            "QBUF_OUT %ux%u stride=%u used=%zu len=%zu",
            encoder->visible_width, encoder->visible_height,
            stride, packed_size, encoder->output[index].length);
        return -saved_errno;
    }

    encoder->output[index].queued = true;

    /*
     * Stateful V4L2 encoders expect the first raw frame to be queued before
     * either queue is started.  This is the ordering used by FFmpeg's
     * v4l2_m2m encoder and GStreamer's v4l2 buffer pool.
     */
    if (!encoder->output_streaming) {
        status = stream_on(
            encoder, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
        if (status < 0)
            return status;
    }
    if (!encoder->capture_streaming) {
        status = stream_on(
            encoder, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
        if (status < 0)
            return status;
    }

    return 0;
}

int venus_v4l2_encoder_submit_dmabuf(
    struct venus_v4l2_encoder *encoder, int dma_fd,
    size_t dma_size, uint32_t stride, uint32_t scanlines,
    size_t uv_offset, uint64_t tag,
    venus_v4l2_packet_callback callback, void *opaque)
{
    struct v4l2_plane planes[VIDEO_MAX_PLANES] = { 0 };
    struct v4l2_buffer buffer = {
        .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
        .memory = V4L2_MEMORY_DMABUF,
        .length = 1,
        .m.planes = planes,
    };
    uint32_t required_size;
    size_t layout_size;
    size_t chroma_scanlines;
    int imported_fd;
    int index;
    int status;

    if (!encoder || encoder->output_memory != V4L2_MEMORY_DMABUF ||
        dma_fd < 0 || dma_size == 0 || dma_size > UINT32_MAX ||
        stride < encoder->visible_width ||
        scanlines < encoder->visible_height ||
        uv_offset != (size_t)stride * scanlines)
        return -EINVAL;

    required_size =
        encoder->output_format.plane_fmt[0].sizeimage;
    chroma_scanlines =
        ((size_t)encoder->visible_height / 2u + 15u) /
        16u * 16u;
    if (stride > SIZE_MAX / chroma_scanlines ||
        uv_offset > SIZE_MAX -
                        (size_t)stride * chroma_scanlines)
        return -EOVERFLOW;
    layout_size =
        uv_offset + (size_t)stride * chroma_scanlines;
    if (required_size == 0 || dma_size < required_size ||
        dma_size < layout_size)
        return -EINVAL;

    index = find_free_output(encoder, callback, opaque);
    if (index < 0)
        return index;

    imported_fd = fcntl(dma_fd, F_DUPFD_CLOEXEC, 0);
    if (imported_fd < 0)
        return -errno;

    buffer.index = (unsigned int)index;
    buffer.field = encoder->output_format.field;
    buffer.timestamp.tv_sec = (long)(tag / 1000000u);
    buffer.timestamp.tv_usec = (long)(tag % 1000000u);
    planes[0].m.fd = imported_fd;
    planes[0].bytesused = required_size;
    planes[0].data_offset = 0;
    planes[0].length = (uint32_t)dma_size;

    if (xioctl(encoder->fd, VIDIOC_QBUF, &buffer) < 0) {
        int saved_errno = errno;

        close(imported_fd);
        snprintf(encoder->last_operation,
                 sizeof(encoder->last_operation),
                 "VIDIOC_QBUF(OUTPUT DMABUF)");
        return -saved_errno;
    }

    encoder->output[index].dma_fd = imported_fd;
    encoder->output[index].queued = true;

    if (!encoder->output_streaming) {
        status = stream_on(
            encoder, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
        if (status < 0)
            return status;
    }
    if (!encoder->capture_streaming) {
        status = stream_on(
            encoder, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
        if (status < 0)
            return status;
    }

    return 0;
}

int venus_v4l2_encoder_force_keyframe(
    struct venus_v4l2_encoder *encoder)
{
    if (!encoder || encoder->fd < 0)
        return -EINVAL;
    return set_control(
        encoder, V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME,
        0, "S_CTRL(FORCE_KEY_FRAME)");
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
        V4L2_MEMORY_MMAP, &encoder->capture,
        &encoder->capture_count);
    release_buffers(
        encoder, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
        encoder->output_memory, &encoder->output,
        &encoder->output_count);

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
