// SPDX-License-Identifier: MIT
#include "backend_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/v4l2-controls.h>
#include <linux/videodev2.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <va/va_enc_h264.h>

#define VENUS_ENCODE_OUTPUT_BUFFERS 4u
#define VENUS_ENCODE_CAPTURE_BUFFERS 16u
#define VENUS_ENCODE_DEFAULT_BITRATE 1000000u
#define VENUS_ENCODE_MAX_BITRATE 160000000u
#define VENUS_ENCODE_DEFAULT_FPS 30u

struct venus_h264_encode_parameters {
    const VAEncSequenceParameterBufferH264 *sequence;
    const VAEncPictureParameterBufferH264 *picture;
    uint32_t bitrate;
    uint32_t frames_per_second;
    int8_t slice_qp_delta;
    bool has_slice;
    bool has_aud;
};

struct venus_hevc_encode_parameters {
    const VAEncSequenceParameterBufferHEVC *sequence;
    const VAEncPictureParameterBufferHEVC *picture;
    uint32_t bitrate;
    uint32_t frames_per_second;
    int8_t slice_qp_delta;
    uint32_t num_ctu_in_slice;
    bool has_slice;
    bool intra;
    bool has_aud;
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

static bool environment_flag_enabled(const char *name)
{
    const char *value = getenv(name);

    return value && value[0] && strcmp(value, "0") != 0;
}

static void sample_input_luma(const struct venus_surface *surface,
                              unsigned int width, unsigned int height,
                              struct venus_buffer *coded)
{
    uint8_t minimum = UINT8_MAX;
    uint8_t maximum = 0;
    uint32_t bright = 0;
    unsigned int row;
    unsigned int column;

    if (!surface->data || !surface->stride || !width || !height)
        return;

    for (row = 0; row < 36; row++) {
        size_t y = (size_t)row * height / 36u;

        for (column = 0; column < 64; column++) {
            size_t x = (size_t)column * width / 64u;
            size_t offset = y * surface->stride + x;
            uint8_t value;

            if (offset >= surface->data_size)
                return;
            value = surface->data[offset];
            if (value < minimum)
                minimum = value;
            if (value > maximum)
                maximum = value;
            if (value > 32)
                bright++;
        }
    }

    coded->input_sample_valid = true;
    coded->input_sample_min = minimum;
    coded->input_sample_max = maximum;
    coded->input_sample_bright = bright;
}

static void dump_encoded_packet(
    const struct venus_v4l2_packet *packet)
{
    const char *path = getenv("VENUS_VAAPI_DUMP_H264");
    size_t offset = 0;
    int fd;

    if (!path || !path[0] || !packet || !packet->data ||
        packet->size == 0)
        return;

    fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC,
              0600);
    if (fd < 0)
        return;

