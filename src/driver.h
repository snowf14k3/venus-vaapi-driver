// SPDX-License-Identifier: MIT
#ifndef VENUS_DRIVER_H
#define VENUS_DRIVER_H

#include <va/va_backend.h>

/* Entry point exported to libva for the selected VA API version. */
VAStatus venus_driver_init(VADriverContextP context);

#endif
