// SPDX-License-Identifier: MIT
#include "backend_internal.h"

#include "h264_annexb.h"

#include <errno.h>
#include <limits.h>
#include <linux/videodev2.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define VENUS_MAX_SLICE_BATCHES 64
#define VENUS_ACCESS_UNIT_OVERHEAD 4096u
#define VENUS_SYNC_TIMEOUT_MS 30000

static int64_t monotonic_milliseconds(void)
{
    struct timespec timestamp;

    if (clock_gettime(CLOCK_MONOTONIC, &timestamp) != 0)
        return -1;
    return (int64_t)timestamp.tv_sec * 1000 +
           timestamp.tv_nsec / 1000000;
}

static void clear_pending(struct venus_context *context)
{
    context->pending_count = 0;
}

static void destroy_context_buffers(struct venus_backend *backend,
                                    VAContextID context_id)
{
    unsigned int index;

    for (index = 0; index < VENUS_MAX_BUFFERS; index++) {
        if (backend->buffers[index].used &&
            backend->buffers[index].context_id == context_id)
            venus_backend_free_buffer(&backend->buffers[index]);
    }
}

int venus_decode_store_frame_locked(
    const struct venus_v4l2_frame *frame, void *opaque)
{
    struct venus_backend *backend = opaque;
    struct venus_surface *surface;
    uint32_t stride;
    size_t source_uv_offset;
    int status;

    if (!frame || frame->tag > UINT32_MAX ||
        frame->width == 0 || frame->height == 0 ||
        (frame->width & 1u) || (frame->height & 1u))
        return -EINVAL;

    surface = venus_backend_find_surface(
        backend, (VASurfaceID)frame->tag);
    if (!surface)
        return -ENOENT;
    if (surface->width > frame->width ||
        surface->height > frame->height)
        return -EINVAL;

    stride = frame->bytes_per_line
                 ? frame->bytes_per_line
                 : frame->width;
    if (stride < frame->width ||
        stride > SIZE_MAX / frame->height)
        return -EINVAL;
    source_uv_offset = (size_t)stride * frame->height;

    status = venus_surface_copy_from_nv12(
        surface, frame->data, frame->size, stride,
        source_uv_offset, surface->width, surface->height);
    if (status < 0)
        return status;

    venus_backend_log(
        backend,
        "capture surface=0x%x tag=%llu bytes=%zu visible=%ux%u coded=%ux%u stride=%u",
        surface->id, (unsigned long long)frame->tag,
        surface->data_size, surface->width, surface->height,
        frame->width, frame->height, stride);
    return 0;
}

