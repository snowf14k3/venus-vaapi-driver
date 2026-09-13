// SPDX-License-Identifier: MIT
#include "backend.h"
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
    VAEntrypoint entrypoints[1];
    VAConfigAttrib config_attribute = {
        .type = VAConfigAttribRTFormat,
        .value = VA_RT_FORMAT_YUV420,
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
    VAImage image;
    VAImage derived;
    void *mapped;
    unsigned int num_surface_attributes = 0;
    int num_profiles = 0;
    int num_entrypoints = 0;
    size_t index;

    memset(&context, 0, sizeof(context));
    memset(&vtable, 0, sizeof(vtable));
    context.vtable = &vtable;

    venus_capabilities_reset(&capabilities);
    assert(venus_capabilities_add_fourcc(
        &capabilities, VENUS_ROLE_DECODER, V4L2_PIX_FMT_H264));
    strcpy(capabilities.decoder_path, "/dev/unused-for-object-test");

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
    assert(num_entrypoints == 1);
    assert(entrypoints[0] == VAEntrypointVLD);

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

    assert(vtable.vaDestroySurfaces(
               &context, surfaces, 2) == VA_STATUS_SUCCESS);
    assert(vtable.vaDestroyConfig(
               &context, config) == VA_STATUS_SUCCESS);
    assert(vtable.vaTerminate(&context) == VA_STATUS_SUCCESS);
    assert(context.pDriverData == NULL);
    return 0;
}
