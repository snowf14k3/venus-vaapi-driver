// SPDX-License-Identifier: MIT
#ifndef VENUS_HEVC_ANNEXB_H
#define VENUS_HEVC_ANNEXB_H

#include <stddef.h>
#include <stdint.h>
#include <va/va.h>
#include <va/va_dec_hevc.h>

#ifdef __cplusplus
extern "C" {
#endif

struct venus_hevc_state;

struct venus_hevc_slice {
    const VASliceParameterBufferHEVC *parameters;
    const uint8_t *data;
    size_t size;
};

/* Build one Annex-B access unit. Commit next_state only after V4L2 accepts it. */
int venus_hevc_build_access_unit(
    VAProfile profile,
    const VAPictureParameterBufferHEVC *picture,
    const VAIQMatrixBufferHEVC *iq_matrix,
    const struct venus_hevc_slice *slices, size_t num_slices,
    const struct venus_hevc_state *previous_state,
    struct venus_hevc_state **next_state,
    uint8_t **access_unit, size_t *access_unit_size);

void venus_hevc_state_free(struct venus_hevc_state *state);

#ifdef __cplusplus
}
#endif

#endif
