#!/usr/bin/env bash
set -Eeuo pipefail

if (( EUID != 0 )); then
    echo "请使用 sudo 运行此脚本" >&2
    exit 2
fi

for command in ffmpeg ffprobe vainfo timeout stat dmesg awk grep find; do
    command -v "${command}" >/dev/null 2>&1 || {
        echo "缺少命令: ${command}" >&2
        exit 1
    }
done

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
DRIVER_DIR="${ROOT}/build"
DRIVER="${DRIVER_DIR}/venus_drv_video.so"
[[ -s "${DRIVER}" ]] || {
    echo "缺少 ${DRIVER}，请先执行 meson compile -C build" >&2
    exit 1
}

DRM="$(find /dev/dri -maxdepth 1 -type c -name 'renderD*' -print -quit)"
[[ -n "${DRM}" ]] || {
    echo "没有找到 DRM render node" >&2
    exit 1
}

OUT="$(mktemp -d /var/tmp/venus-vaapi-h264-encode.XXXXXX)"
MARK="VENUS_VAAPI_H264_ENCODE_$(cat /proc/sys/kernel/random/boot_id)_$$"
echo "${MARK}" > /dev/kmsg
echo "日志目录：${OUT}"
echo "drm=${DRM}"

set +e
env LIBVA_DRIVERS_PATH="${DRIVER_DIR}" \
    LIBVA_DRIVER_NAME=venus \
    LIBVA_MESSAGING_LEVEL=2 \
    VENUS_VAAPI_LOG=1 \
    vainfo --display drm --device "${DRM}" \
    >"${OUT}/vainfo.log" 2>&1
VAINFO_RC=$?

timeout -k 2s 60s env \
    LIBVA_DRIVERS_PATH="${DRIVER_DIR}" \
    LIBVA_DRIVER_NAME=venus \
    LIBVA_MESSAGING_LEVEL=2 \
    VENUS_VAAPI_LOG=1 \
    ffmpeg -hide_banner -loglevel verbose -y -nostdin \
    -init_hw_device "vaapi=venus:${DRM}" \
    -filter_hw_device venus \
    -f lavfi -i "testsrc2=size=640x480:rate=15" \
    -frames:v 30 -vf "format=nv12,hwupload" \
    -c:v h264_vaapi -profile:v high \
    -rc_mode CBR -b:v 1M -maxrate 1M -bufsize 2M \
    -g 15 -bf 0 -f h264 "${OUT}/output.h264" \
    >"${OUT}/ffmpeg.log" 2>&1
ENCODE_RC=$?

ffmpeg -hide_banner -loglevel error -y \
    -f h264 -i "${OUT}/output.h264" \
    -frames:v 30 -pix_fmt nv12 \
    -f rawvideo "${OUT}/decoded.nv12" \
    >"${OUT}/decode.log" 2>&1
DECODE_RC=$?
set -e

dmesg |
    awk -v marker="${MARK}" 'index($0, marker) { keep=1 } keep' \
    > "${OUT}/kernel.log"

ENCODED_BYTES="$(stat -c %s "${OUT}/output.h264" 2>/dev/null || echo 0)"
DECODED_BYTES="$(stat -c %s "${OUT}/decoded.nv12" 2>/dev/null || echo 0)"
DECODED_FRAMES="$(ffprobe -v error -count_frames -select_streams v:0 \
    -show_entries stream=nb_read_frames -of default=nw=1:nk=1 \
    "${OUT}/output.h264" 2>/dev/null || echo 0)"

echo "vainfo退出码：${VAINFO_RC}"
grep -E 'Driver version|VAProfileH264|va_openDriver' \
    "${OUT}/vainfo.log" || true
echo "VAAPI编码退出码：${ENCODE_RC}"
echo "软件回验退出码：${DECODE_RC}"
echo "H.264字节：${ENCODED_BYTES}"
echo "软件解码字节：${DECODED_BYTES}"
echo "软件解码帧数：${DECODED_FRAMES}"

grep -E 'venus-vaapi:|VAAPI driver|Failed|Error' \
    "${OUT}/ffmpeg.log" | tail -n 180 || true

if (( VAINFO_RC == 0 )) &&
   (( ENCODE_RC == 0 )) &&
   (( DECODE_RC == 0 )) &&
   (( ENCODED_BYTES > 0 )) &&
   [[ "${DECODED_BYTES}" == "13824000" ]] &&
   [[ "${DECODED_FRAMES}" == "30" ]] &&
   grep -Eq \
       'VAProfileH264High[[:space:]]*:[[:space:]]*VAEntrypointEncSlice' \
       "${OUT}/vainfo.log"; then
    echo "PASS：H.264 VAAPI硬编30帧，软件完整解码30帧"
else
    echo "FAIL"
    tail -n 50 "${OUT}/ffmpeg.log"
    tail -n 30 "${OUT}/decode.log"
    grep -E \
        'qcom-venus|session error|enc: event|IOMMU|Oops:|BUG:|Call trace:' \
        "${OUT}/kernel.log" |
        tail -n 100 || true
fi

echo "日志目录：${OUT}"