    while (offset < packet->size) {
        ssize_t written =
            write(fd, packet->data + offset,
                  packet->size - offset);

        if (written < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (written == 0)
            break;
        offset += (size_t)written;
    }
    close(fd);
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
    uint32_t *bitrate, uint32_t *frames_per_second)
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
            *bitrate = rate_control->bits_per_second;
        return 0;
    }
    case VAEncMiscParameterTypeFrameRate: {
        const VAEncMiscParameterFrameRate *frame_rate;

        if (size < sizeof(*header) + sizeof(*frame_rate))
            return -EINVAL;
        frame_rate =
            (const VAEncMiscParameterFrameRate *)header->data;
        return parse_frame_rate(
            frame_rate->framerate, frames_per_second);
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
            if (packed->type == VAEncPackedHeaderRawData)
                parameters->has_aud = true;
            packed = NULL;
            break;
        case VAEncMiscParameterBufferType:
            status = parse_misc_parameter(
                buffer, &parameters->bitrate,
                &parameters->frames_per_second);
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

static int collect_hevc_parameters(
    struct venus_backend *backend, struct venus_context *context,
    struct venus_hevc_encode_parameters *parameters)
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
            if (parameters->sequence || buffer->num_elements != 1 ||
                buffer->element_size <
                    sizeof(VAEncSequenceParameterBufferHEVC))
                return -EINVAL;
            parameters->sequence =
                (const VAEncSequenceParameterBufferHEVC *)buffer->data;
            break;
        case VAEncPictureParameterBufferType:
            if (parameters->picture || buffer->num_elements != 1 ||
                buffer->element_size <
                    sizeof(VAEncPictureParameterBufferHEVC))
                return -EINVAL;
            parameters->picture =
                (const VAEncPictureParameterBufferHEVC *)buffer->data;
            break;
        case VAEncSliceParameterBufferType: {
            const VAEncSliceParameterBufferHEVC *slice;

            if (parameters->has_slice || buffer->num_elements != 1 ||
                buffer->element_size <
                    sizeof(VAEncSliceParameterBufferHEVC))
                return -EINVAL;
            slice = (const VAEncSliceParameterBufferHEVC *)buffer->data;
            if (slice->slice_segment_address != 0 ||
                slice->num_ctu_in_slice == 0 ||
                (slice->slice_type != 1 && slice->slice_type != 2))
                return -ENOTSUP;
            parameters->slice_qp_delta = slice->slice_qp_delta;
            parameters->num_ctu_in_slice = slice->num_ctu_in_slice;
            parameters->intra = slice->slice_type == 2;
            parameters->has_slice = true;
            break;
        }
        case VAEncPackedHeaderParameterBufferType:
            if (packed || buffer->num_elements != 1 ||
                buffer->element_size <
                    sizeof(VAEncPackedHeaderParameterBuffer))
                return -EINVAL;
            packed =
                (const VAEncPackedHeaderParameterBuffer *)buffer->data;
            if (!(packed->type & VENUS_H264_PACKED_HEADERS))
                return -ENOTSUP;
            break;
        case VAEncPackedHeaderDataBufferType:
            if (!packed ||
                packed->bit_length > buffer_bytes(buffer) * 8)
                return -EINVAL;
            if (packed->type == VAEncPackedHeaderRawData)
                parameters->has_aud = true;
            packed = NULL;
            break;
        case VAEncMiscParameterBufferType:
            status = parse_misc_parameter(
                buffer, &parameters->bitrate,
                &parameters->frames_per_second);
            if (status < 0)
                return status;
            break;
        case VAIQMatrixBufferType:
            break;
        default:
            return -ENOTSUP;
        }
    }

    if (!parameters->picture || !parameters->has_slice || packed)
        return -EINVAL;
    if (parameters->picture->pic_fields.bits.idr_pic_flag &&
        !parameters->intra)
        return -ENOTSUP;
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

int venus_encode_cqp_bitrate(
    uint32_t width, uint32_t height, uint32_t frames_per_second,
    uint32_t qp, uint32_t *bitrate)
{
    uint64_t pixels;
    uint64_t samples_per_second;
    uint64_t suggested;
    uint32_t quality_weight;

    if (!width || !height || !frames_per_second ||
        qp < 1 || qp > 51 || !bitrate)
        return -EINVAL;
    if ((uint64_t)width > UINT64_MAX / height)
        return -EOVERFLOW;
    pixels = (uint64_t)width * height;
    if (pixels > UINT64_MAX / frames_per_second)
        return -EOVERFLOW;
    samples_per_second = pixels * frames_per_second;
    quality_weight = 52u - qp;
    if (samples_per_second > UINT64_MAX / quality_weight)
        return -EOVERFLOW;

    /*
     * IRIS1 cannot run H.264 with frame RC disabled, so VA CQP is carried
     * through CBR.  Scale the compatibility bitrate with both pixel rate
     * and requested QP.  QP 22 receives one third of a bit per pixel at
     * full frame rate, which is suitable for detailed desktop content.
     */
    suggested =
        samples_per_second * quality_weight / 90u;
    if (suggested < VENUS_ENCODE_DEFAULT_BITRATE)
        suggested = VENUS_ENCODE_DEFAULT_BITRATE;
    if (suggested > VENUS_ENCODE_MAX_BITRATE)
        suggested = VENUS_ENCODE_MAX_BITRATE;

    *bitrate = (uint32_t)suggested;
    return 0;
}

static int apply_cqp_bitrate_override(uint32_t *bitrate)
{
    const char *value = getenv("VENUS_VAAPI_CQP_BITRATE");
    unsigned long long parsed;
    char *end;

    if (!value || !value[0])
        return 0;

    errno = 0;
    end = NULL;
    parsed = strtoull(value, &end, 10);
    if (errno || end == value || *end != '\0' ||
        parsed < 32000u ||
        parsed > VENUS_ENCODE_MAX_BITRATE)
        return -EINVAL;

    *bitrate = (uint32_t)parsed;
    return 0;
}

