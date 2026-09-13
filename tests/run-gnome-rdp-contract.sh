#!/usr/bin/env bash
set -Eeuo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
CHECKER="${ROOT}/build/venus-gnome-rdp-check"
DRIVER_DIR="${ROOT}/build"
RDP_USER="${RDP_USER:-user}"

[[ -x "${CHECKER}" ]] || {
    echo "缺少 ${CHECKER}，请先编译" >&2
    exit 1
}

DRM="$(find /dev/dri -maxdepth 1 -type c -name 'renderD*' -print -quit)"
[[ -n "${DRM}" ]] || {
    echo "没有找到 DRM render node 节点" >&2
    exit 1
}

HEAP=""
for candidate in \
    /dev/dma_heap/system \
    /dev/dma_heap/linux,cma \
    /dev/dma_heap/default_cma_region; do
    if [[ -e "${candidate}" ]]; then
        HEAP="${candidate}"
        break
    fi
done
[[ -n "${HEAP}" ]] || {
    echo "没有找到可用的 DMA-BUF heap" >&2
    exit 1
}

OUT="$(mktemp -d /var/tmp/venus-gnome-rdp-contract.XXXXXX)"
echo "日志目录：${OUT}"
echo "drm=${DRM}"
echo "dma_heap=${HEAP}"
echo "rdp_user=${RDP_USER}"

RUN=()
if (( EUID == 0 )) && [[ "${RDP_USER}" != root ]]; then
    RUN=(sudo -u "${RDP_USER}")
fi

"${RUN[@]}" test -r "${DRM}"
"${RUN[@]}" test -r "${HEAP}"
"${RUN[@]}" test -w "${HEAP}"

"${RUN[@]}" env \
    LIBVA_DRIVERS_PATH="${DRIVER_DIR}" \
    LIBVA_DRIVER_NAME=venus \
    VENUS_VAAPI_LOG=1 \
    "${CHECKER}" "${DRM}" 2>&1 |
    tee "${OUT}/contract.log"

echo "PASS：GNOME RDP 用户可访问 render node 和 DMA-BUF heap"
echo "日志目录：${OUT}"
