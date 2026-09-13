// SPDX-License-Identifier: MIT
#include "driver.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    struct VADriverContext context;
    struct VADriverVTable vtable;
    VAProfile profiles[1];
    int num_profiles = -1;

    memset(&context, 0, sizeof(context));
    memset(&vtable, 0, sizeof(vtable));
    context.vtable = &vtable;

    assert(setenv("VENUS_VAAPI_ALLOW_NO_DEVICE", "1", 1) == 0);
    assert(venus_driver_init(&context) == VA_STATUS_SUCCESS);
    assert(context.pDriverData != NULL);
    assert(context.max_profiles == 3);
    assert(context.max_entrypoints == 1);
    assert(context.max_attributes == 4);
    assert(context.str_vendor != NULL);

    assert(vtable.vaTerminate != NULL);
    assert(vtable.vaQueryConfigProfiles != NULL);
    assert(vtable.vaQueryConfigEntrypoints != NULL);
    assert(vtable.vaQueryConfigAttributes != NULL);
    assert(vtable.vaCreateConfig != NULL);
    assert(vtable.vaDestroyConfig != NULL);
    assert(vtable.vaGetConfigAttributes != NULL);
    assert(vtable.vaCreateSurfaces != NULL);
    assert(vtable.vaDestroySurfaces != NULL);
    assert(vtable.vaCreateContext != NULL);
    assert(vtable.vaDestroyContext != NULL);
    assert(vtable.vaCreateBuffer != NULL);
    assert(vtable.vaBufferSetNumElements != NULL);
    assert(vtable.vaMapBuffer != NULL);
    assert(vtable.vaUnmapBuffer != NULL);
    assert(vtable.vaDestroyBuffer != NULL);
    assert(vtable.vaBeginPicture != NULL);
    assert(vtable.vaRenderPicture != NULL);
    assert(vtable.vaEndPicture != NULL);
    assert(vtable.vaSyncSurface != NULL);
    assert(vtable.vaQuerySurfaceStatus != NULL);
    assert(vtable.vaQueryImageFormats != NULL);
    assert(vtable.vaCreateImage != NULL);
    assert(vtable.vaDeriveImage != NULL);
    assert(vtable.vaDestroyImage != NULL);
    assert(vtable.vaSetImagePalette != NULL);
    assert(vtable.vaGetImage != NULL);
    assert(vtable.vaPutImage != NULL);
    assert(vtable.vaQuerySubpictureFormats != NULL);
    assert(vtable.vaCreateSubpicture != NULL);
    assert(vtable.vaDestroySubpicture != NULL);
    assert(vtable.vaSetSubpictureImage != NULL);
    assert(vtable.vaSetSubpictureChromakey != NULL);
    assert(vtable.vaSetSubpictureGlobalAlpha != NULL);
    assert(vtable.vaAssociateSubpicture != NULL);
    assert(vtable.vaDeassociateSubpicture != NULL);
    assert(vtable.vaQueryDisplayAttributes != NULL);
    assert(vtable.vaGetDisplayAttributes != NULL);
    assert(vtable.vaSetDisplayAttributes != NULL);

    assert(vtable.vaQueryConfigProfiles(
               &context, profiles, &num_profiles) == VA_STATUS_SUCCESS);
    assert(num_profiles == 0);
    assert(vtable.vaCreateConfig(
               &context, VAProfileH264High, VAEntrypointVLD,
               NULL, 0, NULL) == VA_STATUS_ERROR_INVALID_PARAMETER);

    assert(vtable.vaTerminate(&context) == VA_STATUS_SUCCESS);
    assert(context.pDriverData == NULL);
    return 0;
}
