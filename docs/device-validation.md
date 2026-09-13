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

## H.264 stateful V4L2 encode

Validated on the same device before enabling the VA-API encode entrypoint.

Test command:

```bash
sudo ./tests/run-v4l2-h264-encode.sh
```

Result:

- 30 progressive 640x480 NV12 frames were submitted to qcom-venus;
- the encoder used 4 OUTPUT buffers and 16 CAPTURE buffers;
- the encoded Annex-B stream was 89,056 bytes;
- software decoding recovered 30 frames and 13,824,000 NV12 bytes;
- EOS drain completed successfully.

## H.264 VA-API EncSlice

Validated on the same device with the `h264_vaapi` FFmpeg encoder.

Test command:

```bash
sudo ./tests/run-vaapi-h264-encode.sh
```

Result:

- `vainfo` reported H.264 Constrained Baseline, Main and High EncSlice;
- FFmpeg uploaded and encoded 30 progressive 640x480 NV12 frames;
- the encoded Annex-B stream was 96,976 bytes;
- software decoding recovered 30 frames and 13,824,000 NV12 bytes;
- the independent V4L2 frame tags increased monotonically from 1 through 30;
- the coded-buffer FIFO drained to zero without timeout or a Venus session
  error.

This validates CPU-backed NV12 upload, CBR parameter translation,
`VAEncCodedBufferType`, `vaSyncBuffer` and FFmpeg's coded-buffer reuse.
Higher resolutions, concurrent sessions and long-running encoding remain
separate milestones.
