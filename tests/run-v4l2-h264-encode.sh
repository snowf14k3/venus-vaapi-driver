#!/usr/bin/env bash
set -Eeuo pipefail

if (( EUID != 0 )); then
    echo "请使用 sudo 运行此脚本" >&2
    exit 2
fi

for command in ffmpeg ffprobe timeout stat dmesg awk grep; do
    command -v "${command}" >/dev/null 2>&1 || {
        echo "缺少命令: ${command}" >&2
        exit 1
    }
done

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
ENCODER="${ROOT}/build/venus-v4l2-encode"
[[ -x "${ENCODER}" ]] || {
    echo "缺少 ${ENCODER}，请先执行 meson compile -C build" >&2
    exit 1
}

OUT="$(mktemp -d /var/tmp/venus-vaapi-v4l2-encode.XXXXXX)"
MARK="VENUS_VAAPI_V4L2_ENCODE_$(cat /proc/sys/kernel/random/boot_id)_$$"
echo "${MARK}" > /dev/kmsg
echo "日志目录：${OUT}"

ffmpeg -hide_banner -loglevel error -y \
    -f lavfi -i "testsrc2=size=640x480:rate=15" \
    -frames:v 30 -vf format=nv12 \
    -pix_fmt nv12 -f rawvideo "${OUT}/input.nv12"

set +e
timeout -k 2s 50s "${ENCODER}" \
    640 480 15 30 1000000 \
    "${OUT}/input.nv12" "${OUT}/output.h264" \
    >"${OUT}/encoder.log" 2>&1
ENCODER_RC=$?

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

echo "编码退出码：${ENCODER_RC}"
echo "软件回验退出码：${DECODE_RC}"
echo "H.264字节：${ENCODED_BYTES}"
echo "软件解码字节：${DECODED_BYTES}"
echo "软件解码帧数：${DECODED_FRAMES}"
cat "${OUT}/encoder.log"

if (( ENCODER_RC == 0 )) &&
   (( DECODE_RC == 0 )) &&
   (( ENCODED_BYTES > 0 )) &&
   [[ "${DECODED_BYTES}" == "13824000" ]] &&
   [[ "${DECODED_FRAMES}" == "30" ]]; then
    echo "PASS：自有 V4L2 session 完成30帧 NV12→H.264，软件完整解码30帧"
else
    echo "FAIL"
    tail -n 30 "${OUT}/decode.log"
    grep -E \
        'qcom-venus|session error|enc: event|IOMMU|Oops:|BUG:|Call trace:' \
        "${OUT}/kernel.log" |
        tail -n 80 || true
fi

echo "日志目录：${OUT}"
