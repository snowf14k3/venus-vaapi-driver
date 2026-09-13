// SPDX-License-Identifier: MIT
#include "backend_internal.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static int nv12_size(unsigned int width, unsigned int height,
                     size_t *size)
{
    size_t pixels;

    if (width == 0 || height == 0 || width % 2 || height % 2 ||
        width > SIZE_MAX / height)
        return -1;

    pixels = (size_t)width * height;
    if (pixels > SIZE_MAX / 3 * 2)
        return -1;

    *size = pixels + pixels / 2;
    return 0;
}

static struct venus_buffer *allocate_buffer(
    struct venus_backend *backend, VAContextID context_id,
    VABufferType type, size_t element_size, size_t num_elements,
    const void *data, bool owns_data)
{
    size_t total_size;
    unsigned int index;

    if (element_size == 0 || num_elements == 0 ||
        element_size > SIZE_MAX / num_elements)
        return NULL;
    total_size = element_size * num_elements;

    for (index = 0; index < VENUS_MAX_BUFFERS; index++) {
        struct venus_buffer *buffer = &backend->buffers[index];

        if (buffer->used)
            continue;

        *buffer = (struct venus_buffer) {
            .used = true,
            .id = VENUS_BUFFER_BASE | (index + 1),
            .context_id = context_id,
            .type = type,
            .element_size = element_size,
            .capacity_elements = num_elements,
            .num_elements = num_elements,
            .owns_data = owns_data,
        };

        if (owns_data) {
            buffer->data = malloc(total_size);
            if (!buffer->data) {
                memset(buffer, 0, sizeof(*buffer));
                return NULL;
            }
            if (data)
                memcpy(buffer->data, data, total_size);
            else
                memset(buffer->data, 0, total_size);
        } else {
            buffer->data = (uint8_t *)data;
        }

        return buffer;
    }

    return NULL;
}

static bool valid_surface_attributes(VASurfaceAttrib *attributes,
                                     unsigned int num_attributes,
                                     uint32_t *fourcc)
{
    unsigned int index;

    *fourcc = VA_FOURCC_NV12;
    for (index = 0; index < num_attributes; index++) {
        const VASurfaceAttrib *attribute = &attributes[index];

        if (attribute->type == VASurfaceAttribPixelFormat) {
            if (attribute->value.type != VAGenericValueTypeInteger ||
                attribute->value.value.i != VA_FOURCC_NV12)
                return false;
            *fourcc = (uint32_t)attribute->value.value.i;
        } else if (attribute->type == VASurfaceAttribMemoryType) {
            if (attribute->value.type != VAGenericValueTypeInteger ||
                !(attribute->value.value.i &
                  VA_SURFACE_ATTRIB_MEM_TYPE_VA))
                return false;
        } else if (attribute->flags & VA_SURFACE_ATTRIB_SETTABLE) {
            return false;
        }
    }

    return true;
}

static VAStatus create_surfaces_locked(
    struct venus_backend *backend, unsigned int format,
    unsigned int width, unsigned int height,
    VASurfaceID *surface_ids, unsigned int num_surfaces,
    VASurfaceAttrib *attributes, unsigned int num_attributes)
{
    struct venus_surface *created[VENUS_MAX_SURFACES];
    size_t capacity;
    uint32_t fourcc;
    unsigned int free_slots = 0;
    unsigned int created_count = 0;
    unsigned int index;

    if (!(format & VA_RT_FORMAT_YUV420))
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    if (!surface_ids || num_surfaces == 0 ||
        num_surfaces > VENUS_MAX_SURFACES ||
        width < VENUS_MIN_WIDTH || height < VENUS_MIN_HEIGHT ||
        width > VENUS_MAX_WIDTH || height > VENUS_MAX_HEIGHT ||
        nv12_size(width, height, &capacity) < 0)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (num_attributes > 0 && !attributes)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (!valid_surface_attributes(
            attributes, num_attributes, &fourcc))
        return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;

    for (index = 0; index < VENUS_MAX_SURFACES; index++) {
        if (!backend->surfaces[index].used)
            free_slots++;
    }
    if (free_slots < num_surfaces)
        return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;

    for (index = 0; index < VENUS_MAX_SURFACES &&
                    created_count < num_surfaces;
         index++) {
        struct venus_surface *surface = &backend->surfaces[index];

        if (surface->used)
            continue;

        surface->data = calloc(1, capacity);
        if (!surface->data)
            goto fail;

        surface->used = true;
        surface->id = VENUS_SURFACE_BASE | (index + 1);
        surface->width = width;
        surface->height = height;
        surface->fourcc = fourcc;
        surface->capacity = capacity;
        surface->context_id = VA_INVALID_ID;
        created[created_count] = surface;
        surface_ids[created_count] = surface->id;
        venus_backend_log(backend,
                          "create-surface id=0x%x size=%ux%u bytes=%zu",
                          surface->id, width, height, capacity);
        created_count++;
    }

    return VA_STATUS_SUCCESS;

fail:
    while (created_count > 0) {
        struct venus_surface *surface = created[--created_count];

        free(surface->data);
        memset(surface, 0, sizeof(*surface));
    }
    return VA_STATUS_ERROR_ALLOCATION_FAILED;
}

