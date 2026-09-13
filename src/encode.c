// SPDX-License-Identifier: MIT
#include "backend_internal.h"

#include <errno.h>
#include <limits.h>
#include <linux/v4l2-controls.h>
#include <linux/videodev2.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
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
    int8_t slice_qp_delta;
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
    case VAEncMiscParameterTypeQualityLevel:
    case VAEncMiscParameterTypeSubMbPartPel:
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
    const VAEncPackedHeaderParameterBuffer *packed = NULL;
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
            if (!parameters->has_slice) {
                const VAEncSliceParameterBufferH264 *slice =
                    (const VAEncSliceParameterBufferH264 *)
                        buffer->data;

                parameters->slice_qp_delta =
                    slice->slice_qp_delta;
            }
            parameters->has_slice = true;
            break;
        case VAEncPackedHeaderParameterBufferType:
            if (packed || buffer->num_elements != 1 ||
                buffer->element_size <
                    sizeof(VAEncPackedHeaderParameterBuffer))
                return -EINVAL;
            packed =
                (const VAEncPackedHeaderParameterBuffer *)
                    buffer->data;
            if (!(packed->type &
                  VENUS_H264_PACKED_HEADERS))
                return -ENOTSUP;
            break;
        case VAEncPackedHeaderDataBufferType:
            if (!packed ||
                packed->bit_length > buffer_bytes(buffer) * 8)
                return -EINVAL;
            packed = NULL;
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

    if (!parameters->picture || !parameters->has_slice ||
        packed)
        return -EINVAL;
    return 0;
}

int venus_encode_h264_dimensions(
    const VAEncSequenceParameterBufferH264 *sequence,
    uint32_t *width, uint32_t *height)
{
    uint32_t coded_width;
    uint32_t coded_height;
    uint32_t crop_width = 0;
    uint32_t crop_height = 0;

    if (!sequence || !width || !height ||
        sequence->picture_width_in_mbs == 0 ||
        sequence->picture_height_in_mbs == 0)
        return -EINVAL;
    if (sequence->seq_fields.bits.chroma_format_idc != 1 ||
        !sequence->seq_fields.bits.frame_mbs_only_flag ||
        sequence->bit_depth_luma_minus8 != 0 ||
        sequence->bit_depth_chroma_minus8 != 0)
        return -ENOTSUP;

    coded_width =
        (uint32_t)sequence->picture_width_in_mbs * 16u;
    coded_height =
        (uint32_t)sequence->picture_height_in_mbs * 16u;
    if ((uint32_t)sequence->picture_width_in_mbs *
            sequence->picture_height_in_mbs >
        VENUS_H264_MAX_MACROBLOCKS)
        return -EINVAL;

    if (sequence->frame_cropping_flag) {
        uint64_t horizontal =
            (uint64_t)sequence->frame_crop_left_offset +
            sequence->frame_crop_right_offset;
        uint64_t vertical =
            (uint64_t)sequence->frame_crop_top_offset +
            sequence->frame_crop_bottom_offset;

        horizontal *= 2u;
        vertical *= 2u;
        if (horizontal >= coded_width ||
            vertical >= coded_height)
            return -EINVAL;
        crop_width = (uint32_t)horizontal;
        crop_height = (uint32_t)vertical;
    }

    *width = coded_width - crop_width;
    *height = coded_height - crop_height;
    if (*width < VENUS_MIN_WIDTH ||
        *height < VENUS_MIN_HEIGHT ||
        *width > VENUS_MAX_WIDTH ||
        *height > VENUS_MAX_HEIGHT ||
        (*width & 1u) || (*height & 1u))
        return -EINVAL;
    return 0;
}

