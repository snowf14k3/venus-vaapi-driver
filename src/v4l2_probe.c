// SPDX-License-Identifier: MIT
#include "v4l2_probe.h"

#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <linux/videodev2.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#ifndef VENUS_VAAPI_PROBE_LIMIT
#define VENUS_VAAPI_PROBE_LIMIT 64
#endif

static int xioctl(int fd, unsigned long request, void *argument)
{
    int result;

    do {
        result = ioctl(fd, request, argument);
    } while (result < 0 && errno == EINTR);

    return result;
}

static bool qcom_venus_device(const struct v4l2_capability *cap)
{
    return strcmp((const char *)cap->driver, "qcom-venus") == 0;
}

static bool device_role(const struct v4l2_capability *cap,
                        enum venus_role *role)
{
    const char *card = (const char *)cap->card;

    if (strstr(card, "decoder")) {
        *role = VENUS_ROLE_DECODER;
        return true;
    }

    if (strstr(card, "encoder")) {
        *role = VENUS_ROLE_ENCODER;
        return true;
    }

    return false;
}

static void enumerate_codecs(int fd, enum venus_role role,
                             struct venus_capabilities *caps)
{
    struct v4l2_fmtdesc format = {
        .type = role == VENUS_ROLE_DECODER
                    ? V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE
                    : V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
    };

    for (format.index = 0; xioctl(fd, VIDIOC_ENUM_FMT, &format) == 0;
         format.index++)
        venus_capabilities_add_fourcc(caps, role, format.pixelformat);
}

static void remember_path(struct venus_capabilities *caps,
                          enum venus_role role, const char *path)
{
    char *destination;
    size_t destination_size;

    if (role == VENUS_ROLE_DECODER) {
        destination = caps->decoder_path;
        destination_size = sizeof(caps->decoder_path);
    } else {
        destination = caps->encoder_path;
        destination_size = sizeof(caps->encoder_path);
    }

    if (destination[0] == '\0')
        snprintf(destination, destination_size, "%s", path);
}

int venus_v4l2_probe_prefix(struct venus_capabilities *caps,
                            const char *device_prefix, unsigned int limit)
{
    unsigned int index;
    bool found = false;

    if (!caps || !device_prefix || limit == 0)
        return -EINVAL;

    venus_capabilities_reset(caps);

    for (index = 0; index < limit; index++) {
        struct v4l2_capability capability = { 0 };
        enum venus_role role;
        char path[64];
        int fd;

        if (snprintf(path, sizeof(path), "%s%u", device_prefix, index) >=
            (int)sizeof(path))
            return -ENAMETOOLONG;

        fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;

        if (xioctl(fd, VIDIOC_QUERYCAP, &capability) == 0 &&
            qcom_venus_device(&capability) &&
            device_role(&capability, &role)) {
            found = true;
            remember_path(caps, role, path);
            enumerate_codecs(fd, role, caps);
        }

        close(fd);
    }

    return found ? 0 : -ENODEV;
}

int venus_v4l2_probe(struct venus_capabilities *caps)
{
    return venus_v4l2_probe_prefix(caps, "/dev/video",
                                   VENUS_VAAPI_PROBE_LIMIT);
}
