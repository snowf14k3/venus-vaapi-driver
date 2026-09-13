// SPDX-License-Identifier: MIT
#include "v4l2_probe.h"

#include <stdio.h>

int main(void)
{
    struct venus_capabilities caps;
    char decode[128];
    char encode[128];
    int result;

    result = venus_v4l2_probe(&caps);
    if (result < 0) {
        fprintf(stderr, "No qcom-venus V4L2 M2M device found\n");
        return 1;
    }

    venus_capabilities_format(&caps, VENUS_ROLE_DECODER, decode,
                              sizeof(decode));
    venus_capabilities_format(&caps, VENUS_ROLE_ENCODER, encode,
                              sizeof(encode));

    printf("decoder=%s\n", caps.decoder_path[0] ? caps.decoder_path : "none");
    printf("decoder_codecs=%s\n", decode);
    printf("encoder=%s\n", caps.encoder_path[0] ? caps.encoder_path : "none");
    printf("encoder_codecs=%s\n", encode);
    if (venus_capabilities_has(
            &caps, VENUS_ROLE_DECODER, VENUS_CODEC_H264))
        printf("vaapi_profiles=H.264 Baseline/Main/High VLD (experimental)\n");
    else
        printf("vaapi_profiles=none\n");

    return 0;
}