int venus_encode_apply_native_mode(
    const char *native_mode, uint32_t context_width,
    uint32_t context_height, uint32_t *width,
    uint32_t *height)
{
    unsigned int native_width;
    unsigned int native_height;
    char trailing;

    if (!width || !height)
        return -EINVAL;
    if (!native_mode || !native_mode[0])
        return 0;
    if (sscanf(native_mode, "%ux%u%c", &native_width,
               &native_height, &trailing) != 2 ||
        native_width < VENUS_MIN_WIDTH ||
        native_height < VENUS_MIN_HEIGHT ||
        native_width > VENUS_MAX_WIDTH ||
        native_height > VENUS_MAX_HEIGHT ||
        (native_width & 1u) || (native_height & 1u))
        return -EINVAL;

    if ((native_width + 15u) / 16u * 16u == context_width &&
        (native_height + 15u) / 16u * 16u == context_height) {
        *width = native_width;
        *height = native_height;
        return 1;
    }
    if ((native_height + 15u) / 16u * 16u == context_width &&
        (native_width + 15u) / 16u * 16u == context_height) {
        *width = native_height;
        *height = native_width;
        return 1;
    }
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

static uint32_t level_to_v4l2(uint8_t level_idc)
{
    switch (level_idc) {
    case 9:
        return V4L2_MPEG_VIDEO_H264_LEVEL_1B;
    case 10:
        return V4L2_MPEG_VIDEO_H264_LEVEL_1_0;
    case 11:
        return V4L2_MPEG_VIDEO_H264_LEVEL_1_1;
    case 12:
        return V4L2_MPEG_VIDEO_H264_LEVEL_1_2;
    case 13:
        return V4L2_MPEG_VIDEO_H264_LEVEL_1_3;
    case 20:
        return V4L2_MPEG_VIDEO_H264_LEVEL_2_0;
    case 21:
        return V4L2_MPEG_VIDEO_H264_LEVEL_2_1;
    case 22:
        return V4L2_MPEG_VIDEO_H264_LEVEL_2_2;
    case 30:
        return V4L2_MPEG_VIDEO_H264_LEVEL_3_0;
    case 31:
        return V4L2_MPEG_VIDEO_H264_LEVEL_3_1;
    case 32:
        return V4L2_MPEG_VIDEO_H264_LEVEL_3_2;
    case 40:
        return V4L2_MPEG_VIDEO_H264_LEVEL_4_0;
    case 41:
        return V4L2_MPEG_VIDEO_H264_LEVEL_4_1;
    case 42:
        return V4L2_MPEG_VIDEO_H264_LEVEL_4_2;
    case 50:
        return V4L2_MPEG_VIDEO_H264_LEVEL_5_0;
    case 51:
        return V4L2_MPEG_VIDEO_H264_LEVEL_5_1;
    case 52:
        return V4L2_MPEG_VIDEO_H264_LEVEL_5_2;
    default:
        return UINT32_MAX;
    }
}

int venus_encode_queue_coded_buffer_locked(
    struct venus_context *context, VABufferID buffer_id)
{
    size_t tail;

    if (context->encode_queue_count >= VENUS_MAX_SURFACES)
        return -ENOSPC;

    tail = (context->encode_queue_head +
            context->encode_queue_count) %
           VENUS_MAX_SURFACES;
    context->encode_queue[tail] = buffer_id;
    context->encode_queue_count++;
    return 0;
}

static void rollback_coded_buffer(struct venus_context *context,
                                  VABufferID buffer_id)
{
    size_t tail;

    if (context->encode_queue_count == 0)
        return;

    tail = (context->encode_queue_head +
            context->encode_queue_count - 1) %
           VENUS_MAX_SURFACES;
    if (context->encode_queue[tail] == buffer_id)
        context->encode_queue_count--;
}

int venus_encode_store_packet_locked(
    struct venus_context *context,
    const struct venus_v4l2_packet *packet)
{
    struct venus_backend *backend;
    struct venus_buffer *buffer;
    struct venus_surface *surface;
    VABufferID buffer_id;
    size_t capacity;

    if (!packet || !context || !context->backend ||
        context->encode_queue_count == 0)
        return -EINVAL;

    backend = context->backend;
    buffer_id =
        context->encode_queue[context->encode_queue_head];
    buffer = venus_backend_find_buffer(backend, buffer_id);
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

    context->encode_queue_head =
        (context->encode_queue_head + 1) %
        VENUS_MAX_SURFACES;
    context->encode_queue_count--;

    surface = venus_backend_find_surface(
        backend, buffer->source_surface_id);
    if (surface &&
        surface->coded_buffer_id == buffer->id)
        surface->encode_pending = false;

    venus_backend_log(
        backend,
        "encoded buffer=0x%x surface=0x%x bytes=%zu flags=0x%x driver-tag=0x%llx queued=%zu",
        buffer->id, buffer->source_surface_id,
        buffer->coded_size, packet->flags,
        (unsigned long long)packet->tag,
        context->encode_queue_count);
    return 0;
}

static int store_packet(const struct venus_v4l2_packet *packet,
                        void *opaque)
{
    return venus_encode_store_packet_locked(opaque, packet);
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
    uint32_t level;
    uint32_t bitrate = parameters->bitrate;
    bool bitrate_supplied;
    uint32_t frames_per_second = parameters->frames_per_second;
    uint32_t encode_width;
    uint32_t encode_height;
    uint32_t bitrate_mode;
    int32_t qp;
    uint32_t gop_size = 0;
    int status;

    if (!parameters->sequence || profile == UINT32_MAX)
        return -EINVAL;

    if (level_to_v4l2(parameters->sequence->level_idc) == UINT32_MAX)
        return -ENOTSUP;
    /*
     * Patch 0029 in the Raphael kernel uses the V4L2 Level 1.0 default as
     * an IRIS1 firmware-auto trigger whenever the active frame exceeds
     * Level 1 limits.  SM8150 downstream follows the same auto-level
     * contract.  Validate the VA client's requested level, then let
     * Venus choose the active wire level.
     */
    level = V4L2_MPEG_VIDEO_H264_LEVEL_1_0;

    status = venus_encode_h264_dimensions(
        parameters->sequence, &encode_width, &encode_height);
    if (status < 0)
        return status;
    status = venus_encode_apply_native_mode(
        getenv("VENUS_VAAPI_NATIVE_MODE"),
        context->width, context->height,
        &encode_width, &encode_height);
    if (status < 0)
        return status;
    if (status > 0)
        venus_backend_log(
            backend,
            "native-mode context=%ux%u visible=%ux%u",
            context->width, context->height,
            encode_width, encode_height);
    if ((encode_width + 15u) / 16u * 16u != context->width ||
        (encode_height + 15u) / 16u * 16u != context->height)
        return -EINVAL;

    if (bitrate == 0)
        bitrate = parameters->sequence->bits_per_second;
    bitrate_supplied = bitrate != 0;
    if (bitrate == 0)
        bitrate = VENUS_ENCODE_DEFAULT_BITRATE;

    if (frames_per_second == 0 &&
        parameters->sequence->
            vui_fields.bits.timing_info_present_flag &&
        parameters->sequence->num_units_in_tick > 0) {
        uint64_t denominator =
            (uint64_t)parameters->sequence->
                num_units_in_tick * 2u;
        uint64_t derived =
            parameters->sequence->time_scale /
            denominator;

        if (derived > 0 && derived <= 240)
            frames_per_second = (uint32_t)derived;
    }
    if (frames_per_second == 0)
        frames_per_second = VENUS_ENCODE_DEFAULT_FPS;

    if (config->rate_control == VA_RC_CQP &&
        !bitrate_supplied) {
        uint64_t suggested =
            (uint64_t)encode_width * encode_height *
            frames_per_second / 12u;

        if (suggested < VENUS_ENCODE_DEFAULT_BITRATE)
            suggested = VENUS_ENCODE_DEFAULT_BITRATE;
        if (suggested > 120000000u)
            suggested = 120000000u;
        bitrate = (uint32_t)suggested;
    }
    if (bitrate > INT_MAX)
        return -ERANGE;

    if ((context->width / 16u) *
            (context->height / 16u) >
        VENUS_H264_MAX_MACROBLOCKS_PER_SECOND /
            frames_per_second)
        return -EINVAL;

    gop_size = parameters->sequence->intra_idr_period;
    if (gop_size == 0)
        gop_size = parameters->sequence->intra_period;
    if (gop_size == 0)
        gop_size = VENUS_ENCODE_DEFAULT_GOP;

    qp = (int32_t)parameters->picture->pic_init_qp +
         parameters->slice_qp_delta;
    if (qp < 1 || qp > 51)
        return -EINVAL;

    /*
     * IRIS1 rejects H.264 sessions with frame rate control disabled
     * when the first OUTPUT buffer is queued.  Translate VA CQP to
     * the validated Venus CBR path while preserving the requested QP
     * in VA metadata.
     */
    bitrate_mode = V4L2_MPEG_VIDEO_BITRATE_MODE_CBR;

    context->encode_width = encode_width;
    context->encode_height = encode_height;

    encoder_config = (struct venus_v4l2_encoder_config) {
        .device = backend->capabilities.encoder_path,
        .coded_format = V4L2_PIX_FMT_H264,
        .width = encode_width,
        .height = encode_height,
        .frames_per_second = frames_per_second,
        .bitrate = bitrate,
        .rate_control_enabled = true,
        .bitrate_mode = bitrate_mode,
        .h264_i_qp = (uint32_t)qp,
        .h264_p_qp = (uint32_t)qp,
        .gop_size = gop_size,
        .h264_profile = profile,
        .h264_level = level,
        .h264_entropy_mode =
            parameters->picture->
                pic_fields.bits.entropy_coding_mode_flag
                ? V4L2_MPEG_VIDEO_H264_ENTROPY_MODE_CABAC
                : V4L2_MPEG_VIDEO_H264_ENTROPY_MODE_CAVLC,
        .h264_transform_8x8 =
            parameters->picture->
                pic_fields.bits.transform_8x8_mode_flag,
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
        "encoder-open context=0x%x profile=%d requested-level=%u v4l2-level=%u(auto) size=%ux%u fps=%u rc=0x%x bitrate=%u qp=%d gop=%u output=%u capture=%u",
        context->id, config->profile,
        parameters->sequence->level_idc, level,
        context->encode_width, context->encode_height,
        frames_per_second, config->rate_control,
        bitrate, qp, gop_size,
        venus_v4l2_encoder_output_count(context->encoder),
        venus_v4l2_encoder_capture_count(context->encoder));
    return 0;
}

static int prepare_surface_frame(
    const struct venus_surface *surface,
    unsigned int width, unsigned int height,
    const uint8_t **frame_data, uint8_t **allocated,
    size_t *frame_size)
{
    uint32_t source_stride;
    size_t source_uv_offset;
    size_t source_required;
    size_t destination_pixels;
    unsigned int row;

    *frame_data = NULL;
    *allocated = NULL;
    *frame_size = 0;

    if (!surface || width == 0 || height == 0 ||
        width > surface->width || height > surface->height ||
        width > SIZE_MAX / height)
        return -EINVAL;

    source_stride =
        surface->stride ? surface->stride : surface->width;
    source_uv_offset =
        surface->uv_offset
            ? surface->uv_offset
            : (size_t)surface->width * surface->height;
    if (source_stride < surface->width ||
        source_stride > SIZE_MAX / (height / 2))
        return -EINVAL;
    source_required =
        source_uv_offset +
        (size_t)source_stride * (height / 2);
    destination_pixels = (size_t)width * height;
    if (surface->data_size < source_required ||
        destination_pixels >
            SIZE_MAX - destination_pixels / 2)
        return -EINVAL;

    *frame_size =
        destination_pixels + destination_pixels / 2;
    if (width == surface->width &&
        height == surface->height &&
        source_stride == width &&
        source_uv_offset == destination_pixels) {
        *frame_data = surface->data;
        return 0;
    }

    *allocated = malloc(*frame_size);
    if (!*allocated)
        return -ENOMEM;

    for (row = 0; row < height; row++)
        memcpy(*allocated + (size_t)row * width,
               surface->data +
                   (size_t)row * source_stride,
               width);

    for (row = 0; row < height / 2; row++)
        memcpy(*allocated + destination_pixels +
                   (size_t)row * width,
               surface->data + source_uv_offset +
                   (size_t)row * source_stride,
               width);

    *frame_data = *allocated;
    return 0;
}

VAStatus venus_encode_end_picture_locked(
    struct venus_backend *backend, struct venus_context *context,
    const struct venus_config *config)
{
    struct venus_h264_encode_parameters parameters;
    struct venus_buffer *coded;
    struct venus_surface *surface;
    const uint8_t *frame_data;
    uint8_t *allocated_frame;
    size_t coded_capacity;
    size_t expected_frame_size;
    uint64_t frame_tag;
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

    if (!context->encoder) {
        status = open_encoder(
            backend, context, config, &parameters,
            coded_capacity);
        if (status < 0)
            return venus_backend_encode_status_from_errno(status);
    } else if (parameters.sequence) {
        uint32_t width;
        uint32_t height;

        status = venus_encode_h264_dimensions(
            parameters.sequence, &width, &height);
        if (status < 0 ||
            width != context->encode_width ||
            height != context->encode_height)
            return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    status = venus_surface_begin_cpu_read(surface);
    if (status < 0)
        return venus_backend_encode_status_from_errno(status);

    status = prepare_surface_frame(
        surface, context->encode_width,
        context->encode_height, &frame_data,
        &allocated_frame, &expected_frame_size);
    if (status < 0) {
        venus_surface_end_cpu_read(surface);
        return venus_backend_encode_status_from_errno(status);
    }

    coded->coded_size = 0;
    coded->coded_ready = false;
    coded->source_surface_id = surface->id;
    memset(&coded->coded_segment, 0,
           sizeof(coded->coded_segment));
    coded->coded_segment.buf = coded->data;

    status = venus_encode_queue_coded_buffer_locked(
        context, coded->id);
    if (status < 0) {
        free(allocated_frame);
        venus_surface_end_cpu_read(surface);
        return venus_backend_encode_status_from_errno(status);
    }

    surface->encode_pending = true;
    surface->coded_buffer_id = coded->id;

    context->encode_sequence++;
    if (context->encode_sequence == 0)
        context->encode_sequence++;
    frame_tag = context->encode_sequence;

    status = venus_v4l2_encoder_submit(
        context->encoder, frame_data,
        expected_frame_size, frame_tag,
        store_packet, context);
    free(allocated_frame);
    {
        int sync_status =
            venus_surface_end_cpu_read(surface);

        if (sync_status < 0)
            venus_backend_log(
                backend,
                "surface CPU sync end failed surface=0x%x error=%d",
                surface->id, -sync_status);
    }
    if (status < 0) {
        rollback_coded_buffer(context, coded->id);
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
        "encode-submit context=0x%x surface=0x%x coded=0x%x frame=%llu bytes=%zu",
        context->id, surface->id, coded->id,
        (unsigned long long)frame_tag,
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
            store_packet, context, NULL);
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
    context->encode_queue_head = 0;
    context->encode_queue_count = 0;
}
