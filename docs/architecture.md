# Architecture

## Boundary

This project is a userspace libva driver. It does not speak Qualcomm HFI
directly and does not duplicate the kernel Venus driver.

```text
VA-API client
    |
    v
venus_drv_video.so
    |
    +-- VA object lifetime and synchronization
    +-- codec parameter and bytestream translation
    +-- surface and DMA-BUF management
    |
    v
V4L2 stateful M2M API
    |
    v
qcom-venus kernel driver and firmware
```

## Capability policy

The V4L2 probe intersects the formats exposed by qcom-venus with a project
allowlist. The allowlist contains only formats already validated on Xiaomi
Raphael:

| Codec | Decode candidate | Encode candidate |
| --- | --- | --- |
| H.264 | yes | yes |
| HEVC Main 8-bit | yes | yes |
| VP8 | yes | yes |
| VP9 Profile 0 | yes | no |

A candidate is not a VA-API capability. The driver reports no VA profiles
until the complete VA submission path for that profile has tests and device
evidence.

MPEG-2, MPEG-4, H.263, VC-1, Xvid, HEVC Main10 and VP9 encode are outside the
initial allowlist.

## Decode model

Venus implements the V4L2 stateful decoder interface. It consumes complete
coded-stream chunks on OUTPUT and returns decoded frames on CAPTURE. VA-API
VLD clients submit picture parameters, slice parameters and slice data. Each
codec adapter must therefore collect all buffers between `vaBeginPicture`
and `vaEndPicture`, construct the stream syntax required by the stateful
decoder, and associate the dequeued CAPTURE buffer with the requested VA
surface.

Dynamic resolution changes must follow the V4L2 source-change sequence. A
CAPTURE buffer cannot be requeued while a client owns the corresponding VA
surface.

## Encode model

The encoder queues raw VA surfaces on V4L2 OUTPUT and returns complete coded
chunks through V4L2 CAPTURE. VA sequence, picture, slice and miscellaneous
parameters are translated to V4L2 controls before streaming.

## Surface plan

1. CPU-visible linear NV12 surfaces for the first correctness milestone.
2. DMA-BUF allocation and V4L2 import.
3. DRM PRIME export through `vaExportSurfaceHandle`.
4. Explicit synchronization and direct display integration.

The first milestone intentionally prioritizes byte-correct H.264 decode over
zero-copy presentation.
