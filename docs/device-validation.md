# Device validation

## H.264 VLD

Validated on Xiaomi Redmi K20 Pro / Mi 9T Pro (Raphael, SM8150) with the
qcom-venus stateful decoder.

Test command:

```bash
sudo ./tests/run-vaapi-h264.sh
```

Result:

- libva loaded `venus_drv_video.so`;
- `vainfo` reported H.264 Constrained Baseline, Main and High VLD;
- FFmpeg decoded 30 progressive 640x480 frames through VA-API;
- software output: 13,824,000 bytes;
- VA-API output: 13,824,000 bytes;
- both NV12 outputs had SHA-256
  `5a5aa547019fe1c8e33bf682a619c24562efc370ceee31b3d93be2fa22c70cb8`;
- no stale or invalid VA buffer destruction remained after the ownership fix.

This validates CPU-backed NV12 download. DMA-BUF export, interlaced H.264,
custom scaling matrices, picture-order-count type 1, higher resolutions,
seeking, concurrency and long-running playback remain separate milestones.
