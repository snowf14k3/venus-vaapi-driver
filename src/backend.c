// SPDX-License-Identifier: MIT
#include "backend.h"

#include "backend_internal.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct venus_backend *venus_backend_from_context(VADriverContextP context)
{
    return context ? context->pDriverData : NULL;
}

void venus_backend_log(const struct venus_backend *backend,
                       const char *format, ...)
{
    va_list arguments;

    if (!backend || !backend->debug)
        return;

    fputs("venus-vaapi: ", stderr);
    va_start(arguments, format);
    vfprintf(stderr, format, arguments);
    va_end(arguments);
    fputc('\n', stderr);
}

VAStatus venus_backend_status_from_errno(int status)
{
    switch (-status) {
    case ENOMEM:
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    case ENOSPC:
        return VA_STATUS_ERROR_NOT_ENOUGH_BUFFER;
    case ETIMEDOUT:
        return VA_STATUS_ERROR_HW_BUSY;
    case ENOTSUP:
#if EOPNOTSUPP != ENOTSUP
    case EOPNOTSUPP:
#endif
        return VA_STATUS_ERROR_UNIMPLEMENTED;
    case EINVAL:
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    default:
        return VA_STATUS_ERROR_DECODING_ERROR;
    }
}

VAStatus venus_backend_encode_status_from_errno(int status)
{
    VAStatus common = venus_backend_status_from_errno(status);

    return common == VA_STATUS_ERROR_DECODING_ERROR
               ? VA_STATUS_ERROR_ENCODING_ERROR
               : common;
}

bool venus_backend_h264_profile(VAProfile profile)
{
    return profile == VAProfileH264ConstrainedBaseline ||
           profile == VAProfileH264Main ||
           profile == VAProfileH264High;
}

bool venus_backend_h264_vld_supported(const struct venus_backend *backend,
                                      VAProfile profile,
                                      VAEntrypoint entrypoint)
{
    return backend && venus_backend_h264_profile(profile) &&
           entrypoint == VAEntrypointVLD &&
           venus_capabilities_has(&backend->capabilities,
                                  VENUS_ROLE_DECODER,
                                  VENUS_CODEC_H264);
}

bool venus_backend_h264_enc_supported(const struct venus_backend *backend,
                                      VAProfile profile,
                                      VAEntrypoint entrypoint)
{
    return backend && venus_backend_h264_profile(profile) &&
           entrypoint == VAEntrypointEncSlice &&
           venus_capabilities_has(&backend->capabilities,
                                  VENUS_ROLE_ENCODER,
                                  VENUS_CODEC_H264);
}

bool venus_backend_hevc_enc_supported(const struct venus_backend *backend,
                                      VAProfile profile,
                                      VAEntrypoint entrypoint)
{
    return backend && profile == VAProfileHEVCMain &&
           entrypoint == VAEntrypointEncSlice &&
           venus_capabilities_has(&backend->capabilities,
                                  VENUS_ROLE_ENCODER,
                                  VENUS_CODEC_HEVC);
}

struct venus_config *venus_backend_find_config(struct venus_backend *backend,
                                               VAConfigID id)
{
    unsigned int slot = id & 0x00ffffffu;

    if ((id & 0xff000000u) != VENUS_CONFIG_BASE || slot == 0 ||
        slot > VENUS_MAX_CONFIGS)
        return NULL;
    return backend->configs[slot - 1].used &&
                   backend->configs[slot - 1].id == id
               ? &backend->configs[slot - 1]
               : NULL;
}

struct venus_context *venus_backend_find_context(
    struct venus_backend *backend, VAContextID id)
{
    unsigned int slot = id & 0x00ffffffu;

    if ((id & 0xff000000u) != VENUS_CONTEXT_BASE || slot == 0 ||
        slot > VENUS_MAX_CONTEXTS)
        return NULL;
    return backend->contexts[slot - 1].used &&
                   backend->contexts[slot - 1].id == id
               ? &backend->contexts[slot - 1]
               : NULL;
}

struct venus_surface *venus_backend_find_surface(
    struct venus_backend *backend, VASurfaceID id)
{
    unsigned int slot = id & 0x00ffffffu;

    if ((id & 0xff000000u) != VENUS_SURFACE_BASE || slot == 0 ||
        slot > VENUS_MAX_SURFACES)
        return NULL;
    return backend->surfaces[slot - 1].used &&
                   backend->surfaces[slot - 1].id == id
               ? &backend->surfaces[slot - 1]
               : NULL;
}

struct venus_buffer *venus_backend_find_buffer(
    struct venus_backend *backend, VABufferID id)
{
    unsigned int slot = id & 0x00ffffffu;