static int apply_quality_qp_override(int32_t *qp, bool *overridden)
{
    const char *value = getenv("VENUS_VAAPI_QUALITY_QP");
    unsigned long parsed;
    char *end;

    if (overridden)
        *overridden = false;
    if (!value || !value[0])
        return 0;
    errno = 0;
    end = NULL;
    parsed = strtoul(value, &end, 10);
    if (errno || end == value || *end != '\0' ||
        parsed < 1 || parsed > 51)
        return -EINVAL;
    *qp = (int32_t)parsed;
    if (overridden)
        *overridden = true;
    return 0;
}

static int32_t quality_qp_from_bitrate(
    uint32_t width, uint32_t height, uint32_t frames_per_second,
    uint32_t bitrate, int32_t client_qp)
{
    uint64_t pixel_rate =
        (uint64_t)width * height * frames_per_second;
    uint64_t quality_steps;
    int32_t budget_qp;

    if (!pixel_rate || !bitrate)
        return client_qp;

    /*
     * The IRIS1 VBR path can spend far less than the requested bitrate when
     * left to choose QP freely. Use the stream's bits per pixel per frame
     * to set a quality floor, while retaining the client's better QP if it
     * requests one. Firmware still adjusts within the narrow QP range.
     */
    quality_steps = (uint64_t)bitrate * 16u / pixel_rate;
    if (quality_steps > 14u)
        quality_steps = 14u;
    budget_qp = 32 - (int32_t)quality_steps;
    return client_qp < budget_qp ? client_qp : budget_qp;
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
    struct venus_context *context, VABufferID buffer_id,
    uint64_t tag)
{
    struct venus_buffer *buffer;
    size_t offset;
    size_t tail;

    if (!context || !context->backend || tag == 0)
        return -EINVAL;
    if (context->encode_queue_count >= VENUS_MAX_SURFACES)
        return -ENOSPC;

    buffer = venus_backend_find_buffer(
        context->backend, buffer_id);
    if (!buffer || buffer->type != VAEncCodedBufferType ||
        buffer->context_id != context->id)
        return -ENOENT;

    for (offset = 0; offset < context->encode_queue_count;
         offset++) {
        size_t index =
            (context->encode_queue_head + offset) %
            VENUS_MAX_SURFACES;

        if (context->encode_queue[index] == buffer_id)
            return -EALREADY;
    }

    tail = (context->encode_queue_head +
            context->encode_queue_count) %
           VENUS_MAX_SURFACES;
    context->encode_queue[tail] = buffer_id;
    context->encode_queue_count++;
    buffer->coded_tag = tag;
    return 0;
}

static void remove_encode_queue_entry(
    struct venus_context *context, size_t offset)
{
    size_t index;

    for (index = offset;
         index + 1 < context->encode_queue_count;
         index++) {
        size_t destination =
            (context->encode_queue_head + index) %
            VENUS_MAX_SURFACES;
        size_t source =
            (context->encode_queue_head + index + 1) %
            VENUS_MAX_SURFACES;

        context->encode_queue[destination] =
            context->encode_queue[source];
    }
    context->encode_queue_count--;
}

