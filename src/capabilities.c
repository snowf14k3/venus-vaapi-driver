// SPDX-License-Identifier: MIT
#include "capabilities.h"

#include <time.h>
#include <linux/videodev2.h>
#include <stdio.h>
#include <string.h>

struct venus_codec_map {
    uint32_t fourcc;
    enum venus_codec codec;
    const char *name;
    bool decode;
    bool encode;
};

static const struct venus_codec_map codec_map[] = {
    { V4L2_PIX_FMT_H264, VENUS_CODEC_H264, "H.264", true, true },
    { V4L2_PIX_FMT_HEVC, VENUS_CODEC_HEVC, "HEVC Main", true, true },
    { V4L2_PIX_FMT_VP8, VENUS_CODEC_VP8, "VP8", true, true },
    { V4L2_PIX_FMT_VP9, VENUS_CODEC_VP9, "VP9", true, false },
};

void venus_capabilities_reset(struct venus_capabilities *caps)
{
    if (caps)
        memset(caps, 0, sizeof(*caps));
}

bool venus_capabilities_add_fourcc(struct venus_capabilities *caps,
                                   enum venus_role role, uint32_t fourcc)
{
    size_t i;

    if (!caps)
        return false;

    for (i = 0; i < sizeof(codec_map) / sizeof(codec_map[0]); i++) {
        const struct venus_codec_map *item = &codec_map[i];

        if (item->fourcc != fourcc)
            continue;

        if (role == VENUS_ROLE_DECODER && item->decode) {
            caps->decode_codecs |= item->codec;
            return true;
        }

        if (role == VENUS_ROLE_ENCODER && item->encode) {
            caps->encode_codecs |= item->codec;
            return true;
        }

        return false;
    }

    return false;
}

bool venus_capabilities_has(const struct venus_capabilities *caps,
                            enum venus_role role, enum venus_codec codec)
{
    uint32_t codecs;

    if (!caps)
        return false;

    codecs = role == VENUS_ROLE_DECODER ? caps->decode_codecs
                                        : caps->encode_codecs;
    return (codecs & codec) != 0;
}

const char *venus_codec_name(enum venus_codec codec)
{
    size_t i;

    for (i = 0; i < sizeof(codec_map) / sizeof(codec_map[0]); i++) {
        if (codec_map[i].codec == codec)
            return codec_map[i].name;
    }

    return "unknown";
}

size_t venus_capabilities_format(const struct venus_capabilities *caps,
                                 enum venus_role role, char *buffer,
                                 size_t buffer_size)
{
    static const enum venus_codec ordered_codecs[] = {
        VENUS_CODEC_H264,
        VENUS_CODEC_HEVC,
        VENUS_CODEC_VP8,
        VENUS_CODEC_VP9,
    };
    size_t i;
    size_t used = 0;

    if (!buffer || buffer_size == 0)
        return 0;

    buffer[0] = '\0';

    for (i = 0; i < sizeof(ordered_codecs) / sizeof(ordered_codecs[0]); i++) {
        const char *name;
        int written;

        if (!venus_capabilities_has(caps, role, ordered_codecs[i]))
            continue;

        name = venus_codec_name(ordered_codecs[i]);
        written = snprintf(buffer + used, buffer_size - used, "%s%s",
                           used ? "," : "", name);
        if (written < 0)
            break;
        if ((size_t)written >= buffer_size - used) {
            used = buffer_size - 1;
            break;
        }
        used += (size_t)written;
    }

    if (used == 0) {
        int written = snprintf(buffer, buffer_size, "none");

        if (written > 0)
            used = (size_t)written < buffer_size ? (size_t)written
                                                 : buffer_size - 1;
    }

    return used;
}
