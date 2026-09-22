#!/usr/bin/env bash
set -Eeuo pipefail

for command in meson ninja pkg-config cc udevadm; do
    command -v "${command}" >/dev/null 2>&1 || {
        echo "缺少命令: ${command}" >&2
        exit 1
    }
done

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

cd "${ROOT}"

if [[ -f "${BUILD}/meson-private/coredata.dat" ]]; then
    meson setup --reconfigure --prefix=/usr "${BUILD}"
else
    meson setup --prefix=/usr "${BUILD}"
fi

meson compile -C "${BUILD}"
"${SUDO[@]}" meson install -C "${BUILD}"

UDEV_RULE="${ROOT}/data/70-venus-vaapi-dma-heap.rules"
"${SUDO[@]}" install -D -m 0644 "${UDEV_RULE}" \
    /etc/udev/rules.d/70-venus-vaapi-dma-heap.rules
"${SUDO[@]}" udevadm control --reload-rules
"${SUDO[@]}" udevadm trigger --subsystem-match=dma_heap --action=change || true
if [[ -e /dev/dma_heap/system ]]; then
    "${SUDO[@]}" chmod 0666 /dev/dma_heap/system
fi

MULTIARCH="$(cc -print-multiarch 2>/dev/null || true)"
DRIVER=""
if [[ -n "${MULTIARCH}" &&
      -s "/usr/lib/${MULTIARCH}/dri/venus_drv_video.so" ]]; then
    DRIVER="/usr/lib/${MULTIARCH}/dri/venus_drv_video.so"
else
    DRIVER="$(find /usr/lib /usr/lib64 -type f \
        -path '*/dri/venus_drv_video.so' -print -quit 2>/dev/null || true)"
fi

if [[ -z "${DRIVER}" ]]; then
    echo "安装完成，但没有在 /usr/lib 下找到 venus_drv_video.so" >&2
    exit 1
fi

echo "PASS：已安装 ${DRIVER}"
echo "当前终端启用：export LIBVA_DRIVER_NAME=venus"
echo "验证：LIBVA_DRIVER_NAME=venus vainfo --display drm --device /dev/dri/renderD128"