static VAStatus backend_create_context(
    VADriverContextP driver_context, VAConfigID config_id,
    int picture_width, int picture_height, int flags,
    VASurfaceID *render_targets, int num_render_targets,
    VAContextID *result)
{
    struct venus_backend *backend =
        venus_backend_from_context(driver_context);
    struct venus_v4l2_decoder_config decoder_config;
    struct venus_v4l2_error decoder_error;
    struct venus_config *config;
    struct venus_context *context = NULL;
    unsigned int index;
    int status;

    (void)flags;

    if (!backend || !result || picture_width < 0 ||
        picture_height < 0 || num_render_targets < 0 ||
        (num_render_targets > 0 && !render_targets))
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if ((unsigned int)picture_width < VENUS_MIN_WIDTH ||
        (unsigned int)picture_height < VENUS_MIN_HEIGHT ||
        (unsigned int)picture_width > VENUS_MAX_WIDTH ||
        (unsigned int)picture_height > VENUS_MAX_HEIGHT)
        return VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED;

    pthread_mutex_lock(&backend->mutex);
    config = venus_backend_find_config(backend, config_id);
    if (!config) {
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_ERROR_INVALID_CONFIG;
    }

    for (index = 0; index < (unsigned int)num_render_targets; index++) {
        struct venus_surface *surface =
            venus_backend_find_surface(backend, render_targets[index]);

        if (!surface || surface->width != (unsigned int)picture_width ||
            surface->height != (unsigned int)picture_height) {
            pthread_mutex_unlock(&backend->mutex);
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }
    }

    for (index = 0; index < VENUS_MAX_CONTEXTS; index++) {
        if (!backend->contexts[index].used) {
            context = &backend->contexts[index];
            break;
        }
    }
    if (!context) {
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
    }

    if (config->entrypoint == VAEntrypointVLD) {
        decoder_config = (struct venus_v4l2_decoder_config) {
            .device = backend->capabilities.decoder_path,
            .coded_format = V4L2_PIX_FMT_H264,
            .width = (uint32_t)picture_width,
            .height = (uint32_t)picture_height,
            .output_buffer_size = 2u * 1024u * 1024u,
            .output_buffers = 4,
            .capture_buffers = 16,
        };

        status = venus_v4l2_decoder_open(
            &decoder_config, &context->decoder, &decoder_error);
        if (status < 0) {
            venus_backend_log(
                backend,
                "create-context decoder-open failed operation=%s error=%d",
                decoder_error.operation[0]
                    ? decoder_error.operation
                    : "none",
                -status);
            pthread_mutex_unlock(&backend->mutex);
            return venus_backend_status_from_errno(status);
        }
    } else if (config->entrypoint != VAEntrypointEncSlice) {
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
    }

    context->used = true;
    context->id =
        VENUS_CONTEXT_BASE | (unsigned int)(context - backend->contexts + 1);
    context->config_id = config_id;
    context->width = (unsigned int)picture_width;
    context->backend = backend;
    context->height = (unsigned int)picture_height;
    context->target = VA_INVALID_ID;

    for (index = 0; index < (unsigned int)num_render_targets; index++) {
        struct venus_surface *surface =
            venus_backend_find_surface(backend, render_targets[index]);

        surface->context_id = context->id;
    }

    *result = context->id;
    venus_backend_log(backend,
                      "create-context id=0x%x config=0x%x size=%ux%u targets=%d",
                      context->id, config_id, context->width,
                      context->height, num_render_targets);
    pthread_mutex_unlock(&backend->mutex);
    return VA_STATUS_SUCCESS;
}

static void destroy_context_locked(struct venus_backend *backend,
                                   struct venus_context *context)
{
    unsigned int index;

    if (!context || !context->used)
        return;

    venus_v4l2_decoder_close(context->decoder);
    venus_encode_close_context(context);
    clear_pending(context);
    destroy_context_buffers(backend, context->id);

    for (index = 0; index < VENUS_MAX_SURFACES; index++) {
        if (backend->surfaces[index].used &&
            backend->surfaces[index].context_id == context->id) {
            backend->surfaces[index].context_id = VA_INVALID_ID;
            backend->surfaces[index].encode_pending = false;
            backend->surfaces[index].coded_buffer_id = VA_INVALID_ID;
        }
    }

    memset(context, 0, sizeof(*context));
}

static VAStatus backend_destroy_context(VADriverContextP driver_context,
                                        VAContextID context_id)
{
    struct venus_backend *backend =
        venus_backend_from_context(driver_context);
    struct venus_context *context;

    if (!backend)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    pthread_mutex_lock(&backend->mutex);
    context = venus_backend_find_context(backend, context_id);
    if (!context) {
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }

    destroy_context_locked(backend, context);
    pthread_mutex_unlock(&backend->mutex);
    return VA_STATUS_SUCCESS;
}