static VAStatus backend_create_surfaces(
    VADriverContextP context, int width, int height, int format,
    int num_surfaces, VASurfaceID *surfaces)
{
    struct venus_backend *backend =
        venus_backend_from_context(context);
    VAStatus status;

    if (!backend || width < 0 || height < 0 || num_surfaces < 0)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    pthread_mutex_lock(&backend->mutex);
    status = create_surfaces_locked(
        backend, (unsigned int)format, (unsigned int)width,
        (unsigned int)height, surfaces, (unsigned int)num_surfaces,
        NULL, 0);
    pthread_mutex_unlock(&backend->mutex);
    return status;
}

static VAStatus backend_create_surfaces2(
    VADriverContextP context, unsigned int format,
    unsigned int width, unsigned int height,
    VASurfaceID *surfaces, unsigned int num_surfaces,
    VASurfaceAttrib *attributes, unsigned int num_attributes)
{
    struct venus_backend *backend =
        venus_backend_from_context(context);
    VAStatus status;

    if (!backend)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    pthread_mutex_lock(&backend->mutex);
    status = create_surfaces_locked(
        backend, format, width, height, surfaces, num_surfaces,
        attributes, num_attributes);
    pthread_mutex_unlock(&backend->mutex);
    return status;
}

static VAStatus backend_destroy_surfaces(
    VADriverContextP context, VASurfaceID *surface_ids,
    int num_surfaces)
{
    struct venus_backend *backend =
        venus_backend_from_context(context);
    int index;

    if (!backend || num_surfaces < 0 ||
        (num_surfaces > 0 && !surface_ids))
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    pthread_mutex_lock(&backend->mutex);
    for (index = 0; index < num_surfaces; index++) {
        if (!venus_backend_find_surface(
                backend, surface_ids[index])) {
            pthread_mutex_unlock(&backend->mutex);
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }
    }

    for (index = 0; index < num_surfaces; index++) {
        struct venus_surface *surface =
            venus_backend_find_surface(backend, surface_ids[index]);

        free(surface->data);
        memset(surface, 0, sizeof(*surface));
    }

    pthread_mutex_unlock(&backend->mutex);
    return VA_STATUS_SUCCESS;
}

