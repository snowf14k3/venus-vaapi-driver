// SPDX-License-Identifier: MIT
#include "backend.h"
#include "backend_internal.h"
#include "va_stubs.h"
#include "venus/capabilities.h"

#include <assert.h>
#include <linux/videodev2.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <va/va_backend.h>

int main(void)
{
    struct venus_capabilities capabilities;
    struct VADriverContext context;
    struct VADriverVTable vtable;
    VAProfile profiles[3];
    VAEntrypoint entrypoints[2];
    VAEncSequenceParameterBufferH264 sequence = {
        .picture_width_in_mbs = 120,
        .picture_height_in_mbs = 68,
        .seq_fields.bits = {
            .chroma_format_idc = 1,
            .frame_mbs_only_flag = 1,
        },
        .frame_cropping_flag = 1,
        .frame_crop_bottom_offset = 4,
    };
    VAConfigAttrib config_attribute = {
        .type = VAConfigAttribRTFormat,
        .value = VA_RT_FORMAT_YUV420,
    };
    VAConfigAttrib encode_attributes[2] = {
        {
            .type = VAConfigAttribRTFormat,
            .value = VA_RT_FORMAT_YUV420,
        },
        {
            .type = VAConfigAttribRateControl,
            .value = VA_RC_CBR,
        },
    };
    VAConfigAttrib queried_attributes[4] = {
        { .type = VAConfigAttribRTFormat },
        { .type = VAConfigAttribRateControl },
        { .type = VAConfigAttribEncPackedHeaders },
        { .type = VAConfigAttribEncMaxRefFrames },
    };
    VASurfaceAttrib surface_attribute = {
        .type = VASurfaceAttribPixelFormat,
        .flags = VA_SURFACE_ATTRIB_SETTABLE,
        .value = {
            .type = VAGenericValueTypeInteger,
            .value.i = VA_FOURCC_NV12,
        },
    };
    VAImageFormat image_format = {
        .fourcc = VA_FOURCC_NV12,
    };
    VASurfaceID surfaces[2];
    VAConfigID config;
    VAConfigID encode_config;
    VAContextID encode_context;
    VABufferID coded_buffers[2];
    VACodedBufferSegment *coded_segment;
    struct venus_backend *backend;
    struct venus_context *internal_context;
    struct venus_buffer *first_coded;
    struct venus_buffer *second_coded;
    const uint8_t first_packet_data[] = { 0x00, 0x00, 0x01, 0x65 };
    const uint8_t second_packet_data[] = { 0x00, 0x00, 0x01, 0x41 };
    struct venus_v4l2_packet packet = {
        .data = first_packet_data,
        .size = sizeof(first_packet_data),
    };
    VAImage image;
    VAImage derived;
    void *mapped;
    unsigned int num_surface_attributes = 0;
    uint32_t encoded_width = 0;
    uint32_t encoded_height = 0;
    int num_profiles = 0;
    int num_entrypoints = 0;
    size_t index;

    assert(venus_encode_h264_dimensions(
               &sequence, &encoded_width,
               &encoded_height) == 0);
    assert(encoded_width == 1920);
    assert(encoded_height == 1080);

    memset(&context, 0, sizeof(context));
    memset(&vtable, 0, sizeof(vtable));
    context.vtable = &vtable;

    venus_capabilities_reset(&capabilities);
    assert(venus_capabilities_add_fourcc(
        &capabilities, VENUS_ROLE_DECODER, V4L2_PIX_FMT_H264));
    assert(venus_capabilities_add_fourcc(
        &capabilities, VENUS_ROLE_ENCODER, V4L2_PIX_FMT_H264));
    strcpy(capabilities.decoder_path, "/dev/unused-for-object-test");
    strcpy(capabilities.encoder_path, "/dev/unused-for-object-test");

    assert(venus_backend_create(
               &capabilities, &context.pDriverData) == VA_STATUS_SUCCESS);
    venus_vtable_init(&vtable);
    venus_backend_fill_vtable(&vtable);

    assert(vtable.vaQueryConfigProfiles(
               &context, profiles, &num_profiles) == VA_STATUS_SUCCESS);
    assert(num_profiles == 3);
    assert(profiles[0] == VAProfileH264ConstrainedBaseline);
    assert(profiles[1] == VAProfileH264Main);
    assert(profiles[2] == VAProfileH264High);

    assert(vtable.vaQueryConfigEntrypoints(
               &context, VAProfileH264High, entrypoints,
               &num_entrypoints) == VA_STATUS_SUCCESS);
    assert(num_entrypoints == 2);
    assert(entrypoints[0] == VAEntrypointVLD);
    assert(entrypoints[1] == VAEntrypointEncSlice);

    assert(vtable.vaGetConfigAttributes(
               &context, VAProfileH264High,
               VAEntrypointEncSlice, queried_attributes,
               4) == VA_STATUS_SUCCESS);
    assert(queried_attributes[0].value == VA_RT_FORMAT_YUV420);
    assert(queried_attributes[1].value == VA_RC_CBR);
    assert(queried_attributes[2].value == VA_ATTRIB_NOT_SUPPORTED);
    assert(queried_attributes[3].value == 1);

    assert(vtable.vaCreateConfig(
               &context, VAProfileH264High, VAEntrypointVLD,
               &config_attribute, 1, &config) == VA_STATUS_SUCCESS);

    assert(vtable.vaQuerySurfaceAttributes(
               &context, config, NULL,
               &num_surface_attributes) == VA_STATUS_SUCCESS);
    assert(num_surface_attributes == 6);

    assert(vtable.vaCreateSurfaces2(
               &context, VA_RT_FORMAT_YUV420, 64, 32,
               surfaces, 2, &surface_attribute, 1) == VA_STATUS_SUCCESS);

    assert(vtable.vaCreateImage(
               &context, &image_format, 64, 32,
               &image) == VA_STATUS_SUCCESS);
    assert(image.format.fourcc == VA_FOURCC_NV12);
    assert(image.data_size == 64 * 32 * 3 / 2);
    assert(vtable.vaMapBuffer(
               &context, image.buf, &mapped) == VA_STATUS_SUCCESS);
    memset(mapped, 0x5a, image.data_size);
    assert(vtable.vaUnmapBuffer(
               &context, image.buf) == VA_STATUS_SUCCESS);

    assert(vtable.vaPutImage(
               &context, surfaces[0], image.image_id,
               0, 0, 64, 32, 0, 0, 64, 32) == VA_STATUS_SUCCESS);
    assert(vtable.vaDestroyImage(
               &context, image.image_id) == VA_STATUS_SUCCESS);
    assert(vtable.vaSyncSurface(
               &context, surfaces[0]) == VA_STATUS_SUCCESS);

    assert(vtable.vaDeriveImage(
               &context, surfaces[0], &derived) == VA_STATUS_SUCCESS);
    assert(vtable.vaMapBuffer(
               &context, derived.buf, &mapped) == VA_STATUS_SUCCESS);
    for (index = 0; index < derived.data_size; index++)
        assert(((const uint8_t *)mapped)[index] == 0x5a);
    assert(vtable.vaUnmapBuffer(
               &context, derived.buf) == VA_STATUS_SUCCESS);
    assert(vtable.vaDestroyImage(
               &context, derived.image_id) == VA_STATUS_SUCCESS);

    assert(vtable.vaCreateConfig(
               &context, VAProfileH264High, VAEntrypointEncSlice,
               encode_attributes, 2,
               &encode_config) == VA_STATUS_SUCCESS);
    assert(vtable.vaCreateContext(
               &context, encode_config, 64, 32, VA_PROGRESSIVE,
               surfaces, 2, &encode_context) == VA_STATUS_SUCCESS);
    assert(vtable.vaCreateBuffer(
               &context, encode_context, VAEncCodedBufferType,
               4096, 1, NULL, &coded_buffers[0]) == VA_STATUS_SUCCESS);
    assert(vtable.vaCreateBuffer(
               &context, encode_context, VAEncCodedBufferType,
               4096, 1, NULL, &coded_buffers[1]) == VA_STATUS_SUCCESS);
    assert(vtable.vaMapBuffer(
               &context, coded_buffers[0],
               (void **)&coded_segment) == VA_STATUS_SUCCESS);
    assert(coded_segment->size == 0);
    assert(coded_segment->buf != NULL);
    assert(coded_segment->next == NULL);
    assert(vtable.vaUnmapBuffer(
               &context, coded_buffers[0]) == VA_STATUS_SUCCESS);

    backend = context.pDriverData;
    internal_context =
        venus_backend_find_context(backend, encode_context);
    first_coded =
        venus_backend_find_buffer(backend, coded_buffers[0]);
    second_coded =
        venus_backend_find_buffer(backend, coded_buffers[1]);
    assert(internal_context != NULL);
    assert(first_coded != NULL);
    assert(second_coded != NULL);
    assert(venus_encode_queue_coded_buffer_locked(
               internal_context, coded_buffers[0]) == 0);
    assert(venus_encode_queue_coded_buffer_locked(
               internal_context, coded_buffers[1]) == 0);

    packet.tag = coded_buffers[1];
    assert(venus_encode_store_packet_locked(
               internal_context, &packet) == 0);
    assert(first_coded->coded_ready);
    assert(first_coded->coded_size == sizeof(first_packet_data));
    assert(memcmp(first_coded->data, first_packet_data,
                  sizeof(first_packet_data)) == 0);
    assert(!second_coded->coded_ready);

    packet.data = second_packet_data;
    packet.size = sizeof(second_packet_data);
    packet.tag = coded_buffers[0];
    assert(venus_encode_store_packet_locked(
               internal_context, &packet) == 0);
    assert(second_coded->coded_ready);
    assert(second_coded->coded_size == sizeof(second_packet_data));
    assert(memcmp(second_coded->data, second_packet_data,
                  sizeof(second_packet_data)) == 0);
    assert(internal_context->encode_queue_count == 0);

    assert(vtable.vaDestroyBuffer(
               &context, coded_buffers[0]) == VA_STATUS_SUCCESS);
    assert(vtable.vaDestroyBuffer(
               &context, coded_buffers[1]) == VA_STATUS_SUCCESS);
    assert(vtable.vaDestroyContext(
               &context, encode_context) == VA_STATUS_SUCCESS);
    assert(vtable.vaDestroyConfig(
               &context, encode_config) == VA_STATUS_SUCCESS);

    assert(vtable.vaDestroySurfaces(
               &context, surfaces, 2) == VA_STATUS_SUCCESS);
    assert(vtable.vaDestroyConfig(
               &context, config) == VA_STATUS_SUCCESS);
    assert(vtable.vaTerminate(&context) == VA_STATUS_SUCCESS);
    assert(context.pDriverData == NULL);
    return 0;
}
