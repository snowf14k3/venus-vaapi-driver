# venus-vaapi-driver

VA-API hardware video driver for the Qualcomm Venus codec on the Xiaomi Redmi
K20 Pro / Mi 9T Pro (Raphael, SM8150).

## Support

| Codec | Decode | Encode |
| --- | --- | --- |
| H.264 8-bit | Baseline and High tested; Main exposed | High tested; Baseline and Main exposed |
| HEVC Main 8-bit | Not available | I/P frames |

The driver uses NV12 surfaces and supports DMA-BUF export. HEVC Main10 and B
frames are not supported. H.264 and HEVC encoding were tested on Raphael,
including a 600-frame 1080p60 HEVC stream. FFmpeg may align a 1080-high HEVC
context to 1088 coded lines; clients should present the visible height.

## Install

On Debian 13:

```bash
sudo apt install build-essential meson ninja-build pkg-config libva-dev libdrm-dev libudev-dev vainfo
./scripts/install.sh
LIBVA_DRIVER_NAME=venus vainfo --display drm --device /dev/dri/renderD128
```

See [usage](docs/usage.md) for FFmpeg commands and [architecture](docs/architecture.md)
for implementation details. Licensed under MIT.
