// SPDX-License-Identifier: MIT
#include "driver.h"

#include "v4l2_probe.h"
#include "va_stubs.h"

#include <stdlib.h>
#include <string.h>

#define VENUS_VENDOR_STRING "Mesa-independent Qualcomm Venus VA-API backend 0.1.0"
#define VENUS_INIT_NAME_INNER(major, minor) __vaDriverInit_##major##_##minor
#define VENUS_INIT_NAME(major, minor) VENUS_INIT_NAME_INNER(major, minor)
#define VENUS_PUBLIC __attribute__((visibility("default")))

struct venus_driver {
    struct venus_capabilities capabilities;
};

VAStatus venus_driver_init(VADriverContextP context)
{
    struct venus_driver *driver;
    const char *allow_no_device;
    int result;

    if (!context || !context->vtable)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    driver = calloc(1, sizeof(*driver));
    if (!driver)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;

    result = venus_v4l2_probe(&driver->capabilities);
    allow_no_device = getenv("VENUS_VAAPI_ALLOW_NO_DEVICE");
    if (result < 0 &&
        (!allow_no_device || strcmp(allow_no_device, "1") != 0)) {
        free(driver);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    context->pDriverData = driver;
    context->version_major = VA_MAJOR_VERSION;
    context->version_minor = VA_MINOR_VERSION;
    context->max_profiles = 1;
    context->max_entrypoints = 1;
    context->max_attributes = 4;
    context->max_image_formats = 1;
    context->max_subpic_formats = 1;
    context->max_display_attributes = 1;
    context->str_vendor = VENUS_VENDOR_STRING;

    venus_vtable_init(context->vtable);
    return VA_STATUS_SUCCESS;
}

VENUS_PUBLIC VAStatus
VENUS_INIT_NAME(VA_MAJOR_VERSION, VA_MINOR_VERSION)(VADriverContextP context)
{
    return venus_driver_init(context);
}
