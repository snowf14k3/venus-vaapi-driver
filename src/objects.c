// SPDX-License-Identifier: MIT
#include "backend_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <va/va_drmcommon.h>
#include <drm_fourcc.h>

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

static int xioctl(int fd, unsigned long request, void *argument)
{
    int result;

    do {
        result = ioctl(fd, request, argument);
    } while (result < 0 && errno == EINTR);
    return result;
}

static int align_size(size_t value, size_t alignment,
                      size_t *result)
{
    size_t remainder;

    if (alignment == 0)
        return -EINVAL;
    remainder = value % alignment;
    if (remainder == 0) {
        *result = value;
        return 0;
    }
    if (value > SIZE_MAX - (alignment - remainder))
        return -EOVERFLOW;
    *result = value + alignment - remainder;
    return 0;
}

static void release_surface(struct venus_surface *surface)
{
    if (!surface)
        return;

    if (surface->dma_backed) {
        if (surface->data && surface->capacity)
            munmap(surface->data, surface->capacity);
        if (surface->dma_fd >= 0)
            close(surface->dma_fd);
    } else {
        free(surface->data);
    }
    memset(surface, 0, sizeof(*surface));
}

static int allocate_dma_surface(struct venus_surface *surface,
                                unsigned int width,
                                unsigned int height)
{
    static const char *const heap_paths[] = {
        "/dev/dma_heap/system",
        "/dev/dma_heap/linux,cma",
        "/dev/dma_heap/default_cma_region",
    };
    struct dma_heap_allocation_data allocation = { 0 };
    size_t stride;
    size_t scanlines;
    size_t chroma_scanlines;
    size_t uv_offset;
    size_t allocation_size;
    unsigned int index;
    int heap_fd = -1;
    int saved_errno;

    if (align_size(width, 128, &stride) < 0 ||
        align_size(height, 32, &scanlines) < 0 ||
        align_size(height / 2, 16, &chroma_scanlines) < 0 ||
        stride > SIZE_MAX / scanlines)
        return -EOVERFLOW;

    uv_offset = stride * scanlines;
    if (stride > SIZE_MAX / chroma_scanlines ||
        uv_offset >
            SIZE_MAX - stride * chroma_scanlines)
        return -EOVERFLOW;
    allocation_size =
        uv_offset + stride * chroma_scanlines;
    if (align_size(allocation_size, 4096,
                   &allocation_size) < 0 ||
        allocation_size > UINT32_MAX)
        return -EOVERFLOW;

    for (index = 0;
         index < sizeof(heap_paths) / sizeof(heap_paths[0]);
         index++) {
        heap_fd = open(heap_paths[index],
                       O_RDWR | O_CLOEXEC);
        if (heap_fd >= 0)
            break;
    }
    if (heap_fd < 0)
        return -errno;

    allocation.len = allocation_size;
    allocation.fd_flags = O_RDWR | O_CLOEXEC;
    if (xioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC,
               &allocation) < 0) {
        saved_errno = errno;
        close(heap_fd);
        return -saved_errno;
    }
    close(heap_fd);

    surface->data =
        mmap(NULL, allocation_size, PROT_READ | PROT_WRITE,
             MAP_SHARED, (int)allocation.fd, 0);
    if (surface->data == MAP_FAILED) {
        saved_errno = errno;
        surface->data = NULL;
        close((int)allocation.fd);
        return -saved_errno;
    }

    memset(surface->data, 0, allocation_size);
    surface->dma_fd = (int)allocation.fd;
    surface->dma_backed = true;
    surface->stride = (uint32_t)stride;
    surface->scanlines = (uint32_t)scanlines;
    surface->uv_offset = uv_offset;
    surface->capacity = allocation_size;
    surface->data_size = allocation_size;
    return 0;
}

static int surface_cpu_sync(struct venus_surface *surface,
                            uint64_t flags)
{
    struct dma_buf_sync sync = {
        .flags = flags,
    };

    if (!surface || !surface->dma_backed)
        return 0;
    if (surface->dma_fd < 0)
        return -EBADF;
    if (xioctl(surface->dma_fd, DMA_BUF_IOCTL_SYNC,
               &sync) < 0)
        return -errno;
    return 0;
}

