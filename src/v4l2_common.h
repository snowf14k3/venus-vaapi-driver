// SPDX-License-Identifier: MIT
#ifndef VENUS_V4L2_COMMON_H
#define VENUS_V4L2_COMMON_H

struct venus_v4l2_error {
    /* Keep the last ioctl name with errno so VA failures remain actionable. */
    int code;
    char operation[64];
};

#endif
