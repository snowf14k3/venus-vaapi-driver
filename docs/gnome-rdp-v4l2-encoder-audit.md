# GNOME RDP and Venus V4L2 encoder contract audit

## Scope

This audit covers the H.264 encode path used by GNOME Remote Desktop 48.1 on
Raphael/SM8150. The target session is 2340x1080 at 60 frames per second. The
VA client requests H.264 High, CQP, packed sequence/picture/slice/raw headers,
and macroblock-aligned 2352x1088 VA surfaces.

The comparison fixes the following source baselines:

- GNOME Remote Desktop 48.1
  [`grd-encode-session-vaapi.c`](https://gitlab.gnome.org/GNOME/gnome-remote-desktop/-/blob/48.1/src/grd-encode-session-vaapi.c)
  and
  [`grd-hwaccel-vaapi.c`](https://gitlab.gnome.org/GNOME/gnome-remote-desktop/-/blob/48.1/src/grd-hwaccel-vaapi.c).
- FFmpeg release/7.1
  [`v4l2_m2m_enc.c`](https://github.com/FFmpeg/FFmpeg/blob/release/7.1/libavcodec/v4l2_m2m_enc.c),
  [`v4l2_m2m.c`](https://github.com/FFmpeg/FFmpeg/blob/release/7.1/libavcodec/v4l2_m2m.c),
  [`v4l2_context.c`](https://github.com/FFmpeg/FFmpeg/blob/release/7.1/libavcodec/v4l2_context.c),
  and
  [`v4l2_buffers.c`](https://github.com/FFmpeg/FFmpeg/blob/release/7.1/libavcodec/v4l2_buffers.c).
- GStreamer 1.26
  [`gstv4l2bufferpool.c`](https://github.com/GStreamer/gstreamer/blob/1.26/subprojects/gst-plugins-good/sys/v4l2/gstv4l2bufferpool.c),
  [`gstv4l2videoenc.c`](https://github.com/GStreamer/gstreamer/blob/1.26/subprojects/gst-plugins-good/sys/v4l2/gstv4l2videoenc.c),
  and
  [`gstv4l2object.c`](https://github.com/GStreamer/gstreamer/blob/1.26/subprojects/gst-plugins-good/sys/v4l2/gstv4l2object.c).
- Raphael kernel patches 0010, 0027, 0028, 0029, 0032, 0041, and 0044
  in `raphael-kernel-venus` commit `20fcbc8`.
- Xiaomi Android Q downstream commit
  `192eca8550f95c2eec58a474793d1d93fc1b3b67`.

## Device evidence

The 0.5.1 driver reaches all VA capability and allocation gates:

- VA-API 1.22 initializes the Venus backend.
- H.264 High EncSlice exposes CQP and all packed-header flags required by
  GNOME.
- Eight linear DMA-BUF NV12 surfaces are allocated and exported.
- The 2352x1088 macroblock surface is mapped to the 2340x1080 Raphael panel
  extent.
- The requested frame rate remains 60 fps.
- The first raw OUTPUT buffer is accepted after moving QBUF before STREAMON.
- The session then fails synchronously at `VIDIOC_STREAMON(CAPTURE)` with
  `EINVAL`.

This failure position is the Venus `venc_start_streaming()` transaction. It
is not a libva capability failure, a DMA-BUF export failure, an OUTPUT QBUF
shape error, an EOS/drain failure, or a later encoded-packet routing failure.

## Full contract comparison

| Contract | FFmpeg 7.1 | GStreamer 1.26 | Backend before 0.6.0 | 0.6.0 decision |
| --- | --- | --- | --- | --- |
| Raw/coded S_FMT | OUTPUT then CAPTURE | Negotiates both pools before streaming | OUTPUT then CAPTURE | Keep |
| CAPTURE sizeimage | Compressed-frame heuristic, aligned to 4 KiB | Driver-negotiated pool size | VA coded-buffer capacity | Use FFmpeg heuristic; VA capacity remains independent |
| REQBUFS timing | Both queues before encoder controls | Pools exist before streaming | Encoder controls before both REQBUFS | Move controls after both REQBUFS |
| CAPTURE queueing | Queue all CAPTURE buffers during context init | Queue pool buffers before stream-on | Queue all CAPTURE buffers | Keep |
| EOS event | Subscribe before encoding | Pool/event driven | Not subscribed | Subscribe, tolerate unsupported ioctl |
| B frames | Set 0 before other encode controls | Encoder property | Set after GOP | Set 0 first |
| Frame interval | Set after REQBUFS | Set during final object configuration | Set before REQBUFS | Set after REQBUFS |
| Header mode | Explicit SEPARATE | Driver property negotiation | Kernel default JOINED_WITH_1ST_FRAME | Explicit SEPARATE |
| Rate control | Frame RC enabled; kernel default VBR | Encoder property | CQP emulated as explicit VBR after 0.5.1 | Keep VBR and omit redundant mode ioctl |
| Bitrate | Set before frame-RC enable | Encoder property | Set after frame-RC enable | Set before frame-RC enable |
| GOP | Set after frame-RC enable | Encoder property | Set before B-frame reset | Set after B-frame reset |
| H.264 profile | Set High | Negotiated codec profile | Set High | Keep |
| H.264 level | Kernel default; Raphael patch 0029 turns Level 1 default into firmware-auto above Level 1 limits | Negotiated | Firmware-auto trigger after 0.4.5 | Keep |
| QP range | Explicit minimum/maximum | Negotiated controls | Kernel defaults | Set 1..51 like FFmpeg |
| CABAC/8x8 | Leaves kernel defaults | Derived from caps | Mirrors GNOME picture parameters | Keep, because firmware produces the actual headers |
| First OUTPUT frame | QBUF before either STREAMON | QBUF before starting the pool | Fixed in 0.5.0 | Keep |
| Queue start | OUTPUT STREAMON, then CAPTURE STREAMON | Queue then stream-on | Fixed in 0.5.0 | Keep |
| OUTPUT bytesused | Actual packed NV12 payload | Actual memory payload | Temporarily changed to full plane in 0.4.3 | Use actual payload |
| OUTPUT field | Frame/format field | Explicit format field | Zero/ANY | Set negotiated NONE explicitly |
| Keyframe request | FORCE_KEY_FRAME for requested I pictures | Encoder force-key-unit event | Ignored after the first frame | Forward later IDR requests |

## Kernel coupling

Raphael kernel patch 0032 makes control timing observable. IRIS1 creates its
firmware session and applies one property suite during REQBUFS. Successful
S_FMT, S_SELECTION, S_PARM, or encoder-control changes mark that cached suite
dirty. STREAMON applies the suite again only when dirty.

The pre-0.6.0 backend set every final control before REQBUFS. It therefore
made the requirements query and internal-buffer setup use a final high-rate
configuration, then asked STREAMON to reuse the cached property suite. The
working FFmpeg path allocates both queues first, changes controls afterward,
and deliberately enters STREAMON with a dirty configuration. Version 0.6.0
matches that path.

Raphael patches 0027 and 0028 make firmware's IRIS1 compressed-output
requirement authoritative. Passing the VA coded-buffer capacity through
CAPTURE S_FMT mixed two independent contracts: V4L2 allocation size and the
client-visible destination capacity. Version 0.6.0 calculates the same
compressed-frame request used by FFmpeg while retaining the larger VA coded
buffer for safe packet copying.

Raphael patch 0010 selects two VPP pipes for H.264 above the 720p30 CBR
threshold. The Android Q downstream additionally configures VBV and low
latency for high-load CBR. The backend therefore exposes CQP for GNOME but
uses the ordinary VBR frame-RC path, rather than claiming native CQP or
forcing the incomplete high-load CBR contract.

## Rejected explanations

The following changes altered symptoms or removed a mismatch but did not
resolve the observed STREAMON failure by themselves:

- increasing the requested OUTPUT buffer count from 4 to 16; kernel patch
  0041 intentionally negotiates the IRIS1 raw queue back to firmware's depth;
- padding `bytesused` to the full raw plane; FFmpeg and GStreamer submit the
  actual payload;
- limiting 60 fps to 30 fps; this violated the target requirement and did not
  resolve the failure;
- changing only H.264 level; firmware-auto is correct for patch 0029, but it
  is not sufficient while the surrounding session lifecycle differs;
- changing only CBR to VBR; rate mode is one part of the final control suite,
  while control timing, header mode, and CAPTURE allocation remained wrong;
- changing only 2352x1088 to 2340x1080; this fixes visible geometry but does
  not repair session construction.

## 0.6.0 implementation boundary

Version 0.6.0 changes the session as one unit:

1. Set raw OUTPUT and coded CAPTURE formats.
2. Request and map OUTPUT buffers.
3. Request, map, and queue CAPTURE buffers.
4. Subscribe to EOS events.
5. Apply the FFmpeg-compatible final control sequence.
6. Pack and queue the first NV12 frame with explicit field and data offset.
7. STREAMON OUTPUT.
8. STREAMON CAPTURE.
9. Forward later IDR requests through FORCE_KEY_FRAME.

It preserves 2340x1080 at 60 fps. It does not introduce a frame-rate cap,
modify the kernel, or claim native constant-QP behavior.

## Validation boundary

Host validation must include GCC and Clang builds, all Meson tests, diff
checks, shell syntax checks, and Clang static analysis. These checks prove
memory and API contracts that do not need Venus hardware. The remaining
hardware acceptance condition is one GNOME RDP session that proceeds beyond
CAPTURE STREAMON, emits at least one non-empty H.264 coded buffer, and remains
connected.
