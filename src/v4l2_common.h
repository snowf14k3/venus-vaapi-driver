// SPDX-License-Identifier: MIT
#ifndef VENUS_V4L2_COMMON_H
#define VENUS_V4L2_COMMON_H

struct venus_v4l2_error {
    int code;
    char operation[64];
};

#endif
