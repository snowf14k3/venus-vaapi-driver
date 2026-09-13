#!/usr/bin/env bash
set -Eeuo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${ROOT}/build"

if (( EUID == 0 )); then
    SUDO=()
else
    command -v sudo >/dev/null 2>&1 || {
        echo "缺少 sudo" >&2
        exit 1
    }
    SUDO=(sudo)
fi

for command in meson ninja ffmpeg ffprobe vainfo install tee find; do
    command -v "${command}" >/dev/null 2>&1 || {
        echo "缺少命令: ${command}" >&2
        exit 1
    }
done

cd "${ROOT}"

if [[ -f "${BUILD}/meson-private/coredata.dat" ]]; then
    meson setup --reconfigure --prefix=/usr "${BUILD}"
else
    meson setup --prefix=/usr "${BUILD}"
fi

meson compile -C "${BUILD}"
meson test -C "${BUILD}" --print-errorlogs

"${SUDO[@]}" "${ROOT}/tests/run-vaapi-h264-matrix.sh"
"${SUDO[@]}" meson install -C "${BUILD}"

"${SUDO[@]}" install -d /etc/environment.d /etc/profile.d
printf '%s\n' 'LIBVA_DRIVER_NAME=venus' |
    "${SUDO[@]}" tee /etc/environment.d/90-venus-vaapi.conf >/dev/null
printf '%s\n' 'export LIBVA_DRIVER_NAME=venus' |
    "${SUDO[@]}" tee /etc/profile.d/venus-vaapi.sh >/dev/null

DRM="$(find /dev/dri -maxdepth 1 -type c -name 'renderD*' -print -quit)"
"${SUDO[@]}" env LIBVA_DRIVER_NAME=venus \
    vainfo --display drm --device "${DRM}" \
    > /var/tmp/venus-vaapi-installed-vainfo.log 2>&1

echo "PASS：H.264 VAAPI 矩阵通过，驱动已安装"
echo "当前终端执行：export LIBVA_DRIVER_NAME=venus"
echo "重新登录后会自动选择 venus 驱动"
echo "vainfo日志：/var/tmp/venus-vaapi-installed-vainfo.log"
echo "使用说明：${ROOT}/docs/usage.md"
