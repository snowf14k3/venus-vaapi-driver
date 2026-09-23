# Using the Venus VA-API driver

## Install

From the repository checkout, build and install the driver:

```bash
./scripts/install.sh
```

Select the driver immediately in the current shell:

```bash
export LIBVA_DRIVER_NAME=venus
```

Verify the installed driver:

```bash
vainfo --display drm --device /dev/dri/renderD128
```

## H.264 encoding with FFmpeg

Encode a normal input file while copying its audio:

```bash
LIBVA_DRIVER_NAME=venus ffmpeg -y \
  -init_hw_device vaapi=venus:/dev/dri/renderD128 \
  -filter_hw_device venus \
  -i input.mp4 \
  -map 0:v:0 -map '0:a?' \
  -vf 'format=nv12,hwupload' \
  -c:v h264_vaapi -profile:v high \
  -rc_mode CBR -b:v 4M -maxrate 4M -bufsize 8M \
  -g 60 -bf 0 \
  -c:a copy output.mp4
```

For 1080p, `-b:v 8M -maxrate 8M -bufsize 16M` is a reasonable starting
point. Adjust bitrate and GOP length for the workload.

## HEVC Main encoding with FFmpeg

The encoder accepts progressive 8-bit 4:2:0 input and I/P frames:

```bash
LIBVA_DRIVER_NAME=venus ffmpeg -y \
  -init_hw_device vaapi=venus:/dev/dri/renderD128 \
  -filter_hw_device venus \
  -i input.mp4 \
  -map 0:v:0 -map '0:a?' \
  -vf 'format=nv12,hwupload' \
  -c:v hevc_vaapi -profile:v main \
  -rc_mode VBR -b:v 8M -g 60 -bf 0 \
  -c:a copy output.mp4
```

HEVC Main10 and B frames are not exposed by this driver.

## H.264 decoding with FFmpeg

Decode through VA-API and download NV12 frames to the CPU:

```bash
LIBVA_DRIVER_NAME=venus ffmpeg \
  -hwaccel vaapi \
  -hwaccel_device /dev/dri/renderD128 \
  -hwaccel_output_format vaapi \
  -i input.mp4 \
  -vf 'hwdownload,format=nv12' \
  -f null -
```

Applications launched from a desktop session must inherit
`LIBVA_DRIVER_NAME=venus`. Set it in that application's service or launcher
environment when required.

## Current limits

- progressive H.264 8-bit encoding and decoding, plus HEVC Main 8-bit
  encoding;
- no B frames; VA CBR and CQP requests keep frame rate control enabled and
  use the SM8150 VBR firmware route because its CBR route severely
  undershoots the requested bitrate. CQP also applies the requested initial
  QP, a narrow QP range and a pixel-rate-scaled compatibility bitrate.
  `VENUS_VAAPI_CQP_BITRATE` can override that bitrate from 32000 through
  160000000 bits per second;
- VA clients normally supply the coded and visible dimensions.
  `VENUS_VAAPI_NATIVE_MODE` remains an explicit override for clients which
  cannot report cropping correctly;
- H.264 encode level selection uses the IRIS1 firmware-auto contract from
  Raphael kernel patch 0029;
- HEVC VBR/CBR constrains firmware QP to within two steps of the VA picture
  QP; this prevents the progressive quality collapse observed with an
  unrestricted QP range on SM8150;
- VBR/CBR encoding chooses a quality QP from the client's bitrate,
  resolution and frame rate, while retaining a better client-requested QP.
  The firmware can adjust within two QP steps. `VENUS_VAAPI_QUALITY_QP`
  remains an optional manual override; lower values improve detail but can
  exceed the requested bitrate;
- per-frame packed raw-header requests enable the Venus hardware
  access-unit delimiter and preserve H.264 access-unit boundaries;
- the first raw frame's separate SPS/PPS and IDR CAPTURE packets are joined
  by their V4L2 timestamp before the VA coded buffer becomes ready;
- the first raw frame is queued before OUTPUT and CAPTURE STREAMON, matching
  FFmpeg and GStreamer's stateful V4L2 encoder lifecycle;
- CPU-backed NV12 surfaces and linear DMA-BUF export for GPU clients;
- maximum advertised width and height of 4096, subject to the SM8150
  H.264 limit of 36,864 macroblocks per frame;
- exported Vulkan surfaces are copied into the V4L2 MMAP queue; direct
  DMA-BUF submission into Venus is not implemented;
- concurrent sessions and long-running service workloads still require
  application-specific testing.
