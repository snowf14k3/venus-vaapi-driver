#!/usr/bin/env bash
set -Eeuo pipefail

if (( EUID != 0 )); then
    echo "请使用 sudo 运行此脚本" >&2
    exit 2
fi

for command in ffmpeg timeout sha256sum stat dmesg awk grep; do
    command -v "${command}" >/dev/null 2>&1 || {
        echo "缺少命令: ${command}" >&2
        exit 1
    }
done

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
DECODER="${ROOT}/build/venus-v4l2-decode"
[[ -x "${DECODER}" ]] || {
    echo "缺少 ${DECODER}，请先执行 meson compile -C build" >&2
    exit 1
}

OUT="$(mktemp -d /var/tmp/venus-vaapi-v4l2-h264.XXXXXX)"
MARK="VENUS_VAAPI_V4L2_H264_$(cat /proc/sys/kernel/random/boot_id)_$$"
echo "${MARK}" > /dev/kmsg

echo "日志目录：${OUT}"

ffmpeg -hide_banner -loglevel error -y \
    -f lavfi -i "testsrc2=size=640x480:rate=15" \
    -frames:v 30 -pix_fmt yuv420p \
    -c:v libx264 -profile:v baseline -bf 0 -g 15 \
    -x264-params "aud=1:bframes=0:keyint=15:min-keyint=15:scenecut=0" \
    -f h264 "${OUT}/input.h264"

ffmpeg -hide_banner -loglevel error -y \
    -f h264 -i "${OUT}/input.h264" \
    -frames:v 30 -pix_fmt nv12 \
    -f rawvideo "${OUT}/software.nv12"

set +e
timeout -k 2s 50s "${DECODER}" \
    640 480 30 \
    "${OUT}/input.h264" \
    "${OUT}/hardware.nv12" \
    >"${OUT}/decoder.log" 2>&1
RC=$?
set -e

dmesg |
    awk -v marker="${MARK}" 'index($0, marker) { keep=1 } keep' \
    > "${OUT}/kernel.log"

SOFTWARE_BYTES="$(stat -c %s "${OUT}/software.nv12")"
HARDWARE_BYTES="$(stat -c %s "${OUT}/hardware.nv12" 2>/dev/null || echo 0)"

echo "退出码：${RC}"
echo "软件解码：${SOFTWARE_BYTES} 字节"
echo "硬件解码：${HARDWARE_BYTES} 字节"
cat "${OUT}/decoder.log"

if (( RC == 0 )) &&
   [[ "${SOFTWARE_BYTES}" == "13824000" ]] &&
   [[ "${HARDWARE_BYTES}" == "13824000" ]] &&
   cmp -s "${OUT}/software.nv12" "${OUT}/hardware.nv12"; then
    echo "PASS：自有 V4L2 session 完整解码30帧，与软件逐字节一致"
else
    echo "FAIL"
    grep -E \
        'qcom-venus|session error|dec: event|IOMMU|Oops:|BUG:|Call trace:' \
        "${OUT}/kernel.log" |
        tail -n 80 || true
fi

sha256sum \
    "${OUT}/input.h264" \
    "${OUT}/software.nv12" \
    "${OUT}/hardware.nv12" 2>/dev/null |
    tee "${OUT}/SHA256SUMS"

echo "日志目录：${OUT}"
