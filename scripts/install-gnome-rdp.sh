#!/usr/bin/env bash
set -Eeuo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${ROOT}/build"
RDP_USER="${RDP_USER:-user}"

if (( EUID == 0 )); then
    SUDO=()
    USER_SYSTEMCTL=(
        systemctl --machine="${RDP_USER}@.host" --user
    )
else
    SUDO=(sudo)
    if [[ "$(id -un)" != "${RDP_USER}" ]]; then
        echo "请由 root 或 RDP 用户 ${RDP_USER} 运行" >&2
        exit 1
    fi
    USER_SYSTEMCTL=(systemctl --user)
fi

cd "${ROOT}"
if [[ -f "${BUILD}/meson-private/coredata.dat" ]]; then
    meson setup --reconfigure --prefix=/usr "${BUILD}"
else
    meson setup --prefix=/usr "${BUILD}"
fi
meson compile -C "${BUILD}"
meson test -C "${BUILD}" --print-errorlogs
"${SUDO[@]}" "${ROOT}/tests/run-vaapi-h264-encode.sh"
"${SUDO[@]}" "${ROOT}/tests/run-vaapi-h264-cqp.sh"

RDP_USER="${RDP_USER}" \
    "${ROOT}/tests/run-gnome-rdp-contract.sh"

"${SUDO[@]}" meson install -C "${BUILD}"

"${USER_SYSTEMCTL[@]}" set-environment \
    LIBVA_DRIVER_NAME=venus \
    VENUS_VAAPI_LOG=1 \
    G_MESSAGES_DEBUG=all \
    GNOME_REMOTE_DESKTOP_DEBUG=vkva-renderer,va-times

if "${USER_SYSTEMCTL[@]}" is-active --quiet \
        gnome-remote-desktop.service; then
    UNIT=gnome-remote-desktop.service
elif "${USER_SYSTEMCTL[@]}" is-active --quiet \
        gnome-remote-desktop-headless.service; then
    UNIT=gnome-remote-desktop-headless.service
else
    echo "没有找到运行中的 GNOME RDP 用户服务" >&2
    exit 1
fi

date --iso-8601=seconds > /var/tmp/grd-vaapi-since
echo "${UNIT}" > /var/tmp/grd-vaapi-unit
echo "${RDP_USER}" > /var/tmp/grd-vaapi-user

"${USER_SYSTEMCTL[@]}" restart "${UNIT}"
"${USER_SYSTEMCTL[@]}" --no-pager --full status "${UNIT}"

echo "PASS：GNOME RDP VAAPI 合同通过，驱动已安装并重启 ${UNIT}"
echo "现在重新建立 RDP 连接，然后运行 scripts/check-gnome-rdp.sh"
