// SPDX-License-Identifier: MIT
#ifndef VENUS_V4L2_PROBE_H
#define VENUS_V4L2_PROBE_H

#include "capabilities.h"

int venus_v4l2_probe(struct venus_capabilities *caps);
int venus_v4l2_probe_prefix(struct venus_capabilities *caps,
                            const char *device_prefix, unsigned int limit);

#endif
