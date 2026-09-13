// SPDX-License-Identifier: MIT
#include "driver.h"

#include "backend.h"
#include "v4l2_probe.h"
#include "va_stubs.h"

#include <stdlib.h>
#include <string.h>

#define VENUS_VENDOR_STRING \
    "Qualcomm Venus stateful V4L2 VA-API backend 0.4.2"
#define VENUS_INIT_NAME_INNER(major, minor) \
    __vaDriverInit_##major##_##minor
#define VENUS_INIT_NAME(major, minor) \
    VENUS_INIT_NAME_INNER(major, minor)
#define VENUS_PUBLIC __attribute__((visibility("default")))

VAStatus venus_driver_init(VADriverContextP context)
{
    struct venus_capabilities capabilities;
    const char *allow_no_device;
    const char *force_no_device;
    VAStatus status;
    void *backend = NULL;
    int result;

    if (!context || !context->vtable)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    force_no_device = getenv("VENUS_VAAPI_FORCE_NO_DEVICE");
    if (force_no_device &&
        strcmp(force_no_device, "1") == 0) {
        venus_capabilities_reset(&capabilities);
        result = 0;
    } else {
        result = venus_v4l2_probe(&capabilities);
        allow_no_device =
            getenv("VENUS_VAAPI_ALLOW_NO_DEVICE");
        if (result < 0 &&
            (!allow_no_device ||
             strcmp(allow_no_device, "1") != 0))
            return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    status = venus_backend_create(&capabilities, &backend);
    if (status != VA_STATUS_SUCCESS)
        return status;

    context->pDriverData = backend;
    context->version_major = VA_MAJOR_VERSION;
    context->version_minor = VA_MINOR_VERSION;
    context->max_profiles = 3;
    context->max_entrypoints = 2;
    context->max_attributes = 8;
    context->max_image_formats = 1;
    context->max_subpic_formats = 1;
    context->max_display_attributes = 1;
    context->str_vendor = VENUS_VENDOR_STRING;

    venus_vtable_init(context->vtable);
    venus_backend_fill_vtable(context->vtable);
    return VA_STATUS_SUCCESS;
}

VENUS_PUBLIC VAStatus
VENUS_INIT_NAME(VA_MAJOR_VERSION, VA_MINOR_VERSION)(
    VADriverContextP context)
{
    return venus_driver_init(context);
}