int venus_surface_begin_cpu_read(struct venus_surface *surface)
{
    return surface_cpu_sync(
        surface, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
}

int venus_surface_end_cpu_read(struct venus_surface *surface)
{
    return surface_cpu_sync(
        surface, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
}

int venus_surface_begin_cpu_write(struct venus_surface *surface)
{
    return surface_cpu_sync(
        surface, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
}

int venus_surface_end_cpu_write(struct venus_surface *surface)
{
    return surface_cpu_sync(
        surface, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
}

int venus_surface_begin_cpu_rw(struct venus_surface *surface)
{
    return surface_cpu_sync(
        surface, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
}

int venus_surface_end_cpu_rw(struct venus_surface *surface)
{
    return surface_cpu_sync(
        surface, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);
}

int venus_surface_copy_from_nv12(
    struct venus_surface *surface, const uint8_t *source,
    size_t source_size, uint32_t source_stride,
    size_t source_uv_offset, unsigned int width,
    unsigned int height)
{
    uint32_t destination_stride;
    size_t destination_uv_offset;
    size_t source_required;
    size_t destination_required;
    unsigned int row;
    int status;
    int end_status;

    if (!surface || !source || !surface->data ||
        width == 0 || height == 0 || (width & 1u) ||
        (height & 1u) || width > surface->width ||
        height > surface->height || source_stride < width)
        return -EINVAL;
    if (source_stride > SIZE_MAX / height ||
        source_uv_offset < (size_t)source_stride * height ||
        source_stride > SIZE_MAX / (height / 2u))
        return -EOVERFLOW;

    source_required =
        source_uv_offset + (size_t)source_stride * (height / 2u);
    destination_stride =
        surface->stride ? surface->stride : surface->width;
    destination_uv_offset =
        surface->uv_offset
            ? surface->uv_offset
            : (size_t)destination_stride * surface->height;
    if (destination_stride < width ||
        destination_stride > SIZE_MAX / (height / 2u) ||
        destination_uv_offset >
            SIZE_MAX - (size_t)destination_stride *
                           (height / 2u))
        return -EOVERFLOW;

    destination_required =
        destination_uv_offset +
        (size_t)destination_stride * (height / 2u);
    if (source_required > source_size ||
        destination_required > surface->capacity)
        return -ENOSPC;

    status = venus_surface_begin_cpu_write(surface);
    if (status < 0)
        return status;

    memset(surface->data, 0, surface->capacity);
    for (row = 0; row < height; row++)
        memcpy(surface->data +
                   (size_t)row * destination_stride,
               source + (size_t)row * source_stride, width);
    for (row = 0; row < height / 2u; row++)
        memcpy(surface->data + destination_uv_offset +
                   (size_t)row * destination_stride,
               source + source_uv_offset +
                   (size_t)row * source_stride,
               width);

    surface->data_size = destination_required;
    surface->ready = true;
    end_status = venus_surface_end_cpu_write(surface);
    return end_status < 0 ? end_status : 0;
}

int venus_surface_copy_to_nv12(
    struct venus_surface *surface, uint8_t *destination,
    size_t destination_size, uint32_t destination_stride,
    size_t destination_uv_offset, unsigned int width,
    unsigned int height)
{
    uint32_t source_stride;
    size_t source_uv_offset;
    size_t source_required;
    size_t destination_required;
    unsigned int row;
    int status;
    int end_status;

    if (!surface || !destination || !surface->data ||
        width == 0 || height == 0 || (width & 1u) ||
        (height & 1u) || width > surface->width ||
        height > surface->height || destination_stride < width)
        return -EINVAL;

    source_stride =
        surface->stride ? surface->stride : surface->width;
    source_uv_offset =
        surface->uv_offset
            ? surface->uv_offset
            : (size_t)source_stride * surface->height;
    if (source_stride < width ||
        source_stride > SIZE_MAX / (height / 2u) ||
        source_uv_offset >
            SIZE_MAX - (size_t)source_stride *
                           (height / 2u) ||
        destination_stride > SIZE_MAX / height ||
        destination_uv_offset <
            (size_t)destination_stride * height ||
        destination_stride > SIZE_MAX / (height / 2u) ||
        destination_uv_offset >
            SIZE_MAX - (size_t)destination_stride *
                           (height / 2u))
        return -EOVERFLOW;

    source_required =
        source_uv_offset + (size_t)source_stride * (height / 2u);
    destination_required =
        destination_uv_offset +
        (size_t)destination_stride * (height / 2u);
    if (source_required > surface->data_size ||
        destination_required > destination_size)
        return -ENOSPC;

    status = venus_surface_begin_cpu_read(surface);
    if (status < 0)
        return status;

    memset(destination, 0, destination_size);
    for (row = 0; row < height; row++)
        memcpy(destination +
                   (size_t)row * destination_stride,
               surface->data + (size_t)row * source_stride,
               width);
    for (row = 0; row < height / 2u; row++)
        memcpy(destination + destination_uv_offset +
                   (size_t)row * destination_stride,
               surface->data + source_uv_offset +
                   (size_t)row * source_stride,
               width);

    end_status = venus_surface_end_cpu_read(surface);
    return end_status < 0 ? end_status : 0;
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
            .source_surface_id = VA_INVALID_ID,
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

static bool valid_surface_attributes(
    VASurfaceAttrib *attributes, unsigned int num_attributes,
    uint32_t *fourcc, bool *exportable)
{
    unsigned int index;

    *fourcc = VA_FOURCC_NV12;
    *exportable = false;
    for (index = 0; index < num_attributes; index++) {
        const VASurfaceAttrib *attribute = &attributes[index];

        if (attribute->type == VASurfaceAttribPixelFormat) {
            if (attribute->value.type != VAGenericValueTypeInteger ||
                attribute->value.value.i != VA_FOURCC_NV12)
                return false;
            *fourcc = (uint32_t)attribute->value.value.i;
        } else if (attribute->type == VASurfaceAttribMemoryType) {
            int memory_type;

            if (attribute->value.type != VAGenericValueTypeInteger)
                return false;
            memory_type = attribute->value.value.i;
            if (!(memory_type &
                  (VA_SURFACE_ATTRIB_MEM_TYPE_VA |
                   VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2)))
                return false;
            if (memory_type &
                VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2)
                *exportable = true;
        } else if (attribute->type ==
                   VASurfaceAttribUsageHint) {
            if (attribute->value.type !=
                VAGenericValueTypeInteger)
                return false;
            if (attribute->value.value.i &
                VA_SURFACE_ATTRIB_USAGE_HINT_EXPORT)
                *exportable = true;
        } else if (attribute->flags &
                   VA_SURFACE_ATTRIB_SETTABLE) {
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
    bool exportable;
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
            attributes, num_attributes, &fourcc,
            &exportable))
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

        surface->dma_fd = -1;
        if (exportable) {
            if (allocate_dma_surface(
                    surface, width, height) < 0) {
                release_surface(surface);
                goto fail;
            }
        } else {
            surface->data = calloc(1, capacity);
            if (!surface->data) {
                release_surface(surface);
                goto fail;
            }
            surface->capacity = capacity;
            surface->data_size = capacity;
            surface->stride = width;
            surface->scanlines = height;
            surface->uv_offset = (size_t)width * height;
        }

        surface->used = true;
        surface->id = VENUS_SURFACE_BASE | (index + 1);
        surface->width = width;
        surface->height = height;
        surface->fourcc = fourcc;
        surface->ready = true;
        surface->coded_buffer_id = VA_INVALID_ID;
        surface->context_id = VA_INVALID_ID;
        created[created_count] = surface;
        surface_ids[created_count] = surface->id;
        venus_backend_log(
            backend,
            "create-surface id=0x%x size=%ux%u bytes=%zu stride=%u dma=%s",
            surface->id, width, height, surface->capacity,
            surface->stride,
            surface->dma_backed ? "yes" : "no");
        created_count++;
    }

    return VA_STATUS_SUCCESS;

fail:
    while (created_count > 0) {
        struct venus_surface *surface = created[--created_count];

        release_surface(surface);
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

static bool surface_has_image(struct venus_backend *backend,
                              VASurfaceID surface_id)
{
    unsigned int index;

    for (index = 0; index < VENUS_MAX_IMAGES; index++) {
        if (backend->images[index].used &&
            backend->images[index].surface_id == surface_id)
            return true;
    }

    return false;
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
        struct venus_surface *surface =
            venus_backend_find_surface(backend, surface_ids[index]);

        if (!surface) {
            pthread_mutex_unlock(&backend->mutex);
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }
        if (surface->encode_pending ||
            surface->context_id != VA_INVALID_ID ||
            surface_has_image(backend, surface->id)) {
            pthread_mutex_unlock(&backend->mutex);
            return VA_STATUS_ERROR_SURFACE_BUSY;
        }
    }

    for (index = 0; index < num_surfaces; index++) {
        struct venus_surface *surface =
            venus_backend_find_surface(backend, surface_ids[index]);

        release_surface(surface);
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

    if (type == VAEncCodedBufferType) {
        buffer->coded_segment.buf = buffer->data;
        buffer->coded_segment.next = NULL;
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
    if (!buffer || !buffer->owns_data ||
        buffer->type == VAEncCodedBufferType) {
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

static struct venus_surface *surface_for_image_buffer(
    struct venus_backend *backend, VABufferID buffer_id)
{
    unsigned int index;

    for (index = 0; index < VENUS_MAX_IMAGES; index++) {
        struct venus_image *image = &backend->images[index];

        if (!image->used || image->buffer_id != buffer_id ||
            image->surface_id == VA_INVALID_ID)
            continue;
        return venus_backend_find_surface(
            backend, image->surface_id);
    }

    return NULL;
}

static VAStatus backend_map_buffer(VADriverContextP context,
                                   VABufferID buffer_id,
                                   void **mapped)
{
    struct venus_backend *backend =
        venus_backend_from_context(context);
    struct venus_surface *surface;
    struct venus_buffer *buffer;
    int status;

    if (!backend || !mapped)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    pthread_mutex_lock(&backend->mutex);
    buffer = venus_backend_find_buffer(backend, buffer_id);
    if (!buffer || buffer->map_count == UINT_MAX) {
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }

    surface = surface_for_image_buffer(backend, buffer_id);
    if (surface && buffer->map_count == 0) {
        status = venus_surface_begin_cpu_rw(surface);
        if (status < 0) {
            pthread_mutex_unlock(&backend->mutex);
            return venus_backend_status_from_errno(status);
        }
    }

    buffer->map_count++;
    *mapped = buffer->type == VAEncCodedBufferType
                  ? (void *)&buffer->coded_segment
                  : (void *)buffer->data;
    pthread_mutex_unlock(&backend->mutex);
    return VA_STATUS_SUCCESS;
}

static VAStatus backend_unmap_buffer(VADriverContextP context,
                                     VABufferID buffer_id)
{
    struct venus_backend *backend =
        venus_backend_from_context(context);
    struct venus_surface *surface;
    struct venus_buffer *buffer;
    VAStatus result = VA_STATUS_SUCCESS;
    int status;

    if (!backend)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    pthread_mutex_lock(&backend->mutex);
    buffer = venus_backend_find_buffer(backend, buffer_id);
    if (!buffer || buffer->map_count == 0) {
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }

    surface = surface_for_image_buffer(backend, buffer_id);
    buffer->map_count--;
    if (surface && buffer->map_count == 0) {
        status = venus_surface_end_cpu_rw(surface);
        if (status < 0)
            result = venus_backend_status_from_errno(status);
    }

    pthread_mutex_unlock(&backend->mutex);
    return result;
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
    if (!buffer || buffer->map_count > 0 ||
        buffer_used_by_image(backend, buffer_id)) {
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }

    if (buffer->type == VAEncCodedBufferType &&
        buffer->source_surface_id != VA_INVALID_ID) {
        struct venus_surface *surface = venus_backend_find_surface(
            backend, buffer->source_surface_id);

        if (surface && surface->coded_buffer_id == buffer->id) {
            surface->encode_pending = false;
            surface->coded_buffer_id = VA_INVALID_ID;
        }
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
    size_t external_size, uint32_t stride,
    size_t uv_offset, VASurfaceID surface_id,
    VAImage *result)
{
    struct venus_buffer *buffer = NULL;
    struct venus_image *image = NULL;
    size_t data_size;
    unsigned int index;

    if (!result || nv12_size(width, height, &data_size) < 0 ||
        stride < width ||
        uv_offset < (size_t)stride * height)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (external_data && external_size <
            uv_offset + (size_t)stride * (height / 2))
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
        .width = width,
        .height = height,
        .stride = stride,
        .uv_offset = uv_offset,
    };

    memset(result, 0, sizeof(*result));
    result->image_id = image->id;
    result->format = nv12_image_format();
    result->buf = buffer->id;
    result->width = (uint16_t)width;
    result->height = (uint16_t)height;
    result->data_size = (uint32_t)(
        external_data ? external_size : data_size);
    result->num_planes = 2;
    result->pitches[0] = stride;
    result->pitches[1] = stride;
    result->offsets[0] = 0;
    result->offsets[1] = (uint32_t)uv_offset;
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
        NULL, 0, (uint32_t)width,
        (size_t)width * (unsigned int)height,
        VA_INVALID_ID, image);
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
        surface->data, surface->capacity,
        surface->stride, surface->uv_offset,
        surface->id, image);
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
    if (!buffer || buffer->map_count > 0) {
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_ERROR_SURFACE_BUSY;
    }
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
    size_t buffer_size;
    int status;

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
        image->width != width || image->height != height ||
        buffer->map_count > 0 ||
        buffer->element_size >
            SIZE_MAX / buffer->num_elements) {
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    buffer_size =
        buffer->element_size * buffer->num_elements;
    status = venus_surface_copy_to_nv12(
        surface, buffer->data, buffer_size,
        image->stride, image->uv_offset, width, height);
    pthread_mutex_unlock(&backend->mutex);
    return status < 0
               ? venus_backend_status_from_errno(status)
               : VA_STATUS_SUCCESS;
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
    size_t buffer_size;
    int status;

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
    if (surface->encode_pending) {
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_ERROR_SURFACE_BUSY;
    }
    if (src_x != 0 || src_y != 0 || dst_x != 0 || dst_y != 0 ||
        src_width != surface->width ||
        src_height != surface->height ||
        dst_width != surface->width ||
        dst_height != surface->height ||
        image->width != src_width ||
        image->height != src_height ||
        buffer->map_count > 0 ||
        buffer->element_size >
            SIZE_MAX / buffer->num_elements) {
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    buffer_size =
        buffer->element_size * buffer->num_elements;
    status = venus_surface_copy_from_nv12(
        surface, buffer->data, buffer_size,
        image->stride, image->uv_offset,
        src_width, src_height);
    pthread_mutex_unlock(&backend->mutex);
    return status < 0
               ? venus_backend_status_from_errno(status)
               : VA_STATUS_SUCCESS;
}

static VAStatus backend_export_surface_handle(
    VADriverContextP context, VASurfaceID surface_id,
    uint32_t memory_type, uint32_t flags, void *descriptor)
{
    struct venus_backend *backend =
        venus_backend_from_context(context);
    struct venus_surface *surface;
    VADRMPRIMESurfaceDescriptor *prime = descriptor;
    int exported_fd;

    if (!backend || !descriptor)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (memory_type !=
        VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2 ||
        !(flags & VA_EXPORT_SURFACE_SEPARATE_LAYERS))
        return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;

    pthread_mutex_lock(&backend->mutex);
    surface = venus_backend_find_surface(backend, surface_id);
    if (!surface || !surface->dma_backed ||
        surface->dma_fd < 0) {
        pthread_mutex_unlock(&backend->mutex);
        return !surface ? VA_STATUS_ERROR_INVALID_SURFACE
                        : VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
    }

    exported_fd =
        fcntl(surface->dma_fd, F_DUPFD_CLOEXEC, 0);
    if (exported_fd < 0) {
        pthread_mutex_unlock(&backend->mutex);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    memset(prime, 0, sizeof(*prime));
    prime->fourcc = VA_FOURCC_NV12;
    prime->width = surface->width;
    prime->height = surface->height;
    prime->num_objects = 1;
    prime->objects[0].fd = exported_fd;
    prime->objects[0].size = (uint32_t)surface->capacity;
    prime->objects[0].drm_format_modifier =
        DRM_FORMAT_MOD_LINEAR;
    prime->num_layers = 2;

    prime->layers[0].drm_format = DRM_FORMAT_R8;
    prime->layers[0].num_planes = 1;
    prime->layers[0].object_index[0] = 0;
    prime->layers[0].offset[0] = 0;
    prime->layers[0].pitch[0] = surface->stride;

    prime->layers[1].drm_format = DRM_FORMAT_GR88;
    prime->layers[1].num_planes = 1;
    prime->layers[1].object_index[0] = 0;
    prime->layers[1].offset[0] =
        (uint32_t)surface->uv_offset;
    prime->layers[1].pitch[0] = surface->stride;

    venus_backend_log(
        backend,
        "export-surface id=0x%x fd=%d size=%zu stride=%u uv=%zu",
        surface->id, exported_fd, surface->capacity,
        surface->stride, surface->uv_offset);
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
    VASurfaceAttrib values[7];
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
            .value.i = VA_SURFACE_ATTRIB_MEM_TYPE_VA |
                       VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
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
    values[6] = (VASurfaceAttrib) {
        .type = VASurfaceAttribUsageHint,
        .flags = VA_SURFACE_ATTRIB_GETTABLE |
                 VA_SURFACE_ATTRIB_SETTABLE,
        .value = {
            .type = VAGenericValueTypeInteger,
            .value.i = VA_SURFACE_ATTRIB_USAGE_HINT_EXPORT |
                       VA_SURFACE_ATTRIB_USAGE_HINT_ENCODER,
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
    for (index = 0; index < VENUS_MAX_SURFACES; index++)
        release_surface(&backend->surfaces[index]);
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
    vtable->vaExportSurfaceHandle =
        backend_export_surface_handle;
}
