// SPDX-License-Identifier: MIT
#ifndef VENUS_BACKEND_H
#define VENUS_BACKEND_H

#include "capabilities.h"

#include <va/va_backend.h>

/* Allocate the per-display state shared by all VA entry points. */
VAStatus venus_backend_create(const struct venus_capabilities *capabilities,
                              void **backend);
/* Install the implemented operations over the default VA stub vtable. */
void venus_backend_fill_vtable(struct VADriverVTable *vtable);

#endif