    if ((id & 0xff000000u) != VENUS_BUFFER_BASE || slot == 0 ||
        slot > VENUS_MAX_BUFFERS)
        return NULL;
    return backend->buffers[slot - 1].used &&
                   backend->buffers[slot - 1].id == id
               ? &backend->buffers[slot - 1]
               : NULL;
}

struct venus_image *venus_backend_find_image(
    struct venus_backend *backend, VAImageID id)
{
    unsigned int slot = id & 0x00ffffffu;

    if ((id & 0xff000000u) != VENUS_IMAGE_BASE || slot == 0 ||
        slot > VENUS_MAX_IMAGES)
        return NULL;
    return backend->images[slot - 1].used &&
                   backend->images[slot - 1].id == id
               ? &backend->images[slot - 1]
               : NULL;
}

void venus_backend_free_buffer(struct venus_buffer *buffer)
{
    if (!buffer || !buffer->used)
        return;

    if (buffer->owns_data)
        free(buffer->data);
    memset(buffer, 0, sizeof(*buffer));
}

static VAStatus backend_terminate(VADriverContextP context)
{
    struct venus_backend *backend = venus_backend_from_context(context);

    if (!backend)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    pthread_mutex_lock(&backend->mutex);
    venus_decode_destroy_all(backend);
    venus_objects_destroy_all(backend);
    pthread_mutex_unlock(&backend->mutex);
    pthread_mutex_destroy(&backend->mutex);

    free(backend);
    context->pDriverData = NULL;
    return VA_STATUS_SUCCESS;
}

static VAStatus backend_query_profiles(VADriverContextP context,
                                       VAProfile *profiles,
                                       int *num_profiles)
{
    struct venus_backend *backend = venus_backend_from_context(context);

    if (!backend || !num_profiles)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    pthread_mutex_lock(&backend->mutex);
    if (venus_capabilities_has(&backend->capabilities,
                               VENUS_ROLE_DECODER,
                               VENUS_CODEC_H264) ||
        venus_capabilities_has(&backend->capabilities,
                               VENUS_ROLE_ENCODER,
                               VENUS_CODEC_H264)) {
        if (profiles) {
            profiles[0] = VAProfileH264ConstrainedBaseline;
            profiles[1] = VAProfileH264Main;
            profiles[2] = VAProfileH264High;
        }
        *num_profiles = 3;
    } else {
        *num_profiles = 0;
    }
    if (venus_capabilities_has(&backend->capabilities,
                               VENUS_ROLE_ENCODER, VENUS_CODEC_HEVC)) {
        if (profiles)
            profiles[*num_profiles] = VAProfileHEVCMain;
        (*num_profiles)++;
    }
    pthread_mutex_unlock(&backend->mutex);
    return VA_STATUS_SUCCESS;
}

static VAStatus backend_query_entrypoints(VADriverContextP context,
                                          VAProfile profile,
                                          VAEntrypoint *entrypoints,
                                          int *num_entrypoints)
{
    struct venus_backend *backend = venus_backend_from_context(context);

    if (!backend || !num_entrypoints)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    pthread_mutex_lock(&backend->mutex);
    if (!venus_backend_h264_profile(profile) &&
        profile != VAProfileHEVCMain) {
        pthread_mutex_unlock(&backend->mutex);
        *num_entrypoints = 0;
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }

    *num_entrypoints = 0;
    if (venus_backend_h264_vld_supported(
            backend, profile, VAEntrypointVLD)) {
        if (entrypoints)
            entrypoints[*num_entrypoints] = VAEntrypointVLD;
        (*num_entrypoints)++;
    }
    if (venus_backend_h264_enc_supported(
            backend, profile, VAEntrypointEncSlice)) {
        if (entrypoints)
            entrypoints[*num_entrypoints] = VAEntrypointEncSlice;
        (*num_entrypoints)++;
    }
    if (venus_backend_hevc_enc_supported(
            backend, profile, VAEntrypointEncSlice)) {
        if (entrypoints)
            entrypoints[*num_entrypoints] = VAEntrypointEncSlice;
        (*num_entrypoints)++;
    }
    if (*num_entrypoints == 0) {
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }

    pthread_mutex_unlock(&backend->mutex);
    return VA_STATUS_SUCCESS;
}

