#!/usr/bin/env bash
set -Eeuo pipefail

if (( EUID != 0 )); then
    echo "请使用 sudo 运行此脚本" >&2
    exit 2
fi

for command in ffmpeg ffprobe vainfo timeout stat dmesg awk grep find cmp wc; do
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

OUT="$(mktemp -d /var/tmp/venus-vaapi-h264-matrix.XXXXXX)"
MARK="VENUS_VAAPI_H264_MATRIX_$(cat /proc/sys/kernel/random/boot_id)_$$"
echo "${MARK}" > /dev/kmsg
echo "日志目录：${OUT}"
echo "drm=${DRM}"

VA_ENV=(
    LIBVA_DRIVERS_PATH="${DRIVER_DIR}"
    LIBVA_DRIVER_NAME=venus
    LIBVA_MESSAGING_LEVEL=2
    VENUS_VAAPI_LOG=1
)

set +e
env "${VA_ENV[@]}" \
    vainfo --display drm --device "${DRM}" \
    >"${OUT}/vainfo.log" 2>&1
VAINFO_RC=$?
set -e

echo "vainfo退出码：${VAINFO_RC}"
grep -E 'Driver version|VAProfileH264|va_openDriver' \
    "${OUT}/vainfo.log" || true

FAILED=0
PASSED=0

run_case()
{
    local name="$1"
    local width="$2"
    local height="$3"
    local fps="$4"
    local frames="$5"
    local bitrate="$6"
    local directory="${OUT}/${name}"
    local encoded_bytes
    local probed_frames
    local software_frames
    local hardware_frames
    local encode_rc
    local software_rc
    local hardware_rc
    local probe_rc

    mkdir -p "${directory}"
    echo "=== ${name}: ${width}x${height}@${fps}, ${frames}帧 ==="

    set +e
    timeout -k 3s 180s env "${VA_ENV[@]}" \
        ffmpeg -hide_banner -loglevel verbose -y -nostdin \
        -init_hw_device "vaapi=venus:${DRM}" \
        -filter_hw_device venus \
        -f lavfi -i "testsrc2=size=${width}x${height}:rate=${fps}" \
        -frames:v "${frames}" \
        -vf "format=nv12,hwupload" \
        -c:v h264_vaapi -profile:v high \
        -rc_mode CBR -b:v "${bitrate}" \
        -maxrate "${bitrate}" -bufsize "$((bitrate * 2))" \
        -g "${fps}" -bf 0 \
        -f h264 "${directory}/output.h264" \
        >"${directory}/encode.log" 2>&1
    encode_rc=$?

    ffprobe -v error -count_frames -select_streams v:0 \
        -show_entries stream=codec_name,profile,width,height,nb_read_frames \
        -of default=nw=1 "${directory}/output.h264" \
        >"${directory}/probe.txt" 2>"${directory}/probe.log"
    probe_rc=$?

    timeout -k 3s 180s \
        ffmpeg -hide_banner -loglevel error -y -nostdin \
        -c:v h264 -f h264 -i "${directory}/output.h264" \
        -frames:v "${frames}" -pix_fmt nv12 \
        -f framemd5 "${directory}/software.md5" \
        >"${directory}/software.log" 2>&1
    software_rc=$?

    timeout -k 3s 180s env "${VA_ENV[@]}" \
        ffmpeg -hide_banner -loglevel verbose -y -nostdin \
        -hwaccel vaapi -hwaccel_device "${DRM}" \
        -hwaccel_output_format vaapi \
        -f h264 -i "${directory}/output.h264" \
        -frames:v "${frames}" \
        -vf "hwdownload,format=nv12" \
        -pix_fmt nv12 -f framemd5 "${directory}/hardware.md5" \
        >"${directory}/hardware.log" 2>&1
    hardware_rc=$?
    set -e

    encoded_bytes="$(stat -c %s "${directory}/output.h264" 2>/dev/null || echo 0)"
    probed_frames="$(awk -F= '$1 == "nb_read_frames" { print $2 }' \
        "${directory}/probe.txt" 2>/dev/null || true)"

    awk -F',' '/^[0-9]+,/ {
        value=$NF
        gsub(/^[[:space:]]+|[[:space:]]+$/, "", value)
        print value
    }' "${directory}/software.md5" \
        >"${directory}/software.frames" 2>/dev/null || true
    awk -F',' '/^[0-9]+,/ {
        value=$NF
        gsub(/^[[:space:]]+|[[:space:]]+$/, "", value)
        print value
    }' "${directory}/hardware.md5" \
        >"${directory}/hardware.frames" 2>/dev/null || true

    software_frames="$(wc -l <"${directory}/software.frames")"
    hardware_frames="$(wc -l <"${directory}/hardware.frames")"

    echo "encode=${encode_rc} probe=${probe_rc} swdecode=${software_rc} hwdecode=${hardware_rc}"
    echo "encoded_bytes=${encoded_bytes} probed_frames=${probed_frames:-0}"
    echo "software_frames=${software_frames} hardware_frames=${hardware_frames}"
    cat "${directory}/probe.txt" 2>/dev/null || true

    if (( encode_rc == 0 )) &&
       (( probe_rc == 0 )) &&
       (( software_rc == 0 )) &&
       (( hardware_rc == 0 )) &&
       (( encoded_bytes > 0 )) &&
       [[ "${probed_frames}" == "${frames}" ]] &&
       [[ "${software_frames}" == "${frames}" ]] &&
       [[ "${hardware_frames}" == "${frames}" ]] &&
       grep -qx "codec_name=h264" "${directory}/probe.txt" &&
       grep -qx "width=${width}" "${directory}/probe.txt" &&
       grep -qx "height=${height}" "${directory}/probe.txt" &&
       cmp -s "${directory}/software.frames" \
              "${directory}/hardware.frames"; then
        echo "PASS ${name}"
        PASSED=$((PASSED + 1))
    else
        echo "FAIL ${name}"
        FAILED=$((FAILED + 1))
        tail -n 40 "${directory}/encode.log" || true
        tail -n 30 "${directory}/software.log" || true
        tail -n 50 "${directory}/hardware.log" || true
    fi
}