static void rollback_coded_buffer(struct venus_context *context,
                                  VABufferID buffer_id)
{
    struct venus_buffer *buffer;
    size_t tail;

    if (context->encode_queue_count == 0)
        return;

    tail = (context->encode_queue_head +
            context->encode_queue_count - 1) %
           VENUS_MAX_SURFACES;
    if (context->encode_queue[tail] == buffer_id) {
        context->encode_queue_count--;
        buffer = venus_backend_find_buffer(
            context->backend, buffer_id);
        if (buffer)
            buffer->coded_tag = 0;
    }
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
    size_t offset;
    bool complete;

    if (!packet || !context || !context->backend ||
        packet->tag == 0 ||
        context->encode_queue_count == 0)
        return -EINVAL;

    backend = context->backend;
    dump_encoded_packet(packet);
    buffer = NULL;
    buffer_id = VA_INVALID_ID;
    for (offset = 0; offset < context->encode_queue_count;
         offset++) {
        size_t index =
            (context->encode_queue_head + offset) %
            VENUS_MAX_SURFACES;
        struct venus_buffer *candidate;

        buffer_id = context->encode_queue[index];
        candidate = venus_backend_find_buffer(
            backend, buffer_id);
        if (candidate &&
            candidate->type == VAEncCodedBufferType &&
            candidate->coded_tag == packet->tag) {
            buffer = candidate;
            break;
        }
    }
    if (!buffer)
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
    buffer->coded_packets++;
    buffer->coded_segment.size = (uint32_t)buffer->coded_size;
    buffer->coded_segment.buf = buffer->data;
    buffer->coded_segment.bit_offset = 0;
    buffer->coded_segment.next = NULL;

    if (packet->flags & V4L2_BUF_FLAG_ERROR)
        buffer->coded_segment.status |=
            VA_CODED_BUF_STATUS_BAD_BITSTREAM;

    complete =
        (packet->flags &
         (V4L2_BUF_FLAG_KEYFRAME |
          V4L2_BUF_FLAG_PFRAME |
          V4L2_BUF_FLAG_BFRAME)) != 0;
    if (!complete) {
        venus_backend_log(
            backend,
            "encoded part buffer=0x%x surface=0x%x bytes=%zu packets=%u flags=0x%x driver-tag=0x%llx complete=0 queued=%zu",
            buffer->id, buffer->source_surface_id,
            buffer->coded_size, buffer->coded_packets,
            packet->flags,
            (unsigned long long)packet->tag,
            context->encode_queue_count);
        return 0;
    }

    buffer->coded_ready = true;
    remove_encode_queue_entry(context, offset);
    surface = venus_backend_find_surface(
        backend, buffer->source_surface_id);
    if (surface &&
        surface->coded_buffer_id == buffer->id &&
        buffer->input_done)
        surface->encode_pending = false;

    venus_backend_log(
        backend,
        "encoded buffer=0x%x surface=0x%x bytes=%zu packets=%u flags=0x%x driver-tag=0x%llx complete=1 queued=%zu",
        buffer->id, buffer->source_surface_id,
        buffer->coded_size, buffer->coded_packets,
        packet->flags,
        (unsigned long long)packet->tag,
        context->encode_queue_count);
    if (environment_flag_enabled("VENUS_VAAPI_TRACE_INPUT") &&
        (buffer->coded_tag <= 8 || buffer->coded_size < 6000))
        venus_backend_log(
            backend,
            "input-sample tag=%llu surface=0x%x valid=%u bright=%u/2304 min=%u max=%u coded=%zu",
            (unsigned long long)buffer->coded_tag,
            buffer->source_surface_id,
            buffer->input_sample_valid ? 1u : 0u,
            buffer->input_sample_bright,
            buffer->input_sample_min,
            buffer->input_sample_max,
            buffer->coded_size);
    return 0;
}

static int store_packet(const struct venus_v4l2_packet *packet,
                        void *opaque)
{
    return venus_encode_store_packet_locked(opaque, packet);
}

static int store_output_done(uint64_t tag, void *opaque)
{
    struct venus_context *context = opaque;
    struct venus_backend *backend = context->backend;
    unsigned int index;

    for (index = 0; index < VENUS_MAX_BUFFERS; index++) {
        struct venus_buffer *buffer = &backend->buffers[index];
        struct venus_surface *surface;

        if (!buffer->used ||
            buffer->type != VAEncCodedBufferType ||
            buffer->context_id != context->id ||
            buffer->coded_tag != tag)
            continue;

        buffer->input_done = true;
        surface = venus_backend_find_surface(
            backend, buffer->source_surface_id);
        if (surface &&
            surface->coded_buffer_id == buffer->id &&
            buffer->coded_ready)
            surface->encode_pending = false;
        return 0;
    }

    return -ENOENT;
}