static VAStatus backend_begin_picture(VADriverContextP driver_context,
                                      VAContextID context_id,
                                      VASurfaceID render_target)
{
    struct venus_backend *backend =
        venus_backend_from_context(driver_context);
    struct venus_context *context;
    struct venus_config *config;
    struct venus_surface *surface;

    if (!backend)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    pthread_mutex_lock(&backend->mutex);
    context = venus_backend_find_context(backend, context_id);
    surface = venus_backend_find_surface(backend, render_target);
    if (!context) {
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    config = venus_backend_find_config(backend, context->config_id);
    if (!config) {
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_ERROR_INVALID_CONFIG;
    }
    if (!surface ||
        (config->entrypoint == VAEntrypointVLD &&
         (surface->width != context->width ||
          surface->height != context->height)) ||
        (config->entrypoint == VAEntrypointEncSlice &&
         (surface->width > context->width ||
          surface->height > context->height))) {
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    if (context->in_picture || surface->encode_pending) {
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    if (config->entrypoint == VAEntrypointEncSlice &&
        (!surface->ready || surface->data_size == 0)) {
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    context->in_picture = true;
    context->target = render_target;
    context->pending_count = 0;
    surface->context_id = context_id;
    if (config->entrypoint == VAEntrypointVLD) {
        surface->ready = false;
        surface->data_size = 0;
    }
    venus_backend_log(backend,
                      "begin-picture context=0x%x surface=0x%x entrypoint=%d",
                      context_id, render_target, config->entrypoint);

    pthread_mutex_unlock(&backend->mutex);
    return VA_STATUS_SUCCESS;
}

static VAStatus backend_render_picture(VADriverContextP driver_context,
                                       VAContextID context_id,
                                       VABufferID *buffers,
                                       int num_buffers)
{
    struct venus_backend *backend =
        venus_backend_from_context(driver_context);
    struct venus_context *context;
    int index;

    if (!backend || num_buffers < 0 ||
        (num_buffers > 0 && !buffers))
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    pthread_mutex_lock(&backend->mutex);
    context = venus_backend_find_context(backend, context_id);
    if (!context || !context->in_picture) {
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    if (context->pending_count + (size_t)num_buffers >
        VENUS_MAX_PENDING_BUFFERS) {
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
    }

    for (index = 0; index < num_buffers; index++) {
        struct venus_buffer *buffer =
            venus_backend_find_buffer(backend, buffers[index]);

        if (!buffer || buffer->context_id != context_id) {
            pthread_mutex_unlock(&backend->mutex);
            return VA_STATUS_ERROR_INVALID_BUFFER;
        }
    }

    for (index = 0; index < num_buffers; index++)
        context->pending[context->pending_count++] = buffers[index];

    pthread_mutex_unlock(&backend->mutex);
    return VA_STATUS_SUCCESS;
}

static int collect_h264_buffers(
    struct venus_backend *backend, struct venus_context *context,
    const VAPictureParameterBufferH264 **picture,
    struct venus_h264_slice_batch *batches, size_t *num_batches,
    size_t *access_unit_capacity)
{
    struct venus_buffer *slice_parameters = NULL;
    size_t index;

    *picture = NULL;
    *num_batches = 0;
    *access_unit_capacity = VENUS_ACCESS_UNIT_OVERHEAD;

    for (index = 0; index < context->pending_count; index++) {
        struct venus_buffer *buffer = venus_backend_find_buffer(
            backend, context->pending[index]);
        size_t buffer_size;

        if (!buffer || buffer->element_size >
                           SIZE_MAX / buffer->num_elements)
            return -EINVAL;
        buffer_size = buffer->element_size * buffer->num_elements;

        switch (buffer->type) {
        case VAPictureParameterBufferType:
            if (*picture || buffer->num_elements != 1 ||
                buffer->element_size <
                    sizeof(VAPictureParameterBufferH264))
                return -EINVAL;
            *picture =
                (const VAPictureParameterBufferH264 *)buffer->data;
            break;
        case VAIQMatrixBufferType:
            break;
        case VASliceParameterBufferType:
            if (slice_parameters ||
                buffer->element_size <
                    sizeof(VASliceParameterBufferH264))
                return -EINVAL;
            slice_parameters = buffer;
            break;
        case VASliceDataBufferType:
            if (!slice_parameters ||
                *num_batches >= VENUS_MAX_SLICE_BATCHES)
                return -EINVAL;
            batches[*num_batches] =
                (struct venus_h264_slice_batch) {
                    .parameters =
                        (const VASliceParameterBufferH264 *)
                            slice_parameters->data,
                    .num_parameters =
                        slice_parameters->num_elements,
                    .data = buffer->data,
                    .data_size = buffer_size,
                };
            (*num_batches)++;
            slice_parameters = NULL;
            if (buffer_size >
                SIZE_MAX - *access_unit_capacity)
                return -EOVERFLOW;
            *access_unit_capacity += buffer_size;
            break;
        default:
            return -ENOTSUP;
        }
    }

    if (!*picture || *num_batches == 0 || slice_parameters)
        return -EINVAL;
    return 0;
}

static VAStatus backend_end_picture(VADriverContextP driver_context,
                                    VAContextID context_id)
{
    struct venus_h264_slice_batch batches[VENUS_MAX_SLICE_BATCHES];
    const VAPictureParameterBufferH264 *picture;
    struct venus_backend *backend =
        venus_backend_from_context(driver_context);
    struct venus_context *context;
    struct venus_config *config;
    uint8_t *access_unit = NULL;
    size_t access_unit_capacity;
    size_t access_unit_size = 0;
    size_t num_batches;
    int status;

    if (!backend)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    pthread_mutex_lock(&backend->mutex);
    context = venus_backend_find_context(backend, context_id);
    if (!context || !context->in_picture) {
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }

    config = venus_backend_find_config(backend, context->config_id);
    if (!config) {
        status = -EINVAL;
        goto finish;
    }

    if (config->entrypoint == VAEntrypointEncSlice) {
        VAStatus encode_status = venus_encode_end_picture_locked(
            backend, context, config);

        clear_pending(context);
        context->in_picture = false;
        context->target = VA_INVALID_ID;
        if (encode_status != VA_STATUS_SUCCESS)
            venus_backend_log(
                backend,
                "end-picture encode failed context=0x%x status=%d",
                context_id, encode_status);
        pthread_mutex_unlock(&backend->mutex);
        return encode_status;
    }
    if (config->entrypoint != VAEntrypointVLD) {
        status = -ENOTSUP;
        goto finish;
    }

    status = collect_h264_buffers(
        backend, context, &picture, batches, &num_batches,
        &access_unit_capacity);
    if (status < 0)
        goto finish;

    access_unit = malloc(access_unit_capacity);
    if (!access_unit) {
        status = -ENOMEM;
        goto finish;
    }

    status = venus_h264_build_access_unit(
        config->profile, picture, batches, num_batches,
        access_unit, access_unit_capacity, &access_unit_size);
    if (status < 0)
        goto finish;

    venus_backend_log(backend,
                      "end-picture context=0x%x surface=0x%x batches=%zu access-unit=%zu",
                      context_id, context->target, num_batches,
                      access_unit_size);
    status = venus_v4l2_decoder_submit(
        context->decoder, access_unit, access_unit_size,
        context->target, venus_decode_store_frame_locked, backend);

finish:
    free(access_unit);
    clear_pending(context);
    context->in_picture = false;
    context->target = VA_INVALID_ID;
    if (status < 0)
        venus_backend_log(backend,
                          "end-picture failed context=0x%x error=%d",
                          context_id, -status);
    pthread_mutex_unlock(&backend->mutex);
    return status < 0 ? venus_backend_status_from_errno(status)
                      : VA_STATUS_SUCCESS;
}

static VAStatus sync_surface_locked(struct venus_backend *backend,
                                    struct venus_surface *surface,
                                    int timeout_ms)
{
    struct venus_context *context;
    int64_t deadline;

    if (surface->encode_pending)
        return venus_encode_sync_surface_locked(
            backend, surface, timeout_ms);
    if (surface->ready)
        return VA_STATUS_SUCCESS;

    context = venus_backend_find_context(
        backend, surface->context_id);
    if (!context)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    deadline = monotonic_milliseconds() + timeout_ms;
    while (!surface->ready &&
           monotonic_milliseconds() < deadline) {
        int status = venus_v4l2_decoder_pump(
            context->decoder, 1000,
            venus_decode_store_frame_locked, backend, NULL);

        if (status == -ETIMEDOUT || status == -EAGAIN)
            continue;
        if (status < 0)
            return venus_backend_status_from_errno(status);
    }

    return surface->ready ? VA_STATUS_SUCCESS
                          : VA_STATUS_ERROR_HW_BUSY;
}

static VAStatus backend_sync_surface(VADriverContextP driver_context,
                                     VASurfaceID surface_id)
{
    struct venus_backend *backend =
        venus_backend_from_context(driver_context);
    struct venus_surface *surface;
    VAStatus status;

    if (!backend)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    pthread_mutex_lock(&backend->mutex);
    surface = venus_backend_find_surface(backend, surface_id);
    if (!surface) {
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    status = sync_surface_locked(
        backend, surface, VENUS_SYNC_TIMEOUT_MS);
    venus_backend_log(backend,
                      "sync-surface id=0x%x status=%d bytes=%zu",
                      surface_id, status, surface->data_size);
    pthread_mutex_unlock(&backend->mutex);
    return status;
}

static VAStatus backend_sync_surface2(
    VADriverContextP driver_context, VASurfaceID surface_id,
    uint64_t timeout_ns)
{
    struct venus_backend *backend =
        venus_backend_from_context(driver_context);
    struct venus_surface *surface;
    uint64_t timeout_ms;
    VAStatus status;

    if (!backend)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    timeout_ms = timeout_ns == VA_TIMEOUT_INFINITE
                     ? VENUS_SYNC_TIMEOUT_MS
                     : (timeout_ns + 999999u) / 1000000u;
    if (timeout_ms > INT_MAX)
        timeout_ms = INT_MAX;

    pthread_mutex_lock(&backend->mutex);
    surface = venus_backend_find_surface(backend, surface_id);
    if (!surface) {
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    status = sync_surface_locked(
        backend, surface, (int)timeout_ms);
    pthread_mutex_unlock(&backend->mutex);
    return status;
}

static VAStatus backend_sync_buffer(
    VADriverContextP driver_context, VABufferID buffer_id,
    uint64_t timeout_ns)
{
    struct venus_backend *backend =
        venus_backend_from_context(driver_context);
    struct venus_buffer *buffer;
    uint64_t timeout_ms;
    VAStatus status;

    if (!backend)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    timeout_ms = timeout_ns == VA_TIMEOUT_INFINITE
                     ? VENUS_SYNC_TIMEOUT_MS
                     : (timeout_ns + 999999u) / 1000000u;
    if (timeout_ms > INT_MAX)
        timeout_ms = INT_MAX;

    pthread_mutex_lock(&backend->mutex);
    buffer = venus_backend_find_buffer(backend, buffer_id);
    if (!buffer || buffer->type != VAEncCodedBufferType) {
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }

    status = venus_encode_sync_buffer_locked(
        backend, buffer, (int)timeout_ms);
    venus_backend_log(
        backend,
        "sync-buffer id=0x%x status=%d bytes=%zu",
        buffer_id, status, buffer->coded_size);
    pthread_mutex_unlock(&backend->mutex);
    return status;
}

static VAStatus backend_query_surface_status(
    VADriverContextP driver_context, VASurfaceID surface_id,
    VASurfaceStatus *surface_status)
{
    struct venus_backend *backend =
        venus_backend_from_context(driver_context);
    struct venus_surface *surface;

    if (!backend || !surface_status)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    pthread_mutex_lock(&backend->mutex);
    surface = venus_backend_find_surface(backend, surface_id);
    if (!surface) {
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    *surface_status =
        surface->ready && !surface->encode_pending
            ? VASurfaceReady
            : VASurfaceRendering;
    pthread_mutex_unlock(&backend->mutex);
    return VA_STATUS_SUCCESS;
}

void venus_decode_destroy_all(struct venus_backend *backend)
{
    unsigned int index;

    for (index = 0; index < VENUS_MAX_CONTEXTS; index++)
        destroy_context_locked(backend, &backend->contexts[index]);
}

void venus_decode_fill_vtable(struct VADriverVTable *vtable)
{
    vtable->vaCreateContext = backend_create_context;
    vtable->vaDestroyContext = backend_destroy_context;
    vtable->vaBeginPicture = backend_begin_picture;
    vtable->vaRenderPicture = backend_render_picture;
    vtable->vaEndPicture = backend_end_picture;
    vtable->vaSyncSurface = backend_sync_surface;
    vtable->vaSyncSurface2 = backend_sync_surface2;
    vtable->vaSyncBuffer = backend_sync_buffer;
    vtable->vaQuerySurfaceStatus = backend_query_surface_status;
}
