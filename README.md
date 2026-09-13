# venus-vaapi-driver

A userspace VA-API backend for the Qualcomm Venus stateful V4L2 M2M codec,
initially targeting Xiaomi Redmi K20 Pro / Mi 9T Pro (Raphael, SM8150).

## Current status

**Bootstrap and capability-probe stage. No VA decode or encode profile is
advertised yet.**

The repository currently provides:

- `venus_drv_video.so` with a libva 1.20-compatible driver entry point;
- a complete mandatory libva vtable with explicit unsupported results;
- direct qcom-venus discovery through `VIDIOC_QUERYCAP` and
  `VIDIOC_ENUM_FMT`;
- a validated codec allowlist independent from generic kernel format tables;
- `venus-vaapi-info` for inspecting the live V4L2 devices;
- host tests for the allowlist, no-device behavior, driver initialization and
  exported ABI symbol.

Debian 13 ships libva 2.22. A driver built against libva 1.20 remains loadable
because libva searches compatible lower minor-version init symbols.

## Intended capability order

| Codec | Decode | Encode |
| --- | --- | --- |
| H.264 Baseline/Main/High | first milestone | after decode |
| HEVC Main 8-bit | planned | planned |
| VP8 | planned | planned |
| VP9 Profile 0 | planned | not exposed |

The driver will not advertise a profile before its full submission path is
implemented and validated on hardware.

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
