# Using the Venus VA-API driver

## Install

From the repository checkout, run the complete validation and installation:

```bash
./scripts/validate-and-install.sh
```

It builds the driver, runs host tests, runs the complete hardware matrix and
only installs `venus_drv_video.so` after every check passes. It also writes
`/etc/environment.d/90-venus-vaapi.conf` and
`/etc/profile.d/venus-vaapi.sh` so new login sessions select this driver.

To install without rerunning the hardware matrix, use `./scripts/install.sh`.

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

## Validation

Run the complete H.264 matrix before system installation:

```bash
sudo ./tests/run-vaapi-h264-matrix.sh
```

It tests 640x480, 720p, 1080p and a 300-frame 720p session. Every encoded
stream is software-decoded and VAAPI-decoded; the decoded frame hashes,
frame counts and visible dimensions must match.

## Current limits

- progressive H.264 8-bit encoding and decoding;
- CBR encoding with no B frames;
- CPU-backed NV12 VA surfaces;
- maximum advertised coded size 1920x1088;
- DRM PRIME export and zero-copy GPU interop are not implemented;
- concurrent sessions and long-running service workloads still require
  application-specific testing.
