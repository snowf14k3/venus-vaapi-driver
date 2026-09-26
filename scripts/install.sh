#!/usr/bin/env bash
set -Eeuo pipefail

for command in meson ninja pkg-config cc; do
    command -v "${command}" >/dev/null 2>&1 || {
        echo "Missing command: ${command}" >&2
        exit 1
    }
done

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${ROOT}/build"

if (( EUID == 0 )); then
    SUDO=()
else
    command -v sudo >/dev/null 2>&1 || {
        echo "Missing sudo" >&2
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
    echo "Installation completed, but venus_drv_video.so was not found under /usr/lib" >&2
    exit 1
fi

ALIAS="$(dirname -- "${DRIVER}")/msm_drv_video.so"
if [[ -e "${ALIAS}" && ! -L "${ALIAS}" ]]; then
    echo "Refusing to overwrite an existing non-symlink: ${ALIAS}" >&2
    exit 1
fi
"${SUDO[@]}" ln -sfn venus_drv_video.so "${ALIAS}"

echo "PASS: Installed ${DRIVER}"
echo "PASS: Pointed the MSM VA-API alias to ${DRIVER}"
echo "Enable in the current shell: export LIBVA_DRIVER_NAME=venus"
echo "Verify: LIBVA_DRIVER_NAME=venus vainfo --display drm --device /dev/dri/renderD128"
