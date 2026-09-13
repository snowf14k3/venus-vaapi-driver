// SPDX-License-Identifier: MIT
#include "backend_internal.h"

#include <errno.h>
#include <limits.h>
#include <linux/v4l2-controls.h>
#include <linux/videodev2.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <va/va_enc_h264.h>

#define VENUS_ENCODE_OUTPUT_BUFFERS 4u
#define VENUS_ENCODE_CAPTURE_BUFFERS 16u
#define VENUS_ENCODE_DEFAULT_BITRATE 1000000u
#define VENUS_ENCODE_DEFAULT_FPS 30u
#define VENUS_ENCODE_DEFAULT_GOP 60u

struct venus_h264_encode_parameters {
    const VAEncSequenceParameterBufferH264 *sequence;
    const VAEncPictureParameterBufferH264 *picture;
    uint32_t bitrate;
    uint32_t frames_per_second;
    bool has_slice;
};

static int64_t monotonic_milliseconds(void)
{
    struct timespec timestamp;

    if (clock_gettime(CLOCK_MONOTONIC, &timestamp) != 0)
        return -1;
    return (int64_t)timestamp.tv_sec * 1000 +
           timestamp.tv_nsec / 1000000;
}

static size_t buffer_bytes(const struct venus_buffer *buffer)
{
    if (!buffer || buffer->element_size >
                       SIZE_MAX / buffer->num_elements)
        return 0;
    return buffer->element_size * buffer->num_elements;
}

static int parse_frame_rate(uint32_t value, uint32_t *result)
{
    uint32_t numerator = value & 0xffffu;
    uint32_t denominator = value >> 16;
    uint32_t rounded;

    if (denominator == 0)
        denominator = 1;
    if (numerator == 0)
        return -EINVAL;

    rounded = (numerator + denominator / 2) / denominator;
    if (rounded == 0 || rounded > 240)
        return -EINVAL;

    *result = rounded;
    return 0;
}

static int parse_misc_parameter(
    const struct venus_buffer *buffer,
    struct venus_h264_encode_parameters *parameters)
{
    const VAEncMiscParameterBuffer *header;
    size_t size = buffer_bytes(buffer);

    if (size < sizeof(*header))
        return -EINVAL;

    header = (const VAEncMiscParameterBuffer *)buffer->data;
    switch (header->type) {
    case VAEncMiscParameterTypeRateControl: {
        const VAEncMiscParameterRateControl *rate_control;

        if (size < sizeof(*header) + sizeof(*rate_control))
            return -EINVAL;
        rate_control =
            (const VAEncMiscParameterRateControl *)header->data;
        if (rate_control->bits_per_second)
            parameters->bitrate = rate_control->bits_per_second;
        return 0;
    }
    case VAEncMiscParameterTypeFrameRate: {
        const VAEncMiscParameterFrameRate *frame_rate;

        if (size < sizeof(*header) + sizeof(*frame_rate))
            return -EINVAL;
        frame_rate =
            (const VAEncMiscParameterFrameRate *)header->data;
        return parse_frame_rate(
            frame_rate->framerate, &parameters->frames_per_second);
    }
    case VAEncMiscParameterTypeHRD:
        return 0;
    default:
        return -ENOTSUP;
    }
}

static int parse_slice_parameters(const struct venus_buffer *buffer)
{
    size_t index;

    if (buffer->element_size <
            sizeof(VAEncSliceParameterBufferH264))
        return -EINVAL;

    for (index = 0; index < buffer->num_elements; index++) {
        const VAEncSliceParameterBufferH264 *slice =
            (const VAEncSliceParameterBufferH264 *)
                (buffer->data + index * buffer->element_size);
        unsigned int slice_type = slice->slice_type % 5u;

        if (slice_type == 1)
            return -ENOTSUP;
        if (slice_type > 2 ||
            slice->num_macroblocks == 0 ||
            slice->macroblock_info != VA_INVALID_ID)
            return -EINVAL;
    }

    return 0;
}

