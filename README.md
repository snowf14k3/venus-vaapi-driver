# venus-vaapi-driver

VA-API hardware video driver for the Qualcomm Venus codec on the Xiaomi Redmi
K20 Pro / Mi 9T Pro (Raphael, SM8150).

## Support

| Codec | Decode | Encode |
| --- | --- | --- |
| H.264 8-bit | Baseline, Main, High | Baseline, Main, High |
| HEVC Main 8-bit | Experimental (opt-in) | Available |
| HEVC Main10 | Experimental (opt-in) | Available |
| VP8 | Available | Available |
| VP9 Profile 0 | Available | Not supported |

Raphael hardware tests decoded 45 consecutive HEVC Main and Main10 frames
pixel-identically to software. Main10 uses P010 surfaces. To expose HEVC
hardware decoding for one process, set
`VENUS_VAAPI_EXPERIMENTAL_HEVC_DECODE=1`. On the tested Raphael 7.1 kernel,
after an HEVC hardware decode session, Venus encoding can fail until reboot,
so HEVC decoding stays disabled by default. Encoding supports I/P frames;
B-frame encoding and VP9 encoding are not implemented. The driver supports
DMA-BUF export. FFmpeg may align a 1080-high HEVC encoding context to 1088
coded lines; clients should present the visible height.

## Install

On Debian 13:

```bash
sudo apt install build-essential meson ninja-build pkg-config libva-dev libdrm-dev vainfo
./scripts/install.sh
LIBVA_DRIVER_NAME=venus vainfo --display drm --device /dev/dri/renderD128
```

Licensed under MIT.