run_case sd-640x480      640  480 15  30 1000000
run_case hd-1280x720    1280  720 30  90 4000000
run_case fhd-1920x1080  1920 1080 30  60 8000000
run_case stress-720p    1280  720 30 300 4000000

dmesg |
    awk -v marker="${MARK}" 'index($0, marker) { keep=1 } keep' \
    >"${OUT}/kernel.log"

if grep -Eq \
    'session error|IOMMU.*fault|Unhandled fault|(^|] )Oops:|(^|] )BUG:|Call trace:|Kernel panic|watchdog: BUG' \
    "${OUT}/kernel.log"; then
    echo "FAIL：测试期间出现内核/Venus异常"
    FAILED=$((FAILED + 1))
    grep -E \
        'qcom-venus|session error|IOMMU|Unhandled fault|Oops:|BUG:|Call trace:|Kernel panic|watchdog' \
        "${OUT}/kernel.log" | tail -n 120 || true
else
    echo "PASS：测试期间没有新增内核/Venus异常"
fi

echo "=== 汇总 ==="
echo "通过：${PASSED}/4"
echo "失败：${FAILED}"
echo "日志目录：${OUT}"

if (( VAINFO_RC != 0 )) ||
   ! grep -Eq \
       'VAProfileH264High[[:space:]]*:[[:space:]]*VAEntrypointEncSlice' \
       "${OUT}/vainfo.log" ||
   (( FAILED != 0 )) ||
   (( PASSED != 4 )); then
    exit 1
fi

echo "PASS：H.264 VAAPI 编解码矩阵全部通过"