static VAStatus backend_create_buffer(
    VADriverContextP context, VAContextID context_id,
    VABufferType type, unsigned int size,
    unsigned int num_elements, void *data,
    VABufferID *buffer_id)
{
    struct venus_backend *backend =
        venus_backend_from_context(context);
    struct venus_buffer *buffer;

    if (!backend || !buffer_id || size == 0 || num_elements == 0)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    pthread_mutex_lock(&backend->mutex);
    if (!venus_backend_find_context(backend, context_id)) {
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }

    buffer = allocate_buffer(
        backend, context_id, type, size, num_elements, data, true);
    if (!buffer) {
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    *buffer_id = buffer->id;
    venus_backend_log(backend,
                      "create-buffer id=0x%x context=0x%x type=%d elements=%u size=%u",
                      buffer->id, context_id, type, num_elements, size);
    pthread_mutex_unlock(&backend->mutex);
    return VA_STATUS_SUCCESS;
}

static VAStatus backend_set_num_elements(
    VADriverContextP context, VABufferID buffer_id,
    unsigned int num_elements)
{
    struct venus_backend *backend =
        venus_backend_from_context(context);
    struct venus_buffer *buffer;
    uint8_t *resized;
    size_t old_size;
    size_t new_size;

    if (!backend || num_elements == 0)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    pthread_mutex_lock(&backend->mutex);
    buffer = venus_backend_find_buffer(backend, buffer_id);
    if (!buffer || !buffer->owns_data) {
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }

    if (num_elements > SIZE_MAX / buffer->element_size) {
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    if (num_elements > buffer->capacity_elements) {
        old_size = buffer->capacity_elements * buffer->element_size;
        new_size = (size_t)num_elements * buffer->element_size;
        resized = realloc(buffer->data, new_size);
        if (!resized) {
            pthread_mutex_unlock(&backend->mutex);
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
        memset(resized + old_size, 0, new_size - old_size);
        buffer->data = resized;
        buffer->capacity_elements = num_elements;
    }

    buffer->num_elements = num_elements;
    pthread_mutex_unlock(&backend->mutex);
    return VA_STATUS_SUCCESS;
}

static VAStatus backend_map_buffer(VADriverContextP context,
                                   VABufferID buffer_id,
                                   void **mapped)
{
    struct venus_backend *backend =
        venus_backend_from_context(context);
    struct venus_buffer *buffer;

    if (!backend || !mapped)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    pthread_mutex_lock(&backend->mutex);
    buffer = venus_backend_find_buffer(backend, buffer_id);
    if (!buffer) {
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }

    *mapped = buffer->data;
    pthread_mutex_unlock(&backend->mutex);
    return VA_STATUS_SUCCESS;
}

static VAStatus backend_unmap_buffer(VADriverContextP context,
                                     VABufferID buffer_id)
{
    struct venus_backend *backend =
        venus_backend_from_context(context);
    VAStatus status;

    if (!backend)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    pthread_mutex_lock(&backend->mutex);
    status = venus_backend_find_buffer(backend, buffer_id)
                 ? VA_STATUS_SUCCESS
                 : VA_STATUS_ERROR_INVALID_BUFFER;
    pthread_mutex_unlock(&backend->mutex);
    return status;
}

static bool buffer_used_by_image(struct venus_backend *backend,
                                 VABufferID buffer_id)
{
    unsigned int index;

    for (index = 0; index < VENUS_MAX_IMAGES; index++) {
        if (backend->images[index].used &&
            backend->images[index].buffer_id == buffer_id)
            return true;
    }

    return false;
}

static VAStatus backend_destroy_buffer(VADriverContextP context,
                                       VABufferID buffer_id)
{
    struct venus_backend *backend =
        venus_backend_from_context(context);
    struct venus_buffer *buffer;

    if (!backend)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    pthread_mutex_lock(&backend->mutex);
    buffer = venus_backend_find_buffer(backend, buffer_id);
    if (!buffer || buffer_used_by_image(backend, buffer_id)) {
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }

    venus_backend_free_buffer(buffer);
    pthread_mutex_unlock(&backend->mutex);
    return VA_STATUS_SUCCESS;
}

static VAStatus backend_buffer_info(
    VADriverContextP context, VABufferID buffer_id,
    VABufferType *type, unsigned int *size,
    unsigned int *num_elements)
{
    struct venus_backend *backend =
        venus_backend_from_context(context);
    struct venus_buffer *buffer;

    if (!backend || !type || !size || !num_elements)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    pthread_mutex_lock(&backend->mutex);
    buffer = venus_backend_find_buffer(backend, buffer_id);
    if (!buffer || buffer->element_size > UINT_MAX ||
        buffer->num_elements > UINT_MAX) {
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }

    *type = buffer->type;
    *size = (unsigned int)buffer->element_size;
    *num_elements = (unsigned int)buffer->num_elements;
    pthread_mutex_unlock(&backend->mutex);
    return VA_STATUS_SUCCESS;
}

static VAImageFormat nv12_image_format(void)
{
    return (VAImageFormat) {
        .fourcc = VA_FOURCC_NV12,
        .byte_order = VA_LSB_FIRST,
        .bits_per_pixel = 12,
        .depth = 12,
    };
}

static VAStatus backend_query_image_formats(
    VADriverContextP context, VAImageFormat *formats,
    int *num_formats)
{
    struct venus_backend *backend =
        venus_backend_from_context(context);

    if (!backend || !num_formats)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    if (formats)
        formats[0] = nv12_image_format();
    *num_formats = 1;
    return VA_STATUS_SUCCESS;
}

static VAStatus create_image_locked(
    struct venus_backend *backend, unsigned int width,
    unsigned int height, uint8_t *external_data,
    size_t external_size, VASurfaceID surface_id,
    VAImage *result)
{
    struct venus_buffer *buffer = NULL;
    struct venus_image *image = NULL;
    size_t data_size;
    unsigned int index;

    if (!result || nv12_size(width, height, &data_size) < 0)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (external_data && external_size < data_size)
        return VA_STATUS_ERROR_NOT_ENOUGH_BUFFER;

    buffer = allocate_buffer(
        backend, VA_INVALID_ID, VAImageBufferType,
        external_data ? external_size : data_size, 1,
        external_data, !external_data);
    if (!buffer)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;

    for (index = 0; index < VENUS_MAX_IMAGES; index++) {
        if (!backend->images[index].used) {
            image = &backend->images[index];
            break;
        }
    }
    if (!image) {
        venus_backend_free_buffer(buffer);
        return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
    }

    *image = (struct venus_image) {
        .used = true,
        .id = VENUS_IMAGE_BASE | (index + 1),
        .buffer_id = buffer->id,
        .surface_id = surface_id,
    };

    memset(result, 0, sizeof(*result));
    result->image_id = image->id;
    result->format = nv12_image_format();
    result->buf = buffer->id;
    result->width = (uint16_t)width;
    result->height = (uint16_t)height;
    result->data_size = (uint32_t)data_size;
    result->num_planes = 2;
    result->pitches[0] = width;
    result->pitches[1] = width;
    result->offsets[0] = 0;
    result->offsets[1] = width * height;
    return VA_STATUS_SUCCESS;
}

static VAStatus backend_create_image(
    VADriverContextP context, VAImageFormat *format,
    int width, int height, VAImage *image)
{
    struct venus_backend *backend =
        venus_backend_from_context(context);
    VAStatus status;

    if (!backend || !format || width < 0 || height < 0 ||
        format->fourcc != VA_FOURCC_NV12)
        return VA_STATUS_ERROR_INVALID_IMAGE_FORMAT;

    pthread_mutex_lock(&backend->mutex);
    status = create_image_locked(
        backend, (unsigned int)width, (unsigned int)height,
        NULL, 0, VA_INVALID_ID, image);
    pthread_mutex_unlock(&backend->mutex);
    return status;
}

static VAStatus backend_derive_image(VADriverContextP context,
                                     VASurfaceID surface_id,
                                     VAImage *image)
{
    struct venus_backend *backend =
        venus_backend_from_context(context);
    struct venus_surface *surface;
    VAStatus status;

    if (!backend || !image)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    pthread_mutex_lock(&backend->mutex);
    surface = venus_backend_find_surface(backend, surface_id);
    if (!surface) {
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    if (!surface->ready) {
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_ERROR_SURFACE_BUSY;
    }

    status = create_image_locked(
        backend, surface->width, surface->height,
        surface->data, surface->capacity, surface->id, image);
    if (status == VA_STATUS_SUCCESS)
        venus_backend_log(backend,
                          "derive-image surface=0x%x image=0x%x bytes=%u",
                          surface->id, image->image_id, image->data_size);
    pthread_mutex_unlock(&backend->mutex);
    return status;
}

static VAStatus backend_destroy_image(VADriverContextP context,
                                      VAImageID image_id)
{
    struct venus_backend *backend =
        venus_backend_from_context(context);
    struct venus_image *image;
    struct venus_buffer *buffer;

    if (!backend)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    pthread_mutex_lock(&backend->mutex);
    image = venus_backend_find_image(backend, image_id);
    if (!image) {
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_ERROR_INVALID_IMAGE;
    }

    buffer = venus_backend_find_buffer(backend, image->buffer_id);
    venus_backend_free_buffer(buffer);
    memset(image, 0, sizeof(*image));
    pthread_mutex_unlock(&backend->mutex);
    return VA_STATUS_SUCCESS;
}

static VAStatus backend_get_image(
    VADriverContextP context, VASurfaceID surface_id,
    int x, int y, unsigned int width, unsigned int height,
    VAImageID image_id)
{
    struct venus_backend *backend =
        venus_backend_from_context(context);
    struct venus_surface *surface;
    struct venus_image *image;
    struct venus_buffer *buffer;
    size_t copy_size;

    if (!backend)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    pthread_mutex_lock(&backend->mutex);
    surface = venus_backend_find_surface(backend, surface_id);
    image = venus_backend_find_image(backend, image_id);
    buffer = image
                 ? venus_backend_find_buffer(backend, image->buffer_id)
                 : NULL;
    if (!surface || !image || !buffer) {
        pthread_mutex_unlock(&backend->mutex);
        return !surface ? VA_STATUS_ERROR_INVALID_SURFACE
                        : VA_STATUS_ERROR_INVALID_IMAGE;
    }
    if (!surface->ready) {
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_ERROR_SURFACE_BUSY;
    }
    if (x != 0 || y != 0 || width != surface->width ||
        height != surface->height ||
        nv12_size(width, height, &copy_size) < 0 ||
        copy_size > surface->data_size ||
        copy_size > buffer->element_size * buffer->num_elements) {
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    memcpy(buffer->data, surface->data, copy_size);
    pthread_mutex_unlock(&backend->mutex);
    return VA_STATUS_SUCCESS;
}

static VAStatus backend_put_image(
    VADriverContextP context, VASurfaceID surface_id,
    VAImageID image_id, int src_x, int src_y,
    unsigned int src_width, unsigned int src_height,
    int dst_x, int dst_y, unsigned int dst_width,
    unsigned int dst_height)
{
    struct venus_backend *backend =
        venus_backend_from_context(context);
    struct venus_surface *surface;
    struct venus_image *image;
    struct venus_buffer *buffer;
    size_t copy_size;

    if (!backend)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    pthread_mutex_lock(&backend->mutex);
    surface = venus_backend_find_surface(backend, surface_id);
    image = venus_backend_find_image(backend, image_id);
    buffer = image
                 ? venus_backend_find_buffer(backend, image->buffer_id)
                 : NULL;
    if (!surface || !image || !buffer) {
        pthread_mutex_unlock(&backend->mutex);
        return !surface ? VA_STATUS_ERROR_INVALID_SURFACE
                        : VA_STATUS_ERROR_INVALID_IMAGE;
    }
    if (src_x != 0 || src_y != 0 || dst_x != 0 || dst_y != 0 ||
        src_width != surface->width ||
        src_height != surface->height ||
        dst_width != surface->width ||
        dst_height != surface->height ||
        nv12_size(surface->width, surface->height, &copy_size) < 0 ||
        copy_size > buffer->element_size * buffer->num_elements ||
        copy_size > surface->capacity) {
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    memcpy(surface->data, buffer->data, copy_size);
    surface->data_size = copy_size;
    surface->ready = true;
    pthread_mutex_unlock(&backend->mutex);
    return VA_STATUS_SUCCESS;
}

static VAStatus backend_query_surface_attributes(
    VADriverContextP context, VAConfigID config_id,
    VASurfaceAttrib *attributes, unsigned int *num_attributes)
{
    struct venus_backend *backend =
        venus_backend_from_context(context);
    struct venus_config *config;
    VASurfaceAttrib values[6];
    unsigned int required =
        (unsigned int)(sizeof(values) / sizeof(values[0]));

    if (!backend || !num_attributes)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    pthread_mutex_lock(&backend->mutex);
    config = venus_backend_find_config(backend, config_id);
    if (!config) {
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_ERROR_INVALID_CONFIG;
    }

    values[0] = (VASurfaceAttrib) {
        .type = VASurfaceAttribPixelFormat,
        .flags = VA_SURFACE_ATTRIB_GETTABLE |
                 VA_SURFACE_ATTRIB_SETTABLE,
        .value = {
            .type = VAGenericValueTypeInteger,
            .value.i = VA_FOURCC_NV12,
        },
    };
    values[1] = (VASurfaceAttrib) {
        .type = VASurfaceAttribMemoryType,
        .flags = VA_SURFACE_ATTRIB_GETTABLE |
                 VA_SURFACE_ATTRIB_SETTABLE,
        .value = {
            .type = VAGenericValueTypeInteger,
            .value.i = VA_SURFACE_ATTRIB_MEM_TYPE_VA,
        },
    };
    values[2] = (VASurfaceAttrib) {
        .type = VASurfaceAttribMinWidth,
        .flags = VA_SURFACE_ATTRIB_GETTABLE,
        .value = {
            .type = VAGenericValueTypeInteger,
            .value.i = VENUS_MIN_WIDTH,
        },
    };
    values[3] = (VASurfaceAttrib) {
        .type = VASurfaceAttribMinHeight,
        .flags = VA_SURFACE_ATTRIB_GETTABLE,
        .value = {
            .type = VAGenericValueTypeInteger,
            .value.i = VENUS_MIN_HEIGHT,
        },
    };
    values[4] = (VASurfaceAttrib) {
        .type = VASurfaceAttribMaxWidth,
        .flags = VA_SURFACE_ATTRIB_GETTABLE,
        .value = {
            .type = VAGenericValueTypeInteger,
            .value.i = VENUS_MAX_WIDTH,
        },
    };
    values[5] = (VASurfaceAttrib) {
        .type = VASurfaceAttribMaxHeight,
        .flags = VA_SURFACE_ATTRIB_GETTABLE,
        .value = {
            .type = VAGenericValueTypeInteger,
            .value.i = VENUS_MAX_HEIGHT,
        },
    };

    if (!attributes) {
        *num_attributes = required;
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_SUCCESS;
    }
    if (*num_attributes < required) {
        *num_attributes = required;
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
    }

    memcpy(attributes, values, sizeof(values));
    *num_attributes = required;
    pthread_mutex_unlock(&backend->mutex);
    return VA_STATUS_SUCCESS;
}

void venus_objects_destroy_all(struct venus_backend *backend)
{
    unsigned int index;

    for (index = 0; index < VENUS_MAX_IMAGES; index++)
        memset(&backend->images[index], 0,
               sizeof(backend->images[index]));
    for (index = 0; index < VENUS_MAX_BUFFERS; index++)
        venus_backend_free_buffer(&backend->buffers[index]);
    for (index = 0; index < VENUS_MAX_SURFACES; index++) {
        free(backend->surfaces[index].data);
        memset(&backend->surfaces[index], 0,
               sizeof(backend->surfaces[index]));
    }
    memset(backend->configs, 0, sizeof(backend->configs));
}

void venus_objects_fill_vtable(struct VADriverVTable *vtable)
{
    vtable->vaCreateSurfaces = backend_create_surfaces;
    vtable->vaCreateSurfaces2 = backend_create_surfaces2;
    vtable->vaDestroySurfaces = backend_destroy_surfaces;
    vtable->vaCreateBuffer = backend_create_buffer;
    vtable->vaBufferSetNumElements = backend_set_num_elements;
    vtable->vaMapBuffer = backend_map_buffer;
    vtable->vaUnmapBuffer = backend_unmap_buffer;
    vtable->vaDestroyBuffer = backend_destroy_buffer;
    vtable->vaBufferInfo = backend_buffer_info;
    vtable->vaQueryImageFormats = backend_query_image_formats;
    vtable->vaCreateImage = backend_create_image;
    vtable->vaDeriveImage = backend_derive_image;
    vtable->vaDestroyImage = backend_destroy_image;
    vtable->vaGetImage = backend_get_image;
    vtable->vaPutImage = backend_put_image;
    vtable->vaQuerySurfaceAttributes =
        backend_query_surface_attributes;
}