static VAStatus backend_get_config_attributes(
    VADriverContextP context, VAProfile profile,
    VAEntrypoint entrypoint, VAConfigAttrib *attributes,
    int num_attributes)
{
    struct venus_backend *backend = venus_backend_from_context(context);
    int index;

    if (!backend || num_attributes < 0 ||
        (num_attributes > 0 && !attributes))
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    pthread_mutex_lock(&backend->mutex);
    if (!venus_backend_h264_vld_supported(
            backend, profile, entrypoint) &&
        !venus_backend_h264_enc_supported(
            backend, profile, entrypoint) &&
        !venus_backend_hevc_enc_supported(
            backend, profile, entrypoint)) {
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }

    for (index = 0; index < num_attributes; index++) {
        switch (attributes[index].type) {
        case VAConfigAttribRTFormat:
            attributes[index].value = VA_RT_FORMAT_YUV420;
            break;
        case VAConfigAttribDecSliceMode:
            attributes[index].value =
                entrypoint == VAEntrypointVLD
                    ? VA_DEC_SLICE_MODE_NORMAL
                    : VA_ATTRIB_NOT_SUPPORTED;
            break;
        case VAConfigAttribRateControl:
            attributes[index].value =
                entrypoint == VAEntrypointEncSlice
                    ? VA_RC_VBR | VA_RC_CBR | VA_RC_CQP
                    : VA_ATTRIB_NOT_SUPPORTED;
            break;
        case VAConfigAttribEncPackedHeaders:
            attributes[index].value =
                entrypoint == VAEntrypointEncSlice
                    ? VENUS_H264_PACKED_HEADERS
                    : VA_ATTRIB_NOT_SUPPORTED;
            break;
        case VAConfigAttribEncMaxRefFrames:
            attributes[index].value =
                entrypoint == VAEntrypointEncSlice
                    ? 1
                    : VA_ATTRIB_NOT_SUPPORTED;
            break;
        case VAConfigAttribEncMaxSlices:
            attributes[index].value =
                entrypoint == VAEntrypointEncSlice
                    ? 1
                    : VA_ATTRIB_NOT_SUPPORTED;
            break;
        case VAConfigAttribMaxPictureWidth:
            attributes[index].value = VENUS_MAX_WIDTH;
            break;
        case VAConfigAttribMaxPictureHeight:
            attributes[index].value = VENUS_MAX_HEIGHT;
            break;
        default:
            attributes[index].value = VA_ATTRIB_NOT_SUPPORTED;
            break;
        }

        venus_backend_log(
            backend,
            "config-attribute profile=%d entrypoint=%d type=%u value=0x%x",
            profile, entrypoint, attributes[index].type,
            attributes[index].value);
    }

    pthread_mutex_unlock(&backend->mutex);
    return VA_STATUS_SUCCESS;
}

static VAStatus backend_create_config(
    VADriverContextP context, VAProfile profile,
    VAEntrypoint entrypoint, VAConfigAttrib *attributes,
    int num_attributes, VAConfigID *config_id)
{
    struct venus_backend *backend = venus_backend_from_context(context);
    uint32_t rate_control =
        entrypoint == VAEntrypointEncSlice ? VA_RC_VBR : 0;
    uint32_t packed_headers = 0;
    unsigned int index;
    int attribute;

    if (!backend || !config_id || num_attributes < 0 ||
        (num_attributes > 0 && !attributes))
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    pthread_mutex_lock(&backend->mutex);
    if (!venus_backend_h264_vld_supported(
            backend, profile, entrypoint) &&
        !venus_backend_h264_enc_supported(
            backend, profile, entrypoint) &&
        !venus_backend_hevc_enc_supported(
            backend, profile, entrypoint)) {
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }

    for (attribute = 0; attribute < num_attributes; attribute++) {
        if (attributes[attribute].type == VAConfigAttribRTFormat &&
            !(attributes[attribute].value & VA_RT_FORMAT_YUV420)) {
            pthread_mutex_unlock(&backend->mutex);
            return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
        }
        if (entrypoint == VAEntrypointVLD &&
            attributes[attribute].type == VAConfigAttribDecSliceMode &&
            attributes[attribute].value != VA_DEC_SLICE_MODE_NORMAL) {
            pthread_mutex_unlock(&backend->mutex);
            return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
        }
        if (entrypoint == VAEntrypointEncSlice &&
            attributes[attribute].type == VAConfigAttribRateControl) {
            if (attributes[attribute].value != VA_RC_VBR &&
                attributes[attribute].value != VA_RC_CBR &&
                attributes[attribute].value != VA_RC_CQP) {
                pthread_mutex_unlock(&backend->mutex);
                return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
            }
            rate_control = attributes[attribute].value;
        }
        if (entrypoint == VAEntrypointEncSlice &&
            attributes[attribute].type ==
                VAConfigAttribEncPackedHeaders) {
            if (attributes[attribute].value &
                ~VENUS_H264_PACKED_HEADERS) {
                pthread_mutex_unlock(&backend->mutex);
                return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
            }
            packed_headers = attributes[attribute].value;
        }
    }

    for (index = 0; index < VENUS_MAX_CONFIGS; index++) {
        struct venus_config *config = &backend->configs[index];

        if (config->used)
            continue;

        *config = (struct venus_config) {
            .used = true,
            .id = VENUS_CONFIG_BASE | (index + 1),
            .profile = profile,
            .entrypoint = entrypoint,
            .rate_control = rate_control,
            .packed_headers = packed_headers,
        };
        *config_id = config->id;
        venus_backend_log(
            backend,
            "create-config id=0x%x profile=%d entrypoint=%d rc=0x%x packed=0x%x",
            config->id, profile, entrypoint,
            rate_control, packed_headers);
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_SUCCESS;
    }

    pthread_mutex_unlock(&backend->mutex);
    return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
}

