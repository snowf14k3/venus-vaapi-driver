// SPDX-License-Identifier: MIT
#include "va_stubs.h"

#include <stdlib.h>

/*
 * Keep the unused VA entry points explicit.  libva expects a complete vtable,
 * while this driver only overrides the object, decode, and encode operations
 * that are implemented for the Raphael backend.
 */
static VAStatus unsupported(void)
{
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus venus_terminate(VADriverContextP context)
{
    if (!context)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    free(context->pDriverData);
    context->pDriverData = NULL;
    return VA_STATUS_SUCCESS;
}

static VAStatus venus_query_config_profiles(VADriverContextP context,
                                            VAProfile *profiles,
                                            int *num_profiles)
{
    (void)profiles;

    if (!context || !num_profiles)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    /*
     * The probe records potential hardware formats, but a profile is not
     * advertised until its VA buffer-to-V4L2 submission path is implemented.
     */
    *num_profiles = 0;
    return VA_STATUS_SUCCESS;
}

static VAStatus venus_query_config_entrypoints(VADriverContextP context,
                                               VAProfile profile,
                                               VAEntrypoint *entrypoints,
                                               int *num_entrypoints)
{
    (void)profile;
    (void)entrypoints;

    if (!context || !num_entrypoints)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    *num_entrypoints = 0;
    return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
}

static VAStatus venus_get_config_attributes(VADriverContextP context,
                                            VAProfile profile,
                                            VAEntrypoint entrypoint,
                                            VAConfigAttrib *attributes,
                                            int num_attributes)
{
    int index;

    (void)profile;
    (void)entrypoint;

    if (!context || num_attributes < 0 ||
        (num_attributes > 0 && !attributes))
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    for (index = 0; index < num_attributes; index++)
        attributes[index].value = VA_ATTRIB_NOT_SUPPORTED;

    return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
}

static VAStatus venus_create_config(VADriverContextP context,
                                    VAProfile profile,
                                    VAEntrypoint entrypoint,
                                    VAConfigAttrib *attributes,
                                    int num_attributes,
                                    VAConfigID *config_id)
{
    (void)context;
    (void)profile;
    (void)entrypoint;
    (void)attributes;
    (void)num_attributes;
    (void)config_id;
    return unsupported();
}

static VAStatus venus_destroy_config(VADriverContextP context,
                                     VAConfigID config_id)
{
    (void)context;
    (void)config_id;
    return VA_STATUS_ERROR_INVALID_CONFIG;
}

static VAStatus venus_query_config_attributes(VADriverContextP context,
                                              VAConfigID config_id,
                                              VAProfile *profile,
                                              VAEntrypoint *entrypoint,
                                              VAConfigAttrib *attributes,
                                              int *num_attributes)
{
    (void)context;
    (void)config_id;
    (void)profile;
    (void)entrypoint;
    (void)attributes;
    (void)num_attributes;
    return VA_STATUS_ERROR_INVALID_CONFIG;
}

static VAStatus venus_create_surfaces(VADriverContextP context, int width,
                                      int height, int format,
                                      int num_surfaces,
                                      VASurfaceID *surfaces)
{
    (void)context;
    (void)width;
    (void)height;
    (void)format;
    (void)num_surfaces;
    (void)surfaces;
    return unsupported();
}

static VAStatus venus_create_surfaces2(VADriverContextP context,
                                       unsigned int format,
                                       unsigned int width,
                                       unsigned int height,
                                       VASurfaceID *surfaces,
                                       unsigned int num_surfaces,
                                       VASurfaceAttrib *attributes,
                                       unsigned int num_attributes)
{
    (void)context;
    (void)format;
    (void)width;
    (void)height;
    (void)surfaces;
    (void)num_surfaces;
    (void)attributes;
    (void)num_attributes;
    return unsupported();
}

static VAStatus venus_destroy_surfaces(VADriverContextP context,
                                       VASurfaceID *surfaces,
                                       int num_surfaces)
{
    (void)context;
    (void)surfaces;
    (void)num_surfaces;
    return unsupported();
}

static VAStatus venus_create_context(VADriverContextP context,
                                     VAConfigID config_id,
                                     int picture_width,
                                     int picture_height,
                                     int flags,
                                     VASurfaceID *render_targets,
                                     int num_render_targets,
                                     VAContextID *context_id)
{
    (void)context;
    (void)config_id;
    (void)picture_width;
    (void)picture_height;
    (void)flags;
    (void)render_targets;
    (void)num_render_targets;
    (void)context_id;
    return unsupported();
}

static VAStatus venus_destroy_context(VADriverContextP context,
                                      VAContextID context_id)
{
    (void)context;
    (void)context_id;
    return VA_STATUS_ERROR_INVALID_CONTEXT;
}

static VAStatus venus_create_buffer(VADriverContextP context,
                                    VAContextID context_id,
                                    VABufferType type,
                                    unsigned int size,
                                    unsigned int num_elements,
                                    void *data,
                                    VABufferID *buffer_id)
{
    (void)context;
    (void)context_id;
    (void)type;
    (void)size;
    (void)num_elements;
    (void)data;
    (void)buffer_id;
    return unsupported();
}

static VAStatus venus_buffer_set_num_elements(VADriverContextP context,
                                              VABufferID buffer_id,
                                              unsigned int num_elements)
{
    (void)context;
    (void)buffer_id;
    (void)num_elements;
    return VA_STATUS_ERROR_INVALID_BUFFER;
}

static VAStatus venus_map_buffer(VADriverContextP context,
                                 VABufferID buffer_id, void **data)
{
    (void)context;
    (void)buffer_id;
    (void)data;
    return VA_STATUS_ERROR_INVALID_BUFFER;
}

static VAStatus venus_unmap_buffer(VADriverContextP context,
                                   VABufferID buffer_id)
{
    (void)context;
    (void)buffer_id;
    return VA_STATUS_ERROR_INVALID_BUFFER;
}

static VAStatus venus_destroy_buffer(VADriverContextP context,
                                     VABufferID buffer_id)
{
    (void)context;
    (void)buffer_id;
    return VA_STATUS_ERROR_INVALID_BUFFER;
}

static VAStatus venus_begin_picture(VADriverContextP context,
                                    VAContextID context_id,
                                    VASurfaceID render_target)
{
    (void)context;
    (void)context_id;
    (void)render_target;
    return unsupported();
}

static VAStatus venus_render_picture(VADriverContextP context,
                                     VAContextID context_id,
                                     VABufferID *buffers,
                                     int num_buffers)
{
    (void)context;
    (void)context_id;
    (void)buffers;
    (void)num_buffers;
    return unsupported();
}

static VAStatus venus_end_picture(VADriverContextP context,
                                  VAContextID context_id)
{
    (void)context;
    (void)context_id;
    return unsupported();
}

static VAStatus venus_sync_surface(VADriverContextP context,
                                   VASurfaceID surface)
{
    (void)context;
    (void)surface;
    return VA_STATUS_ERROR_INVALID_SURFACE;
}

static VAStatus venus_sync_surface2(VADriverContextP context,
                                    VASurfaceID surface,
                                    uint64_t timeout_ns)
{
    (void)timeout_ns;
    return venus_sync_surface(context, surface);
}

static VAStatus venus_sync_buffer(VADriverContextP context,
                                  VABufferID buffer_id,
                                  uint64_t timeout_ns)
{
    (void)context;
    (void)buffer_id;
    (void)timeout_ns;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus venus_query_surface_status(VADriverContextP context,
                                           VASurfaceID surface,
                                           VASurfaceStatus *status)
{
    (void)context;
    (void)surface;
    (void)status;
    return VA_STATUS_ERROR_INVALID_SURFACE;
}

static VAStatus venus_query_surface_error(VADriverContextP context,
                                          VASurfaceID surface,
                                          VAStatus error_status,
                                          void **error_info)
{
    (void)context;
    (void)surface;
    (void)error_status;
    (void)error_info;
    return VA_STATUS_ERROR_INVALID_SURFACE;
}

static VAStatus venus_put_surface(VADriverContextP context,
                                  VASurfaceID surface, void *draw,
                                  short src_x, short src_y,
                                  unsigned short src_width,
                                  unsigned short src_height,
                                  short dst_x, short dst_y,
                                  unsigned short dst_width,
                                  unsigned short dst_height,
                                  VARectangle *cliprects,
                                  unsigned int num_cliprects,
                                  unsigned int flags)
{
    (void)context;
    (void)surface;
    (void)draw;
    (void)src_x;
    (void)src_y;
    (void)src_width;
    (void)src_height;
    (void)dst_x;
    (void)dst_y;
    (void)dst_width;
    (void)dst_height;
    (void)cliprects;
    (void)num_cliprects;
    (void)flags;
    return unsupported();
}

static VAStatus venus_query_image_formats(VADriverContextP context,
                                          VAImageFormat *formats,
                                          int *num_formats)
{
    (void)formats;

    if (!context || !num_formats)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    *num_formats = 0;
    return VA_STATUS_SUCCESS;
}

static VAStatus venus_create_image(VADriverContextP context,
                                   VAImageFormat *format,
                                   int width, int height, VAImage *image)
{
    (void)context;
    (void)format;
    (void)width;
    (void)height;
    (void)image;
    return unsupported();
}

static VAStatus venus_derive_image(VADriverContextP context,
                                   VASurfaceID surface, VAImage *image)
{
    (void)context;
    (void)surface;
    (void)image;
    return VA_STATUS_ERROR_INVALID_SURFACE;
}

static VAStatus venus_destroy_image(VADriverContextP context,
                                    VAImageID image)
{
    (void)context;
    (void)image;
    return VA_STATUS_ERROR_INVALID_IMAGE;
}

static VAStatus venus_set_image_palette(VADriverContextP context,
                                        VAImageID image,
                                        unsigned char *palette)
{
    (void)context;
    (void)image;
    (void)palette;
    return unsupported();
}

static VAStatus venus_get_image(VADriverContextP context,
                                VASurfaceID surface, int x, int y,
                                unsigned int width, unsigned int height,
                                VAImageID image)
{
    (void)context;
    (void)surface;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)image;
    return unsupported();
}

static VAStatus venus_put_image(VADriverContextP context,
                                VASurfaceID surface, VAImageID image,
                                int src_x, int src_y,
                                unsigned int src_width,
                                unsigned int src_height,
                                int dst_x, int dst_y,
                                unsigned int dst_width,
                                unsigned int dst_height)
{
    (void)context;
    (void)surface;
    (void)image;
    (void)src_x;
    (void)src_y;
    (void)src_width;
    (void)src_height;
    (void)dst_x;
    (void)dst_y;
    (void)dst_width;
    (void)dst_height;
    return unsupported();
}

static VAStatus venus_query_subpicture_formats(VADriverContextP context,
                                               VAImageFormat *formats,
                                               unsigned int *flags,
                                               unsigned int *num_formats)
{
    (void)formats;
    (void)flags;

    if (!context || !num_formats)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    *num_formats = 0;
    return VA_STATUS_SUCCESS;
}

static VAStatus venus_create_subpicture(VADriverContextP context,
                                        VAImageID image,
                                        VASubpictureID *subpicture)
{
    (void)context;
    (void)image;
    (void)subpicture;
    return unsupported();
}

static VAStatus venus_destroy_subpicture(VADriverContextP context,
                                         VASubpictureID subpicture)
{
    (void)context;
    (void)subpicture;
    return VA_STATUS_ERROR_INVALID_SUBPICTURE;
}

static VAStatus venus_set_subpicture_image(VADriverContextP context,
                                           VASubpictureID subpicture,
                                           VAImageID image)
{
    (void)context;
    (void)subpicture;
    (void)image;
    return unsupported();
}

static VAStatus venus_set_subpicture_chromakey(VADriverContextP context,
                                               VASubpictureID subpicture,
                                               unsigned int minimum,
                                               unsigned int maximum,
                                               unsigned int mask)
{
    (void)context;
    (void)subpicture;
    (void)minimum;
    (void)maximum;
    (void)mask;
    return unsupported();
}

static VAStatus venus_set_subpicture_global_alpha(VADriverContextP context,
                                                  VASubpictureID subpicture,
                                                  float alpha)
{
    (void)context;
    (void)subpicture;
    (void)alpha;
    return unsupported();
}

static VAStatus venus_associate_subpicture(VADriverContextP context,
                                           VASubpictureID subpicture,
                                           VASurfaceID *surfaces,
                                           int num_surfaces,
                                           short src_x, short src_y,
                                           unsigned short src_width,
                                           unsigned short src_height,
                                           short dst_x, short dst_y,
                                           unsigned short dst_width,
                                           unsigned short dst_height,
                                           unsigned int flags)
{
    (void)context;
    (void)subpicture;
    (void)surfaces;
    (void)num_surfaces;
    (void)src_x;
    (void)src_y;
    (void)src_width;
    (void)src_height;
    (void)dst_x;
    (void)dst_y;
    (void)dst_width;
    (void)dst_height;
    (void)flags;
    return unsupported();
}

static VAStatus venus_deassociate_subpicture(VADriverContextP context,
                                             VASubpictureID subpicture,
                                             VASurfaceID *surfaces,
                                             int num_surfaces)
{
    (void)context;
    (void)subpicture;
    (void)surfaces;
    (void)num_surfaces;
    return unsupported();
}

static VAStatus venus_query_display_attributes(VADriverContextP context,
                                               VADisplayAttribute *attributes,
                                               int *num_attributes)
{
    (void)attributes;

    if (!context || !num_attributes)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    *num_attributes = 0;
    return VA_STATUS_SUCCESS;
}

static VAStatus venus_get_display_attributes(VADriverContextP context,
                                             VADisplayAttribute *attributes,
                                             int num_attributes)
{
    (void)attributes;

    if (!context || num_attributes < 0)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    return num_attributes == 0 ? VA_STATUS_SUCCESS
                               : VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
}

static VAStatus venus_set_display_attributes(VADriverContextP context,
                                             VADisplayAttribute *attributes,
                                             int num_attributes)
{
    return venus_get_display_attributes(context, attributes, num_attributes);
}

static VAStatus venus_buffer_info(VADriverContextP context,
                                  VABufferID buffer_id,
                                  VABufferType *type,
                                  unsigned int *size,
                                  unsigned int *num_elements)
{
    (void)context;
    (void)buffer_id;
    (void)type;
    (void)size;
    (void)num_elements;
    return VA_STATUS_ERROR_INVALID_BUFFER;
}

static VAStatus venus_lock_surface(VADriverContextP context,
                                   VASurfaceID surface,
                                   unsigned int *fourcc,
                                   unsigned int *luma_stride,
                                   unsigned int *chroma_u_stride,
                                   unsigned int *chroma_v_stride,
                                   unsigned int *luma_offset,
                                   unsigned int *chroma_u_offset,
                                   unsigned int *chroma_v_offset,
                                   unsigned int *buffer_name,
                                   void **buffer)
{
    (void)context;
    (void)surface;
    (void)fourcc;
    (void)luma_stride;
    (void)chroma_u_stride;
    (void)chroma_v_stride;
    (void)luma_offset;
    (void)chroma_u_offset;
    (void)chroma_v_offset;
    (void)buffer_name;
    (void)buffer;
    return VA_STATUS_ERROR_INVALID_SURFACE;
}

static VAStatus venus_unlock_surface(VADriverContextP context,
                                     VASurfaceID surface)
{
    (void)context;
    (void)surface;
    return VA_STATUS_ERROR_INVALID_SURFACE;
}

static VAStatus venus_get_surface_attributes(VADriverContextP context,
                                             VAConfigID config_id,
                                             VASurfaceAttrib *attributes,
                                             unsigned int num_attributes)
{
    (void)context;
    (void)config_id;
    (void)attributes;
    (void)num_attributes;
    return VA_STATUS_ERROR_INVALID_CONFIG;
}

static VAStatus venus_query_surface_attributes(VADriverContextP context,
                                               VAConfigID config_id,
                                               VASurfaceAttrib *attributes,
                                               unsigned int *num_attributes)
{
    (void)context;
    (void)config_id;
    (void)attributes;
    (void)num_attributes;
    return VA_STATUS_ERROR_INVALID_CONFIG;
}

static VAStatus venus_acquire_buffer_handle(VADriverContextP context,
                                            VABufferID buffer_id,
                                            VABufferInfo *info)
{
    (void)context;
    (void)buffer_id;
    (void)info;
    return VA_STATUS_ERROR_INVALID_BUFFER;
}

static VAStatus venus_release_buffer_handle(VADriverContextP context,
                                            VABufferID buffer_id)
{
    (void)context;
    (void)buffer_id;
    return VA_STATUS_ERROR_INVALID_BUFFER;
}

static VAStatus venus_export_surface_handle(VADriverContextP context,
                                            VASurfaceID surface,
                                            uint32_t memory_type,
                                            uint32_t flags,
                                            void *descriptor)
{
    (void)context;
    (void)surface;
    (void)memory_type;
    (void)flags;
    (void)descriptor;
    return VA_STATUS_ERROR_INVALID_SURFACE;
}

void venus_vtable_init(struct VADriverVTable *vtable)
{
    vtable->vaTerminate = venus_terminate;
    vtable->vaQueryConfigProfiles = venus_query_config_profiles;
    vtable->vaQueryConfigEntrypoints = venus_query_config_entrypoints;
    vtable->vaGetConfigAttributes = venus_get_config_attributes;
    vtable->vaCreateConfig = venus_create_config;
    vtable->vaDestroyConfig = venus_destroy_config;
    vtable->vaQueryConfigAttributes = venus_query_config_attributes;
    vtable->vaCreateSurfaces = venus_create_surfaces;
    vtable->vaDestroySurfaces = venus_destroy_surfaces;
    vtable->vaCreateContext = venus_create_context;
    vtable->vaDestroyContext = venus_destroy_context;
    vtable->vaCreateBuffer = venus_create_buffer;
    vtable->vaBufferSetNumElements = venus_buffer_set_num_elements;
    vtable->vaMapBuffer = venus_map_buffer;
    vtable->vaUnmapBuffer = venus_unmap_buffer;
    vtable->vaDestroyBuffer = venus_destroy_buffer;
    vtable->vaBeginPicture = venus_begin_picture;
    vtable->vaRenderPicture = venus_render_picture;
    vtable->vaEndPicture = venus_end_picture;
    vtable->vaSyncSurface = venus_sync_surface;
    vtable->vaQuerySurfaceStatus = venus_query_surface_status;
    vtable->vaQuerySurfaceError = venus_query_surface_error;
    vtable->vaPutSurface = venus_put_surface;
    vtable->vaQueryImageFormats = venus_query_image_formats;
    vtable->vaCreateImage = venus_create_image;
    vtable->vaDeriveImage = venus_derive_image;
    vtable->vaDestroyImage = venus_destroy_image;
    vtable->vaSetImagePalette = venus_set_image_palette;
    vtable->vaGetImage = venus_get_image;
    vtable->vaPutImage = venus_put_image;
    vtable->vaQuerySubpictureFormats = venus_query_subpicture_formats;
    vtable->vaCreateSubpicture = venus_create_subpicture;
    vtable->vaDestroySubpicture = venus_destroy_subpicture;
    vtable->vaSetSubpictureImage = venus_set_subpicture_image;
    vtable->vaSetSubpictureChromakey = venus_set_subpicture_chromakey;
    vtable->vaSetSubpictureGlobalAlpha = venus_set_subpicture_global_alpha;
    vtable->vaAssociateSubpicture = venus_associate_subpicture;
    vtable->vaDeassociateSubpicture = venus_deassociate_subpicture;
    vtable->vaQueryDisplayAttributes = venus_query_display_attributes;
    vtable->vaGetDisplayAttributes = venus_get_display_attributes;
    vtable->vaSetDisplayAttributes = venus_set_display_attributes;
    vtable->vaBufferInfo = venus_buffer_info;
    vtable->vaLockSurface = venus_lock_surface;
    vtable->vaUnlockSurface = venus_unlock_surface;
    vtable->vaGetSurfaceAttributes = venus_get_surface_attributes;
    vtable->vaCreateSurfaces2 = venus_create_surfaces2;
    vtable->vaQuerySurfaceAttributes = venus_query_surface_attributes;
    vtable->vaAcquireBufferHandle = venus_acquire_buffer_handle;
    vtable->vaReleaseBufferHandle = venus_release_buffer_handle;
    vtable->vaExportSurfaceHandle = venus_export_surface_handle;
    vtable->vaSyncSurface2 = venus_sync_surface2;
    vtable->vaSyncBuffer = venus_sync_buffer;
}
