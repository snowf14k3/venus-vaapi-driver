// SPDX-License-Identifier: MIT
#include "venus/capabilities.h"
#include "v4l2_probe.h"

#include <assert.h>
#include <errno.h>
#include <time.h>
#include <linux/videodev2.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    struct venus_capabilities caps;
    char output[128];

    venus_capabilities_reset(&caps);
    assert(caps.decode_codecs == 0);
    assert(caps.encode_codecs == 0);

    assert(venus_capabilities_add_fourcc(
        &caps, VENUS_ROLE_DECODER, V4L2_PIX_FMT_H264));
    assert(venus_capabilities_add_fourcc(
        &caps, VENUS_ROLE_DECODER, V4L2_PIX_FMT_HEVC));
    assert(venus_capabilities_add_fourcc(
        &caps, VENUS_ROLE_DECODER, V4L2_PIX_FMT_VP8));
    assert(venus_capabilities_add_fourcc(
        &caps, VENUS_ROLE_DECODER, V4L2_PIX_FMT_VP9));

    assert(venus_capabilities_add_fourcc(
        &caps, VENUS_ROLE_ENCODER, V4L2_PIX_FMT_H264));
    assert(venus_capabilities_add_fourcc(
        &caps, VENUS_ROLE_ENCODER, V4L2_PIX_FMT_HEVC));
    assert(venus_capabilities_add_fourcc(
        &caps, VENUS_ROLE_ENCODER, V4L2_PIX_FMT_VP8));

    assert(!venus_capabilities_add_fourcc(
        &caps, VENUS_ROLE_ENCODER, V4L2_PIX_FMT_VP9));
    assert(!venus_capabilities_add_fourcc(
        &caps, VENUS_ROLE_DECODER, V4L2_PIX_FMT_MPEG2));
    assert(!venus_capabilities_add_fourcc(
        &caps, VENUS_ROLE_DECODER, V4L2_PIX_FMT_MPEG4));
    assert(!venus_capabilities_add_fourcc(
        &caps, VENUS_ROLE_ENCODER, V4L2_PIX_FMT_H263));

    assert(venus_capabilities_has(
        &caps, VENUS_ROLE_DECODER, VENUS_CODEC_VP9));
    assert(!venus_capabilities_has(
        &caps, VENUS_ROLE_ENCODER, VENUS_CODEC_VP9));

    venus_capabilities_format(
        &caps, VENUS_ROLE_DECODER, output, sizeof(output));
    assert(strcmp(output, "H.264,HEVC Main,VP8,VP9") == 0);

    venus_capabilities_format(
        &caps, VENUS_ROLE_ENCODER, output, sizeof(output));
    assert(strcmp(output, "H.264,HEVC Main,VP8") == 0);

    assert(venus_v4l2_probe_prefix(
               &caps, "/path/that/does/not/exist/video", 2) == -ENODEV);
    assert(caps.decode_codecs == 0);
    assert(caps.encode_codecs == 0);

    puts("PASS: validated codec allowlist and empty-device probe");
    return 0;
}