static int collect_parameters(
    struct venus_backend *backend, struct venus_context *context,
    struct venus_h264_encode_parameters *parameters)
{
    size_t index;

    memset(parameters, 0, sizeof(*parameters));

    for (index = 0; index < context->pending_count; index++) {
        struct venus_buffer *buffer = venus_backend_find_buffer(
            backend, context->pending[index]);
        int status;

        if (!buffer || buffer_bytes(buffer) == 0)
            return -EINVAL;

        switch (buffer->type) {
        case VAEncSequenceParameterBufferType:
            if (parameters->sequence ||
                buffer->num_elements != 1 ||
                buffer->element_size <
                    sizeof(VAEncSequenceParameterBufferH264))
                return -EINVAL;
            parameters->sequence =
                (const VAEncSequenceParameterBufferH264 *)buffer->data;
            break;
        case VAEncPictureParameterBufferType:
            if (parameters->picture ||
                buffer->num_elements != 1 ||
                buffer->element_size <
                    sizeof(VAEncPictureParameterBufferH264))
                return -EINVAL;
            parameters->picture =
                (const VAEncPictureParameterBufferH264 *)buffer->data;
            break;
        case VAEncSliceParameterBufferType:
            status = parse_slice_parameters(buffer);
            if (status < 0)
                return status;
            parameters->has_slice = true;
            break;
        case VAEncMiscParameterBufferType:
            status = parse_misc_parameter(buffer, parameters);
            if (status < 0)
                return status;
            break;
        case VAIQMatrixBufferType:
            break;
        default:
            return -ENOTSUP;
        }
    }

    if (!parameters->picture || !parameters->has_slice)
        return -EINVAL;
    return 0;
}

static uint32_t profile_to_v4l2(VAProfile profile)
{
    switch (profile) {
    case VAProfileH264ConstrainedBaseline:
        return V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_BASELINE;
    case VAProfileH264Main:
        return V4L2_MPEG_VIDEO_H264_PROFILE_MAIN;
    case VAProfileH264High:
        return V4L2_MPEG_VIDEO_H264_PROFILE_HIGH;
    default:
        return UINT32_MAX;
    }
}

static int store_packet(const struct venus_v4l2_packet *packet,
                        void *opaque)
{
    struct venus_backend *backend = opaque;
    struct venus_buffer *buffer;
    struct venus_surface *surface;
    size_t capacity;

    if (!packet || packet->tag > UINT32_MAX)
        return -EINVAL;

    buffer = venus_backend_find_buffer(
        backend, (VABufferID)packet->tag);
    if (!buffer || buffer->type != VAEncCodedBufferType)
        return -ENOENT;

    if (buffer->element_size >
        SIZE_MAX / buffer->capacity_elements)
        return -EOVERFLOW;
    capacity =
        buffer->element_size * buffer->capacity_elements;
    if (packet->size > capacity - buffer->coded_size) {
        buffer->coded_segment.status |=
            VA_CODED_BUF_STATUS_FRAME_SIZE_OVERFLOW;
        return -ENOSPC;
    }

    memcpy(buffer->data + buffer->coded_size,
           packet->data, packet->size);
    buffer->coded_size += packet->size;
    buffer->coded_segment.size = (uint32_t)buffer->coded_size;
    buffer->coded_segment.buf = buffer->data;
    buffer->coded_segment.bit_offset = 0;
    buffer->coded_segment.next = NULL;
    buffer->coded_ready = true;

    surface = venus_backend_find_surface(
        backend, buffer->source_surface_id);
    if (surface &&
        surface->coded_buffer_id == buffer->id)
        surface->encode_pending = false;

    venus_backend_log(
        backend,
        "encoded buffer=0x%x surface=0x%x bytes=%zu flags=0x%x",
        buffer->id, buffer->source_surface_id,
        buffer->coded_size, packet->flags);
    return 0;
}

