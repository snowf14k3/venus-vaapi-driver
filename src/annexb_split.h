// SPDX-License-Identifier: MIT
#ifndef VENUS_ANNEXB_SPLIT_H
#define VENUS_ANNEXB_SPLIT_H

#include <stddef.h>
#include <stdint.h>

typedef int (*venus_access_unit_callback)(const uint8_t *data, size_t size,
                                          void *opaque);

int venus_annexb_for_each_access_unit(const uint8_t *data, size_t size,
                                      venus_access_unit_callback callback,
                                      void *opaque, size_t *num_units);

#endif