static int open_encoder(
    struct venus_backend *backend, struct venus_context *context,
    const struct venus_config *config,
    const struct venus_h264_encode_parameters *parameters,
    bool output_dmabuf)
{
    struct venus_v4l2_encoder_config encoder_config;
    struct venus_v4l2_error error;
    uint32_t profile = profile_to_v4l2(config->profile);
    uint32_t level;
    uint32_t bitrate = parameters->bitrate;
    bool bitrate_supplied;
    bool cqp_compat;
    bool quality_qp_overridden;
    uint32_t frames_per_second = parameters->frames_per_second;
    uint32_t encode_width;
    uint32_t encode_height;
    uint32_t bitrate_mode;
    uint32_t minimum_qp = 1;
    uint32_t maximum_qp = 51;
    size_t v4l2_capture_size;
    int32_t qp;
    uint32_t gop_size = 0;
    int status;

    /* Allow a synchronized MMAP staging path when diagnosing DMA imports;
     * the normal path imports the exported VA surface into Venus directly. */
    if (environment_flag_enabled("VENUS_VAAPI_STAGING"))
        output_dmabuf = false;

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
    status = venus_v4l2_encoder_compressed_size(
        encode_width, encode_height, &v4l2_capture_size);
    if (status < 0)
        return status;

    if (bitrate == 0)
        bitrate = parameters->sequence->bits_per_second;
    bitrate_supplied = bitrate != 0;

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

    qp = (int32_t)parameters->picture->pic_init_qp +
         parameters->slice_qp_delta;
    if (qp < 1 || qp > 51)
        return -EINVAL;
    status = apply_quality_qp_override(&qp, &quality_qp_overridden);
    if (status < 0)
        return status;

    cqp_compat = config->rate_control == VA_RC_CQP;
    if (!cqp_compat && !quality_qp_overridden)
        qp = quality_qp_from_bitrate(
            encode_width, encode_height, frames_per_second,
            bitrate, qp);
    minimum_qp = qp > 2 ? (uint32_t)qp - 2u : 1u;
    maximum_qp = qp < 49 ? (uint32_t)qp + 2u : 51u;

    if (cqp_compat && !bitrate_supplied) {
        status = venus_encode_cqp_bitrate(
            encode_width, encode_height, frames_per_second,
            (uint32_t)qp, &bitrate);
        if (status < 0)
            return status;
        status = apply_cqp_bitrate_override(&bitrate);
        if (status < 0)
            return status;
    } else if (bitrate == 0) {
        bitrate = VENUS_ENCODE_DEFAULT_BITRATE;
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
        gop_size = INT_MAX;
    if (environment_flag_enabled("VENUS_VAAPI_INTRA_ONLY"))
        gop_size = 1;

    /*
     * IRIS1 rejects H.264 sessions with frame rate control disabled.
     * Keep frame RC enabled for both VA CBR and CQP compatibility, but use
     * the firmware's VBR route.  The CBR route substantially undershoots
     * the requested bitrate on SM8150 and produces visible block damage,
     * while the current Raphael VBR lifecycle is stable at the same sizes.
     */
    bitrate_mode = V4L2_MPEG_VIDEO_BITRATE_MODE_VBR;

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
        .i_qp = (uint32_t)qp,
        .p_qp = (uint32_t)qp,
        .min_qp = minimum_qp,
        .max_qp = maximum_qp,
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
        .aud = parameters->has_aud,
        .output_dmabuf = output_dmabuf,
        .capture_buffer_size = v4l2_capture_size,
        .output_buffers = VENUS_ENCODE_OUTPUT_BUFFERS,
        .capture_buffers = VENUS_ENCODE_CAPTURE_BUFFERS,
        .output_done = store_output_done,
        .output_done_opaque = context,
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
    context->encode_dmabuf = output_dmabuf;

    venus_backend_log(
        backend,
        "encoder-open context=0x%x profile=%d requested-level=%u v4l2-level=%u(auto) size=%ux%u fps=%u rc=0x%x bitrate=%u qp=%d range=%u..%u gop=%u aud=%u input=%s output=%u capture=%u",
        context->id, config->profile,
        parameters->sequence->level_idc, level,
        context->encode_width, context->encode_height,
        frames_per_second, config->rate_control,
        bitrate, qp, minimum_qp, maximum_qp, gop_size,
        parameters->has_aud ? 1u : 0u,
        output_dmabuf ? "dmabuf" : "mmap",
        venus_v4l2_encoder_output_count(context->encoder),
        venus_v4l2_encoder_capture_count(context->encoder));
    return 0;
}

static uint32_t hevc_level_to_v4l2(uint8_t level_idc)
{
    static const struct {
        uint8_t idc;
        uint32_t control;
    } levels[] = {
        { 30, V4L2_MPEG_VIDEO_HEVC_LEVEL_1 },
        { 60, V4L2_MPEG_VIDEO_HEVC_LEVEL_2 },
        { 63, V4L2_MPEG_VIDEO_HEVC_LEVEL_2_1 },
        { 90, V4L2_MPEG_VIDEO_HEVC_LEVEL_3 },
        { 93, V4L2_MPEG_VIDEO_HEVC_LEVEL_3_1 },
        { 120, V4L2_MPEG_VIDEO_HEVC_LEVEL_4 },
        { 123, V4L2_MPEG_VIDEO_HEVC_LEVEL_4_1 },
        { 150, V4L2_MPEG_VIDEO_HEVC_LEVEL_5 },
        { 153, V4L2_MPEG_VIDEO_HEVC_LEVEL_5_1 },
        { 156, V4L2_MPEG_VIDEO_HEVC_LEVEL_5_2 },
        { 180, V4L2_MPEG_VIDEO_HEVC_LEVEL_6 },
        { 183, V4L2_MPEG_VIDEO_HEVC_LEVEL_6_1 },
        { 186, V4L2_MPEG_VIDEO_HEVC_LEVEL_6_2 },
    };
    size_t index;

    for (index = 0; index < sizeof(levels) / sizeof(levels[0]); index++) {
        if (levels[index].idc == level_idc)
            return levels[index].control;
    }
    return UINT32_MAX;
}

static int open_hevc_encoder(
    struct venus_backend *backend, struct venus_context *context,
    const struct venus_config *config,
    const struct venus_hevc_encode_parameters *parameters,
    bool output_dmabuf)
{
    const VAEncSequenceParameterBufferHEVC *sequence =
        parameters->sequence;
    const VAEncPictureParameterBufferHEVC *picture =
        parameters->picture;
    struct venus_v4l2_encoder_config encoder_config;
    struct venus_v4l2_error error;
    uint32_t level;
    uint32_t bitrate = parameters->bitrate;
    bool quality_qp_overridden;
    uint32_t frames_per_second = parameters->frames_per_second;
    uint32_t minimum_qp = 1;
    uint32_t maximum_qp = 51;
    uint32_t gop_size;
    uint32_t min_cb;
    uint32_t ctu;
    uint32_t ctu_count;
    size_t capture_size;
    int32_t qp;
    int status;

    if (environment_flag_enabled("VENUS_VAAPI_STAGING"))
        output_dmabuf = false;
    if (!sequence || !picture || !parameters->intra ||
        !picture->pic_fields.bits.idr_pic_flag ||
        sequence->general_profile_idc != 1 ||
        sequence->seq_fields.bits.chroma_format_idc != 1 ||
        sequence->seq_fields.bits.bit_depth_luma_minus8 != 0 ||
        sequence->seq_fields.bits.bit_depth_chroma_minus8 != 0 ||
        sequence->seq_fields.bits.separate_colour_plane_flag ||
        sequence->seq_fields.bits.pcm_enabled_flag ||
        sequence->ip_period > 1 ||
        picture->num_tile_columns_minus1 ||
        picture->num_tile_rows_minus1 ||
        picture->pic_fields.bits.tiles_enabled_flag)
        return -ENOTSUP;
    level = hevc_level_to_v4l2(sequence->general_level_idc);
    if (level == UINT32_MAX)
        return -ENOTSUP;
    if (sequence->log2_min_luma_coding_block_size_minus3 > 3 ||
        sequence->log2_diff_max_min_luma_coding_block_size > 3 ||
        sequence->log2_min_luma_coding_block_size_minus3 +
            sequence->log2_diff_max_min_luma_coding_block_size > 3)
        return -EINVAL;
    min_cb = 1u <<
        (sequence->log2_min_luma_coding_block_size_minus3 + 3);
    ctu = min_cb <<
        sequence->log2_diff_max_min_luma_coding_block_size;
    if (sequence->pic_width_in_luma_samples < context->width ||
        sequence->pic_height_in_luma_samples < context->height ||
        sequence->pic_width_in_luma_samples - context->width >= 16 ||
        sequence->pic_height_in_luma_samples - context->height >= 16)
        return -EINVAL;
    ctu_count =
        ((sequence->pic_width_in_luma_samples + ctu - 1) / ctu) *
        ((sequence->pic_height_in_luma_samples + ctu - 1) / ctu);
    if (parameters->num_ctu_in_slice != ctu_count)
        return -ENOTSUP;

    status = venus_v4l2_encoder_compressed_size(
        context->width, context->height, &capture_size);
    if (status < 0)
        return status;
    if (bitrate == 0)
        bitrate = sequence->bits_per_second;
    if (frames_per_second == 0 && sequence->vui_num_units_in_tick &&
        sequence->vui_time_scale)
        frames_per_second = sequence->vui_time_scale /
                            sequence->vui_num_units_in_tick;
    if (frames_per_second == 0 || frames_per_second > 240)
        frames_per_second = VENUS_ENCODE_DEFAULT_FPS;
    qp = (int32_t)picture->pic_init_qp +
         parameters->slice_qp_delta;
    if (qp < 1 || qp > 51)
        return -EINVAL;
    status = apply_quality_qp_override(&qp, &quality_qp_overridden);
    if (status < 0)
        return status;
    if (config->rate_control != VA_RC_CQP &&
        !quality_qp_overridden)
        qp = quality_qp_from_bitrate(
            context->width, context->height,
            frames_per_second, bitrate, qp);
    minimum_qp = qp > 2 ? (uint32_t)qp - 2u : 1u;
    maximum_qp = qp < 49 ? (uint32_t)qp + 2u : 51u;
    if (config->rate_control == VA_RC_CQP) {
        if (bitrate == 0) {
            status = venus_encode_cqp_bitrate(
                context->width, context->height,
                frames_per_second, (uint32_t)qp, &bitrate);
            if (status < 0)
                return status;
        }
    }
    if (bitrate == 0)
        bitrate = VENUS_ENCODE_DEFAULT_BITRATE;
    if (bitrate > INT_MAX)
        return -ERANGE;
    gop_size = sequence->intra_idr_period;
    if (gop_size == 0)
        gop_size = sequence->intra_period;
    if (gop_size == 0)
        gop_size = INT_MAX;
    if (environment_flag_enabled("VENUS_VAAPI_INTRA_ONLY"))
        gop_size = 1;

    context->encode_width = context->width;
    context->encode_height = context->height;
    encoder_config = (struct venus_v4l2_encoder_config) {
        .device = backend->capabilities.encoder_path,
        .coded_format = V4L2_PIX_FMT_HEVC,
        .width = context->encode_width,
        .height = context->encode_height,
        .frames_per_second = frames_per_second,
        .bitrate = bitrate,
        .rate_control_enabled = true,
        .bitrate_mode = V4L2_MPEG_VIDEO_BITRATE_MODE_VBR,
        .i_qp = (uint32_t)qp,
        .p_qp = (uint32_t)qp,
        .min_qp = minimum_qp,
        .max_qp = maximum_qp,
        .gop_size = gop_size,
        .hevc_level = level,
        .hevc_tier = sequence->general_tier_flag,
        .aud = parameters->has_aud,
        .output_dmabuf = output_dmabuf,
        .capture_buffer_size = capture_size,
        .output_buffers = VENUS_ENCODE_OUTPUT_BUFFERS,
        .capture_buffers = VENUS_ENCODE_CAPTURE_BUFFERS,
        .output_done = store_output_done,
        .output_done_opaque = context,
    };
    status = venus_v4l2_encoder_open(
        &encoder_config, &context->encoder, &error);
    if (status < 0) {
        venus_backend_log(
            backend,
            "HEVC encoder-open failed operation=%s error=%d",
            error.operation[0] ? error.operation : "none", -status);
        return status;
    }
    context->encode_dmabuf = output_dmabuf;
    venus_backend_log(
        backend,
        "HEVC encoder-open context=0x%x size=%ux%u fps=%u rc=0x%x bitrate=%u qp=%d gop=%u input=%s",
        context->id, context->encode_width,
        context->encode_height, frames_per_second,
        config->rate_control, bitrate, qp, gop_size,
        output_dmabuf ? "dmabuf" : "mmap");
    return 0;
}

VAStatus venus_encode_end_picture_locked(
    struct venus_backend *backend, struct venus_context *context,
    const struct venus_config *config)
{
    struct venus_h264_encode_parameters parameters;
    struct venus_hevc_encode_parameters hevc_parameters;
    struct venus_buffer *coded;
    struct venus_surface *surface;
    VABufferID coded_buffer_id;
    bool hevc = config->profile == VAProfileHEVCMain;
    size_t coded_capacity;
    size_t expected_frame_size;
    uint64_t frame_tag;
    int status;

    status = hevc
                 ? collect_hevc_parameters(
                       backend, context, &hevc_parameters)
                 : collect_parameters(backend, context, &parameters);
    if (status < 0)
        return venus_backend_encode_status_from_errno(status);

    coded_buffer_id = hevc ? hevc_parameters.picture->coded_buf
                           : parameters.picture->coded_buf;
    coded = venus_backend_find_buffer(
        backend, coded_buffer_id);
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
        status = hevc
                     ? open_hevc_encoder(
                           backend, context, config,
                           &hevc_parameters, surface->dma_backed)
                     : open_encoder(
                           backend, context, config, &parameters,
                           surface->dma_backed);
        if (status < 0)
            return venus_backend_encode_status_from_errno(status);
    } else if (hevc && hevc_parameters.sequence) {
        if (hevc_parameters.sequence->pic_width_in_luma_samples !=
                context->encode_width ||
            hevc_parameters.sequence->pic_height_in_luma_samples !=
                context->encode_height)
            return VA_STATUS_ERROR_INVALID_PARAMETER;
    } else if (!hevc && parameters.sequence) {
        uint32_t width;
        uint32_t height;

        status = venus_encode_h264_dimensions(
            parameters.sequence, &width, &height);
        if (status < 0 ||
            width != context->encode_width ||
            height != context->encode_height)
            return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    if (environment_flag_enabled("VENUS_VAAPI_INTRA_ONLY") ||
        (context->encode_sequence > 0 &&
         (hevc ? hevc_parameters.intra
               : parameters.picture->pic_fields.bits.idr_pic_flag))) {
        status = venus_v4l2_encoder_force_keyframe(context->encoder);
        if (status < 0) {
            venus_backend_log(
                backend,
                "force-keyframe failed context=0x%x operation=%s error=%d",
                context->id,
                venus_v4l2_encoder_last_operation(context->encoder),
                -status);
            return venus_backend_encode_status_from_errno(status);
        }
    }

    expected_frame_size = 0;
    if (context->encode_dmabuf) {
        if (!surface->dma_backed || surface->dma_fd < 0)
            return VA_STATUS_ERROR_INVALID_SURFACE;
        status = venus_surface_wait_for_device_read(surface);
        if (status < 0)
            return venus_backend_encode_status_from_errno(status);
        expected_frame_size = surface->capacity;
    } else {
        status = venus_surface_begin_cpu_read(surface);
        if (status < 0)
            return venus_backend_encode_status_from_errno(status);
        expected_frame_size = surface->data_size;
    }

    context->encode_sequence++;
    if (context->encode_sequence == 0)
        context->encode_sequence++;
    frame_tag = context->encode_sequence;

    coded->coded_size = 0;
    coded->coded_ready = false;
    coded->coded_delivered = false;
    coded->input_done = false;
    coded->input_sample_valid = false;
    coded->input_sample_min = 0;
    coded->input_sample_max = 0;
    coded->input_sample_bright = 0;
    coded->coded_packets = 0;
    coded->coded_tag = 0;
    coded->source_surface_id = surface->id;
    memset(&coded->coded_segment, 0,
           sizeof(coded->coded_segment));
    coded->coded_segment.buf = coded->data;

    if (environment_flag_enabled("VENUS_VAAPI_TRACE_INPUT")) {
        int sample_status = context->encode_dmabuf
                                ? venus_surface_begin_cpu_read(surface)
                                : 0;

        if (sample_status == 0) {
            sample_input_luma(
                surface, context->encode_width,
                context->encode_height, coded);
            if (context->encode_dmabuf)
                sample_status =
                    venus_surface_end_cpu_read(surface);
        }
        if (sample_status < 0)
            venus_backend_log(
                backend,
                "input-sample sync failed tag=%llu error=%d",
                (unsigned long long)frame_tag,
                -sample_status);
    }

    status = venus_encode_queue_coded_buffer_locked(
        context, coded->id, frame_tag);
    if (status < 0) {
        if (!context->encode_dmabuf)
            venus_surface_end_cpu_read(surface);
        return venus_backend_encode_status_from_errno(status);
    }

    surface->encode_pending = true;
    surface->coded_buffer_id = coded->id;

    if (context->encode_dmabuf) {
        status = venus_v4l2_encoder_submit_dmabuf(
            context->encoder, surface->dma_fd,
            surface->capacity, surface->stride,
            surface->scanlines, surface->uv_offset,
            frame_tag, store_packet, context);
    } else {
        status = venus_v4l2_encoder_submit_strided(
            context->encoder, surface->data,
            surface->data_size, surface->stride,
            surface->scanlines, surface->uv_offset,
            frame_tag, store_packet, context);
    }
    if (!context->encode_dmabuf) {
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
    struct venus_context *context;
    int64_t deadline;

    if (!surface->encode_pending)
        return surface->ready
                   ? VA_STATUS_SUCCESS
                   : VA_STATUS_ERROR_SURFACE_BUSY;

    buffer = venus_backend_find_buffer(
        backend, surface->coded_buffer_id);
    if (!buffer)
        return VA_STATUS_ERROR_INVALID_BUFFER;
    context = venus_backend_find_context(
        backend, buffer->context_id);
    if (!context || !context->encoder)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    deadline = monotonic_milliseconds() + timeout_ms;
    while (surface->encode_pending &&
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

    return surface->encode_pending
               ? VA_STATUS_ERROR_HW_BUSY
               : VA_STATUS_SUCCESS;
}

void venus_encode_close_context(struct venus_context *context)
{
    if (!context)
        return;

    venus_v4l2_encoder_close(context->encoder);
    context->encoder = NULL;
    context->encode_dmabuf = false;
    context->encode_queue_head = 0;
    context->encode_queue_count = 0;
}