static int open_encoder(
    struct venus_backend *backend, struct venus_context *context,
    const struct venus_config *config,
    const struct venus_h264_encode_parameters *parameters,
    size_t coded_capacity)
{
    struct venus_v4l2_encoder_config encoder_config;
    struct venus_v4l2_error error;
    uint32_t profile = profile_to_v4l2(config->profile);
    uint32_t bitrate = parameters->bitrate;
    uint32_t frames_per_second = parameters->frames_per_second;
    uint32_t gop_size = 0;
    int status;

    if (!parameters->sequence || profile == UINT32_MAX)
        return -EINVAL;

    if (bitrate == 0)
        bitrate = parameters->sequence->bits_per_second;
    if (bitrate == 0)
        bitrate = VENUS_ENCODE_DEFAULT_BITRATE;
    if (bitrate > INT_MAX)
        return -ERANGE;

    if (frames_per_second == 0)
        frames_per_second = VENUS_ENCODE_DEFAULT_FPS;

    gop_size = parameters->sequence->intra_idr_period;
    if (gop_size == 0)
        gop_size = parameters->sequence->intra_period;
    if (gop_size == 0)
        gop_size = VENUS_ENCODE_DEFAULT_GOP;

    encoder_config = (struct venus_v4l2_encoder_config) {
        .device = backend->capabilities.encoder_path,
        .coded_format = V4L2_PIX_FMT_H264,
        .width = context->width,
        .height = context->height,
        .frames_per_second = frames_per_second,
        .bitrate = bitrate,
        .gop_size = gop_size,
        .h264_profile = profile,
        .capture_buffer_size = coded_capacity,
        .output_buffers = VENUS_ENCODE_OUTPUT_BUFFERS,
        .capture_buffers = VENUS_ENCODE_CAPTURE_BUFFERS,
    };

    status = venus_v4l2_encoder_open(
        &encoder_config, &context->encoder, &error);
    if (status < 0) {
        venus_backend_log(
            backend,
            "encoder-open failed operation=%s error=%d",
            error.operation[0] ? error.operation : "none",
            -status);
        return status;
    }

    venus_backend_log(
        backend,
        "encoder-open context=0x%x profile=%d size=%ux%u fps=%u bitrate=%u gop=%u output=%u capture=%u",
        context->id, config->profile,
        context->width, context->height,
        frames_per_second, bitrate, gop_size,
        venus_v4l2_encoder_output_count(context->encoder),
        venus_v4l2_encoder_capture_count(context->encoder));
    return 0;
}

