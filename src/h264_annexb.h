// SPDX-License-Identifier: MIT
#ifndef VENUS_H264_ANNEXB_H
#define VENUS_H264_ANNEXB_H

#include <stddef.h>
#include <stdint.h>
#include <va/va.h>

struct venus_h264_slice_batch {
    /* VA may split one access unit across several slice-data buffers. */
    const VASliceParameterBufferH264 *parameters;
    size_t num_parameters;
    const uint8_t *data;
    size_t data_size;
};

int venus_h264_build_access_unit(
    VAProfile profile, const VAPictureParameterBufferH264 *picture,
    const struct venus_h264_slice_batch *batches, size_t num_batches,
    uint8_t *output, size_t output_capacity, size_t *output_size);

#endif