static VAStatus backend_destroy_config(VADriverContextP context,
                                       VAConfigID config_id)
{
    struct venus_backend *backend = venus_backend_from_context(context);
    struct venus_config *config;
    unsigned int index;

    if (!backend)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    pthread_mutex_lock(&backend->mutex);
    config = venus_backend_find_config(backend, config_id);
    if (!config) {
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_ERROR_INVALID_CONFIG;
    }

    for (index = 0; index < VENUS_MAX_CONTEXTS; index++) {
        if (backend->contexts[index].used &&
            backend->contexts[index].config_id == config_id) {
            pthread_mutex_unlock(&backend->mutex);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
    }

    memset(config, 0, sizeof(*config));
    pthread_mutex_unlock(&backend->mutex);
    return VA_STATUS_SUCCESS;
}

static VAStatus backend_query_config_attributes(
    VADriverContextP context, VAConfigID config_id,
    VAProfile *profile, VAEntrypoint *entrypoint,
    VAConfigAttrib *attributes, int *num_attributes)
{
    struct venus_backend *backend = venus_backend_from_context(context);
    struct venus_config *config;
    int required;

    if (!backend || !profile || !entrypoint || !num_attributes)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    pthread_mutex_lock(&backend->mutex);
    config = venus_backend_find_config(backend, config_id);
    if (!config) {
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_ERROR_INVALID_CONFIG;
    }

    *profile = config->profile;
    *entrypoint = config->entrypoint;
    required = config->entrypoint == VAEntrypointEncSlice ? 3 : 1;
    if (!attributes) {
        *num_attributes = required;
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_SUCCESS;
    }
    /* VA-API returns the count here; callers need not initialize it. */
    attributes[0] = (VAConfigAttrib) {
        .type = VAConfigAttribRTFormat,
        .value = VA_RT_FORMAT_YUV420,
    };
    if (required == 3) {
        attributes[1] = (VAConfigAttrib) {
            .type = VAConfigAttribRateControl,
            .value = config->rate_control,
        };
        attributes[2] = (VAConfigAttrib) {
            .type = VAConfigAttribEncPackedHeaders,
            .value = config->packed_headers,
        };
    }
    *num_attributes = required;

    pthread_mutex_unlock(&backend->mutex);
    return VA_STATUS_SUCCESS;
}

VAStatus venus_backend_create(const struct venus_capabilities *capabilities,
                              void **result)
{
    struct venus_backend *backend;
    int status;

    if (!capabilities || !result)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    *result = NULL;
    backend = calloc(1, sizeof(*backend));
    if (!backend)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;

    status = pthread_mutex_init(&backend->mutex, NULL);
    if (status != 0) {
        free(backend);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    backend->capabilities = *capabilities;
    backend->debug = getenv("VENUS_VAAPI_LOG") != NULL;
    venus_backend_log(
        backend, "probe decoder=%s decode_mask=0x%x encoder=%s encode_mask=0x%x",
        capabilities->decoder_path[0] ? capabilities->decoder_path : "none",
        capabilities->decode_codecs,
        capabilities->encoder_path[0] ? capabilities->encoder_path : "none",
        capabilities->encode_codecs);
    *result = backend;
    return VA_STATUS_SUCCESS;
}

void venus_backend_fill_vtable(struct VADriverVTable *vtable)
{
    vtable->vaTerminate = backend_terminate;
    vtable->vaQueryConfigProfiles = backend_query_profiles;
    vtable->vaQueryConfigEntrypoints = backend_query_entrypoints;
    vtable->vaGetConfigAttributes = backend_get_config_attributes;
    vtable->vaCreateConfig = backend_create_config;
    vtable->vaDestroyConfig = backend_destroy_config;
    vtable->vaQueryConfigAttributes =
        backend_query_config_attributes;

    venus_objects_fill_vtable(vtable);
    venus_decode_fill_vtable(vtable);
}
