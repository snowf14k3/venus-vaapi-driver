# venus-vaapi-driver

A userspace VA-API backend for the Qualcomm Venus stateful V4L2 M2M codec,
initially targeting Xiaomi Redmi K20 Pro / Mi 9T Pro (Raphael, SM8150).

## Current status

**Experimental H.264 VLD integration. Encoding and the other decode profiles
remain disabled.**

The repository currently provides:

- `venus_drv_video.so` with a libva 1.20-compatible driver entry point;
- a complete mandatory libva vtable with explicit unsupported results;
- direct qcom-venus discovery through `VIDIOC_QUERYCAP` and
  `VIDIOC_ENUM_FMT`;
- a validated codec allowlist independent from generic kernel format tables;
- `venus-vaapi-info` for inspecting the live V4L2 devices;
- host tests for the allowlist, no-device behavior, driver initialization and
  exported ABI symbol;
- a bounded H.264 Annex-B assembler that reconstructs conservative SPS/PPS
  NAL units and validates every VA slice range before copying it;
- an isolated V4L2 stateful decoder session and `venus-v4l2-decode` tool for
  validating queue order, MMAP buffers, source-change events and drain;
- experimental H.264 Baseline/Main/High VLD config, context, buffer, surface,
  sync and NV12 image-download paths backed by that same session.

Debian 13 ships libva 2.22. A driver built against libva 1.20 remains loadable
because libva searches compatible lower minor-version init symbols.

## Intended capability order

| Codec | Decode | Encode |
| --- | --- | --- |
| H.264 Baseline/Main/High | first milestone | after decode |
| HEVC Main 8-bit | planned | planned |
| VP8 | planned | planned |
| VP9 Profile 0 | planned | not exposed |

The `h264-vld` branch advertises H.264 for device validation. It will not be
merged into `main` until the full VA submission path passes byte-exact hardware
tests. Other profiles remain hidden.

## Build

Debian 13 dependencies:

```bash
sudo apt install build-essential meson ninja-build pkg-config libva-dev libdrm-dev libudev-dev vainfo
```

Configure, build and test:

```bash
meson setup build
meson compile -C build
meson test -C build --print-errorlogs
```

Probe the live Venus nodes:

```bash
./build/venus-vaapi-info
```

On a Raphael test device, validate the internal V4L2 session independently
from VA-API:

```bash
sudo ./tests/run-v4l2-h264.sh
```

The script generates a progressive 640x480 H.264 stream with access-unit
delimiters, decodes 30 frames through `venus-v4l2-decode`, compares the raw
NV12 output with software decoding, and saves the complete userspace and
kernel evidence under `/var/tmp`.

Validate the experimental VA-API path:

```bash
sudo ./tests/run-vaapi-h264.sh
```

This runs `vainfo`, decodes the same 30-frame stream through FFmpeg VA-API and
`hwdownload`, compares every NV12 byte, and enables userspace backend tracing
for the test only.

Test driver loading after the first codec profile is implemented:

```bash
LIBVA_DRIVERS_PATH="$PWD/build" LIBVA_DRIVER_NAME=venus \
vainfo --display drm --device /dev/dri/renderD128
```

Install:

```bash
sudo meson install -C build
```

The default destination is `${libdir}/dri/venus_drv_video.so`.

## Design

Venus exposes a stateful V4L2 decoder and encoder. The VA backend translates
libva object lifetimes and picture submissions to the two V4L2 M2M queues. It
does not access HFI directly and does not copy the Android OMX implementation
into userspace.

See [docs/architecture.md](docs/architecture.md) for the interface boundary,
capability policy and implementation stages.

## Initial validation target

The first functional milestone is H.264 VLD:

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
