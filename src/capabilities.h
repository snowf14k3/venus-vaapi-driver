// SPDX-License-Identifier: MIT
#ifndef VENUS_CAPABILITIES_H
#define VENUS_CAPABILITIES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * This is the driver's small capability model.  It records formats reported
 * by the qcom-venus V4L2 nodes after applying the userspace allowlist; it is
 * deliberately not a promise that every VA-API profile is implemented.
 */
enum venus_codec {
    VENUS_CODEC_H264 = 1u << 0,
    VENUS_CODEC_HEVC = 1u << 1,
    VENUS_CODEC_VP8 = 1u << 2,
    VENUS_CODEC_VP9 = 1u << 3,
};

enum venus_role {
    VENUS_ROLE_DECODER,
    VENUS_ROLE_ENCODER,
};

struct venus_capabilities {
    uint32_t decode_codecs;
    uint32_t encode_codecs;
    char decoder_path[64];
    char encoder_path[64];
};

void venus_capabilities_reset(struct venus_capabilities *caps);
bool venus_capabilities_add_fourcc(struct venus_capabilities *caps,
                                   enum venus_role role, uint32_t fourcc);
bool venus_capabilities_has(const struct venus_capabilities *caps,
                            enum venus_role role, enum venus_codec codec);
const char *venus_codec_name(enum venus_codec codec);
size_t venus_capabilities_format(const struct venus_capabilities *caps,
                                 enum venus_role role, char *buffer,
                                 size_t buffer_size);

#endif
