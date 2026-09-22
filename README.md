# venus-vaapi-driver

A userspace VA-API backend for the Qualcomm Venus stateful V4L2 M2M codec,
initially targeting Xiaomi Redmi K20 Pro / Mi 9T Pro (Raphael, SM8150).

## Current status

**H.264 High VLD and EncSlice are validated for progressive 8-bit video
through Raphael's native 1080x2340 and 2340x1080 orientations. Other codecs
remain disabled.**

The repository currently provides:

- `venus_drv_video.so` with a libva 1.20-compatible driver entry point;
- a complete mandatory libva vtable with explicit unsupported results;
- direct qcom-venus discovery through `VIDIOC_QUERYCAP` and
  `VIDIOC_ENUM_FMT`;
- a validated codec allowlist independent from generic kernel format tables;
- `venus-vaapi-info` for inspecting the live V4L2 devices;
- a bounded H.264 Annex-B assembler that reconstructs conservative SPS/PPS
  NAL units and validates every VA slice range before copying it;
- an isolated V4L2 stateful decoder session and `venus-v4l2-decode` tool for
  validating queue order, MMAP buffers, source-change events and drain;
- experimental H.264 Baseline/Main/High VLD config, context, buffer, surface,
  sync and NV12 image-download paths backed by that same session;
- a device-validated H.264 stateful encoder session for NV12 input,
  V4L2 controls, encoded CAPTURE packets and drain;
- H.264 Baseline/Main/High EncSlice config, context, parameter,
  coded-buffer, sync and CPU-backed NV12 upload paths using that session;
- CQP compatibility, packed headers and linear NV12 DRM PRIME export
  backed by the system DMA-BUF heap.

Debian 13 ships libva 2.22. A driver built against libva 1.20 remains loadable
because libva searches compatible lower minor-version init symbols.

## Intended capability order

| Codec | Decode | Encode |
| --- | --- | --- |
| H.264 Baseline/Main/High | Baseline and High validated; Main pending | High validated through 1080x2340 |
| HEVC Main 8-bit | planned | planned |
| VP8 | planned | planned |
| VP9 Profile 0 | planned | not exposed |

Only codec profiles with implemented VA buffer submission paths are exposed.

## Build

Debian 13 dependencies:

```bash
sudo apt install build-essential meson ninja-build pkg-config libva-dev libdrm-dev libudev-dev vainfo
```

Configure and build:

```bash
meson setup build
meson compile -C build
```

Probe the live Venus nodes and load the driver directly from the build
directory:

```bash
./build/venus-vaapi-info
LIBVA_DRIVERS_PATH="$PWD/build" LIBVA_DRIVER_NAME=venus \
vainfo --display drm --device /dev/dri/renderD128
```

Install system-wide:

```bash
./scripts/install.sh
```

See [usage](docs/usage.md) for FFmpeg encode/decode commands and runtime
environment setup.

## Design

Venus exposes a stateful V4L2 decoder and encoder. The VA backend translates
libva object lifetimes and picture submissions to the two V4L2 M2M queues. It
does not access HFI directly and does not copy the Android OMX implementation
into userspace.

See [docs/architecture.md](docs/architecture.md) for the interface boundary,
capability policy and implementation stages.

## Completed first milestone

The H.264 VLD milestone completed:

1. load the backend through libva;
2. create CPU-visible NV12 surfaces;
3. assemble a valid stateful H.264 access unit from VA buffers;
4. decode 30 frames through qcom-venus;
5. download all frames and compare them with software decode;
6. close and reopen the session without leaking buffers or wedging firmware.

## References

- [libva backend API](https://github.com/intel/libva/blob/master/va/va_backend.h)
- [V4L2 stateful decoder interface](https://docs.kernel.org/userspace-api/media/v4l/dev-decoder.html)
- [V4L2 stateful encoder interface](https://docs.kernel.org/userspace-api/media/v4l/dev-encoder.html)
- [DRM PRIME VA surfaces](https://github.com/intel/libva/blob/master/va/va_drmcommon.h)

## License

MIT