VAStatus venus_encode_end_picture_locked(
    struct venus_backend *backend, struct venus_context *context,
    const struct venus_config *config)
{
    struct venus_h264_encode_parameters parameters;
    struct venus_buffer *coded;
    struct venus_surface *surface;
    size_t coded_capacity;
    size_t expected_frame_size;
    int status;

    status = collect_parameters(backend, context, &parameters);
    if (status < 0)
        return venus_backend_encode_status_from_errno(status);

    coded = venus_backend_find_buffer(
        backend, parameters.picture->coded_buf);
    surface = venus_backend_find_surface(backend, context->target);
    if (!coded || coded->context_id != context->id ||
        coded->type != VAEncCodedBufferType)
        return VA_STATUS_ERROR_INVALID_BUFFER;
    if (!surface || !surface->ready)
        return VA_STATUS_ERROR_INVALID_SURFACE;

    if (coded->element_size >
        SIZE_MAX / coded->capacity_elements)
        return VA_STATUS_ERROR_INVALID_BUFFER;
    coded_capacity =
        coded->element_size * coded->capacity_elements;
    if (coded_capacity == 0 || coded_capacity > UINT32_MAX)
        return VA_STATUS_ERROR_INVALID_BUFFER;

    if (context->width > SIZE_MAX / context->height)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    expected_frame_size =
        (size_t)context->width * context->height;
    if (expected_frame_size > SIZE_MAX / 3 * 2)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    expected_frame_size += expected_frame_size / 2;
    if (surface->data_size < expected_frame_size)
        return VA_STATUS_ERROR_INVALID_SURFACE;

    if (!context->encoder) {
        status = open_encoder(
            backend, context, config, &parameters,
            coded_capacity);
        if (status < 0)
            return venus_backend_encode_status_from_errno(status);
    }

    coded->coded_size = 0;
    coded->coded_ready = false;
    coded->source_surface_id = surface->id;
    memset(&coded->coded_segment, 0,
           sizeof(coded->coded_segment));
    coded->coded_segment.buf = coded->data;

    surface->encode_pending = true;
    surface->coded_buffer_id = coded->id;

    status = venus_v4l2_encoder_submit(
        context->encoder, surface->data,
        expected_frame_size, coded->id,
        store_packet, backend);
    if (status < 0) {
        surface->encode_pending = false;
        venus_backend_log(
            backend,
            "encoder-submit failed context=0x%x operation=%s error=%d",
            context->id,
            venus_v4l2_encoder_last_operation(
                context->encoder),
            -status);
        return venus_backend_encode_status_from_errno(status);
    }

    venus_backend_log(
        backend,
        "encode-submit context=0x%x surface=0x%x coded=0x%x bytes=%zu",
        context->id, surface->id, coded->id,
        expected_frame_size);
    return VA_STATUS_SUCCESS;
}

VAStatus venus_encode_sync_buffer_locked(
    struct venus_backend *backend, struct venus_buffer *buffer,
    int timeout_ms)
{
    struct venus_context *context;
    int64_t deadline;

    if (!buffer || buffer->type != VAEncCodedBufferType)
        return VA_STATUS_ERROR_INVALID_BUFFER;
    if (buffer->coded_ready)
        return VA_STATUS_SUCCESS;

    context = venus_backend_find_context(
        backend, buffer->context_id);
    if (!context || !context->encoder)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    deadline = monotonic_milliseconds() + timeout_ms;
    while (!buffer->coded_ready &&
           monotonic_milliseconds() < deadline) {
        int64_t remaining =
            deadline - monotonic_milliseconds();
        int wait_ms;
        int status;

        if (remaining <= 0)
            break;
        wait_ms = remaining > 1000 ? 1000 : (int)remaining;
        status = venus_v4l2_encoder_pump(
            context->encoder, wait_ms,
            store_packet, backend, NULL);
        if (status == -ETIMEDOUT || status == -EAGAIN)
            continue;
        if (status < 0) {
            venus_backend_log(
                backend,
                "encoder-pump failed context=0x%x operation=%s error=%d",
                context->id,
                venus_v4l2_encoder_last_operation(
                    context->encoder),
                -status);
            return venus_backend_encode_status_from_errno(
                status);
        }
    }

    return buffer->coded_ready
               ? VA_STATUS_SUCCESS
               : VA_STATUS_ERROR_HW_BUSY;
}

VAStatus venus_encode_sync_surface_locked(
    struct venus_backend *backend, struct venus_surface *surface,
    int timeout_ms)
{
    struct venus_buffer *buffer;

    if (!surface->encode_pending)
        return surface->ready
                   ? VA_STATUS_SUCCESS
                   : VA_STATUS_ERROR_SURFACE_BUSY;

    buffer = venus_backend_find_buffer(
        backend, surface->coded_buffer_id);
    if (!buffer)
        return VA_STATUS_ERROR_INVALID_BUFFER;

    return venus_encode_sync_buffer_locked(
        backend, buffer, timeout_ms);
}

void venus_encode_close_context(struct venus_context *context)
{
    if (!context)
        return;

    venus_v4l2_encoder_close(context->encoder);
    context->encoder = NULL;
}
