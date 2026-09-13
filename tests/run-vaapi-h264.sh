#!/usr/bin/env bash
set -Eeuo pipefail

if (( EUID != 0 )); then
    echo "请使用 sudo 运行此脚本" >&2
    exit 2
fi

for command in ffmpeg vainfo timeout sha256sum stat dmesg awk grep; do
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

OUT="$(mktemp -d /var/tmp/venus-vaapi-h264.XXXXXX)"
MARK="VENUS_VAAPI_H264_$(cat /proc/sys/kernel/random/boot_id)_$$"
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
set -e

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
timeout -k 2s 60s env \
    LIBVA_DRIVERS_PATH="${DRIVER_DIR}" \
    LIBVA_DRIVER_NAME=venus \
    LIBVA_MESSAGING_LEVEL=2 \
    VENUS_VAAPI_LOG=1 \
    ffmpeg -hide_banner -loglevel verbose -y -nostdin \
    -hwaccel vaapi -hwaccel_device "${DRM}" \
    -hwaccel_output_format vaapi \
    -f h264 -i "${OUT}/input.h264" \
    -frames:v 30 -vf "hwdownload,format=nv12" \
    -pix_fmt nv12 -f rawvideo "${OUT}/hardware.nv12" \
    >"${OUT}/ffmpeg.log" 2>&1
FFMPEG_RC=$?
set -e

dmesg |
    awk -v marker="${MARK}" 'index($0, marker) { keep=1 } keep' \
    > "${OUT}/kernel.log"

SOFTWARE_BYTES="$(stat -c %s "${OUT}/software.nv12")"
HARDWARE_BYTES="$(stat -c %s "${OUT}/hardware.nv12" 2>/dev/null || echo 0)"

echo "vainfo退出码：${VAINFO_RC}"
grep -E 'Driver version|VAProfileH264|va_openDriver' \
    "${OUT}/vainfo.log" || true

echo "FFmpeg退出码：${FFMPEG_RC}"
echo "软件解码：${SOFTWARE_BYTES} 字节"
echo "VAAPI解码：${HARDWARE_BYTES} 字节"

grep -E 'venus-vaapi:|VAAPI driver|Using hardware decoding|Failed|Error' \
    "${OUT}/ffmpeg.log" | tail -n 160 || true

if (( VAINFO_RC == 0 )) &&
   (( FFMPEG_RC == 0 )) &&
   [[ "${SOFTWARE_BYTES}" == "13824000" ]] &&
   [[ "${HARDWARE_BYTES}" == "13824000" ]] &&
   cmp -s "${OUT}/software.nv12" "${OUT}/hardware.nv12"; then
    echo "PASS：H.264 VAAPI硬解30帧，与软件逐字节一致"
else
    echo "FAIL"
    tail -n 40 "${OUT}/ffmpeg.log"
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
