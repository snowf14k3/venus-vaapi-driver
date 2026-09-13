// SPDX-License-Identifier: MIT
#ifndef VENUS_BACKEND_INTERNAL_H
#define VENUS_BACKEND_INTERNAL_H

#include "venus/capabilities.h"
#include "v4l2_decoder.h"
#include "v4l2_encoder.h"

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <va/va_backend.h>
#include <va/va_enc_h264.h>

#define VENUS_MAX_CONFIGS 16
#define VENUS_MAX_CONTEXTS 8
#define VENUS_MAX_SURFACES 64
#define VENUS_MAX_BUFFERS 512
#define VENUS_MAX_IMAGES 128
#define VENUS_MAX_PENDING_BUFFERS 128

#define VENUS_CONFIG_BASE 0x01000000u
#define VENUS_CONTEXT_BASE 0x02000000u
#define VENUS_SURFACE_BASE 0x03000000u
#define VENUS_BUFFER_BASE 0x04000000u
#define VENUS_IMAGE_BASE 0x05000000u

#define VENUS_MIN_WIDTH 48u
#define VENUS_MIN_HEIGHT 32u
#define VENUS_MAX_WIDTH 4096u
#define VENUS_MAX_HEIGHT 4096u
#define VENUS_H264_MAX_MACROBLOCKS 36864u
#define VENUS_H264_MAX_MACROBLOCKS_PER_SECOND 1036800u
#define VENUS_H264_PACKED_HEADERS \
    (VA_ENC_PACKED_HEADER_SEQUENCE | VA_ENC_PACKED_HEADER_PICTURE | \
     VA_ENC_PACKED_HEADER_SLICE | VA_ENC_PACKED_HEADER_RAW_DATA)

struct venus_config {
    bool used;
    VAConfigID id;
    VAProfile profile;
    VAEntrypoint entrypoint;
    uint32_t rate_control;
    uint32_t packed_headers;
};

struct venus_surface {
    bool used;
    VASurfaceID id;
    unsigned int width;
    unsigned int height;
    uint32_t fourcc;
    uint8_t *data;
    size_t capacity;
    size_t data_size;
    uint32_t stride;
    uint32_t scanlines;
    size_t uv_offset;
    int dma_fd;
    bool dma_backed;
    bool ready;
    bool encode_pending;
    VABufferID coded_buffer_id;
    VAContextID context_id;
};

struct venus_buffer {
    bool used;
    VABufferID id;
    VAContextID context_id;
    VABufferType type;
    size_t element_size;
    size_t capacity_elements;
    size_t num_elements;
    uint8_t *data;
    bool owns_data;
    bool coded_ready;
    size_t coded_size;
    VASurfaceID source_surface_id;
    VACodedBufferSegment coded_segment;
};

struct venus_image {
    bool used;
    VAImageID id;
    VABufferID buffer_id;
    VASurfaceID surface_id;
    unsigned int width;
    unsigned int height;
    uint32_t stride;
    size_t uv_offset;
};

struct venus_context {
    bool used;
    VAContextID id;
    VAConfigID config_id;
    unsigned int width;
    unsigned int height;
    unsigned int encode_width;
    unsigned int encode_height;
    struct venus_backend *backend;
    struct venus_v4l2_decoder *decoder;
    struct venus_v4l2_encoder *encoder;
    bool in_picture;
    VASurfaceID target;
    VABufferID pending[VENUS_MAX_PENDING_BUFFERS];
    size_t pending_count;
    VABufferID encode_queue[VENUS_MAX_SURFACES];
    size_t encode_queue_head;
    size_t encode_queue_count;
    uint64_t encode_sequence;
};

struct venus_backend {
    pthread_mutex_t mutex;
    struct venus_capabilities capabilities;
    bool debug;
    struct venus_config configs[VENUS_MAX_CONFIGS];
    struct venus_context contexts[VENUS_MAX_CONTEXTS];
    struct venus_surface surfaces[VENUS_MAX_SURFACES];
    struct venus_buffer buffers[VENUS_MAX_BUFFERS];
    struct venus_image images[VENUS_MAX_IMAGES];
};

struct venus_backend *venus_backend_from_context(VADriverContextP context);
void venus_backend_log(const struct venus_backend *backend,
                       const char *format, ...);
VAStatus venus_backend_status_from_errno(int status);
bool venus_backend_h264_profile(VAProfile profile);
bool venus_backend_h264_vld_supported(const struct venus_backend *backend,
                                      VAProfile profile,
                                      VAEntrypoint entrypoint);
bool venus_backend_h264_enc_supported(const struct venus_backend *backend,
                                      VAProfile profile,
                                      VAEntrypoint entrypoint);
VAStatus venus_backend_encode_status_from_errno(int status);

struct venus_config *venus_backend_find_config(struct venus_backend *backend,
                                               VAConfigID id);
struct venus_context *venus_backend_find_context(
    struct venus_backend *backend, VAContextID id);
struct venus_surface *venus_backend_find_surface(
    struct venus_backend *backend, VASurfaceID id);
struct venus_buffer *venus_backend_find_buffer(
    struct venus_backend *backend, VABufferID id);
struct venus_image *venus_backend_find_image(
    struct venus_backend *backend, VAImageID id);

void venus_backend_free_buffer(struct venus_buffer *buffer);
int venus_surface_begin_cpu_read(struct venus_surface *surface);
int venus_surface_end_cpu_read(struct venus_surface *surface);
void venus_objects_fill_vtable(struct VADriverVTable *vtable);
void venus_objects_destroy_all(struct venus_backend *backend);
void venus_decode_fill_vtable(struct VADriverVTable *vtable);
void venus_decode_destroy_all(struct venus_backend *backend);
int venus_decode_store_frame_locked(
    const struct venus_v4l2_frame *frame, void *opaque);

VAStatus venus_encode_end_picture_locked(
    struct venus_backend *backend, struct venus_context *context,
    const struct venus_config *config);
int venus_encode_queue_coded_buffer_locked(
    struct venus_context *context, VABufferID buffer_id);
int venus_encode_store_packet_locked(
    struct venus_context *context,
    const struct venus_v4l2_packet *packet);
int venus_encode_h264_dimensions(
    const VAEncSequenceParameterBufferH264 *sequence,
    uint32_t *width, uint32_t *height);
VAStatus venus_encode_sync_surface_locked(
    struct venus_backend *backend, struct venus_surface *surface,
    int timeout_ms);
VAStatus venus_encode_sync_buffer_locked(
    struct venus_backend *backend, struct venus_buffer *buffer,
    int timeout_ms);
void venus_encode_close_context(struct venus_context *context);

#endif
