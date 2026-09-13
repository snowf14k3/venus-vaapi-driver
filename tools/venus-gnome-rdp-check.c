// SPDX-License-Identifier: MIT
#include <drm_fourcc.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>

#define REQUIRED_PACKED_HEADERS \
    (VA_ENC_PACKED_HEADER_SEQUENCE | VA_ENC_PACKED_HEADER_PICTURE | \
     VA_ENC_PACKED_HEADER_SLICE | VA_ENC_PACKED_HEADER_RAW_DATA)

static int fail_va(const char *operation, VAStatus status)
{
    fprintf(stderr, "FAIL %s: %s (%d)\n",
            operation, vaErrorStr(status), status);
    return 1;
}

int main(int argc, char **argv)
{
    const char *device =
        argc > 1 ? argv[1] : "/dev/dri/renderD128";
    VAConfigAttrib capabilities[4] = {
        { .type = VAConfigAttribRTFormat },
        { .type = VAConfigAttribRateControl },
        { .type = VAConfigAttribEncMaxRefFrames },
        { .type = VAConfigAttribEncPackedHeaders },
    };
    VAConfigAttrib config_attributes[3] = {
        {
            .type = VAConfigAttribRTFormat,
            .value = VA_RT_FORMAT_YUV420,
        },
        {
            .type = VAConfigAttribRateControl,
            .value = VA_RC_CQP,
        },
        {
            .type = VAConfigAttribEncPackedHeaders,
            .value = REQUIRED_PACKED_HEADERS,
        },
    };
    VASurfaceAttrib surface_attributes[2] = {
        {
            .type = VASurfaceAttribPixelFormat,
            .flags = VA_SURFACE_ATTRIB_SETTABLE,
            .value = {
                .type = VAGenericValueTypeInteger,
                .value.i = VA_FOURCC_NV12,
            },
        },
        {
            .type = VASurfaceAttribUsageHint,
            .flags = VA_SURFACE_ATTRIB_SETTABLE,
            .value = {
                .type = VAGenericValueTypeInteger,
                .value.i = VA_SURFACE_ATTRIB_USAGE_HINT_EXPORT,
            },
        },
    };
    VADRMPRIMESurfaceDescriptor prime;
    VAConfigID config = VA_INVALID_ID;
    VAContextID context = VA_INVALID_ID;
    VASurfaceID surface = VA_INVALID_SURFACE;
    VADisplay display = NULL;
    VAStatus va_status;
    int major = 0;
    int minor = 0;
    int drm_fd = -1;
    int result = 1;
    uint32_t index;

    if (argc > 2) {
        fprintf(stderr, "usage: %s [RENDER_NODE]\n", argv[0]);
        return 2;
    }

    drm_fd = open(device, O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) {
        perror("open render node");
        goto finish;
    }

    display = vaGetDisplayDRM(drm_fd);
    if (!display) {
        fputs("FAIL vaGetDisplayDRM\n", stderr);
        goto finish;
    }

    va_status = vaInitialize(display, &major, &minor);
    if (va_status != VA_STATUS_SUCCESS) {
        fail_va("vaInitialize", va_status);
        display = NULL;
        goto finish;
    }

    va_status = vaGetConfigAttributes(
        display, VAProfileH264High, VAEntrypointEncSlice,
        capabilities, 4);
    if (va_status != VA_STATUS_SUCCESS) {
        fail_va("vaGetConfigAttributes", va_status);
        goto finish;
    }
    if (!(capabilities[0].value & VA_RT_FORMAT_YUV420) ||
        !(capabilities[1].value & VA_RC_CQP) ||
        (capabilities[2].value & 0xffffu) < 1 ||
        (capabilities[3].value & REQUIRED_PACKED_HEADERS) !=
            REQUIRED_PACKED_HEADERS) {
        fprintf(
            stderr,
            "FAIL capabilities rt=0x%x rc=0x%x refs=0x%x packed=0x%x\n",
            capabilities[0].value, capabilities[1].value,
            capabilities[2].value, capabilities[3].value);
        goto finish;
    }

    va_status = vaCreateConfig(
        display, VAProfileH264High, VAEntrypointEncSlice,
        config_attributes, 3, &config);
    if (va_status != VA_STATUS_SUCCESS) {
        fail_va("vaCreateConfig", va_status);
        goto finish;
    }

    va_status = vaCreateContext(
        display, config, 640, 480, VA_PROGRESSIVE,
        NULL, 0, &context);
    if (va_status != VA_STATUS_SUCCESS) {
        fail_va("vaCreateContext", va_status);
        goto finish;
    }

    va_status = vaCreateSurfaces(
        display, VA_RT_FORMAT_YUV420, 640, 480,
        &surface, 1, surface_attributes, 2);
    if (va_status != VA_STATUS_SUCCESS) {
        fail_va("vaCreateSurfaces(export)", va_status);
        goto finish;
    }

    memset(&prime, 0, sizeof(prime));
    va_status = vaExportSurfaceHandle(
        display, surface,
        VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
        VA_EXPORT_SURFACE_WRITE_ONLY |
            VA_EXPORT_SURFACE_SEPARATE_LAYERS,
        &prime);
    if (va_status != VA_STATUS_SUCCESS) {
        fail_va("vaExportSurfaceHandle", va_status);
        goto finish;
    }

    if (prime.fourcc != VA_FOURCC_NV12 ||
        prime.width != 640 || prime.height != 480 ||
        prime.num_objects != 1 || prime.num_layers != 2 ||
        prime.layers[0].drm_format != DRM_FORMAT_R8 ||
        prime.layers[1].drm_format != DRM_FORMAT_GR88 ||
        prime.layers[0].num_planes != 1 ||
        prime.layers[1].num_planes != 1 ||
        prime.layers[0].pitch[0] < 640 ||
        prime.layers[1].pitch[0] !=
            prime.layers[0].pitch[0]) {
        fputs("FAIL exported NV12 descriptor contract\n", stderr);
        for (index = 0; index < prime.num_objects; index++)
            close(prime.objects[index].fd);
        goto finish;
    }

    printf("driver=%s\n", vaQueryVendorString(display));
    printf("vaapi=%d.%d\n", major, minor);
    printf("rate_control=0x%x\n", capabilities[1].value);
    printf("packed_headers=0x%x\n", capabilities[3].value);
    printf("dma_objects=%u layers=%u stride=%u uv_offset=%u\n",
           prime.num_objects, prime.num_layers,
           prime.layers[0].pitch[0],
           prime.layers[1].offset[0]);
    puts("PASS: GNOME Remote Desktop VAAPI capability and DMA-BUF contract");

    for (index = 0; index < prime.num_objects; index++)
        close(prime.objects[index].fd);
    result = 0;

finish:
    if (surface != VA_INVALID_SURFACE)
        vaDestroySurfaces(display, &surface, 1);
    if (context != VA_INVALID_ID)
        vaDestroyContext(display, context);
    if (config != VA_INVALID_ID)
        vaDestroyConfig(display, config);
    if (display)
        vaTerminate(display);
    if (drm_fd >= 0)
        close(drm_fd);
    return result;
}
