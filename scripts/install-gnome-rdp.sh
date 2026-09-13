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

INSTALLED_DRIVER="$(
    meson introspect --installed "${BUILD}" |
        python3 -c '
import json
import sys

installed = json.load(sys.stdin)
matches = [destination for source, destination in installed.items()
           if source.endswith("/venus_drv_video.so")]
if len(matches) != 1:
    raise SystemExit(
        f"expected one installed Venus driver, found {len(matches)}")
print(matches[0])
'
)"
if [[ ! -f "${INSTALLED_DRIVER}" ]]; then
    echo "安装后的驱动不存在：${INSTALLED_DRIVER}" >&2
    exit 1
fi
if ! cmp -s "${BUILD}/venus_drv_video.so" "${INSTALLED_DRIVER}"; then
    echo "安装校验失败：构建文件与 ${INSTALLED_DRIVER} 不一致" >&2
    exit 1
fi

echo "已安装驱动：${INSTALLED_DRIVER}"
sha256sum "${BUILD}/venus_drv_video.so" "${INSTALLED_DRIVER}"

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

OLD_PID="$(
    "${USER_SYSTEMCTL[@]}" show "${UNIT}" \
        --property=MainPID --value
)"

date --iso-8601=seconds > /var/tmp/grd-vaapi-since
echo "${UNIT}" > /var/tmp/grd-vaapi-unit
echo "${RDP_USER}" > /var/tmp/grd-vaapi-user

"${USER_SYSTEMCTL[@]}" restart "${UNIT}"
"${USER_SYSTEMCTL[@]}" --no-pager --full status "${UNIT}"

NEW_PID="$(
    "${USER_SYSTEMCTL[@]}" show "${UNIT}" \
        --property=MainPID --value
)"
if [[ -z "${NEW_PID}" || "${NEW_PID}" == 0 ||
      "${NEW_PID}" == "${OLD_PID}" ]]; then
    echo "服务重启校验失败：旧 PID=${OLD_PID:-unknown} 新 PID=${NEW_PID:-unknown}" >&2
    exit 1
fi

echo "服务 PID：${OLD_PID:-unknown} -> ${NEW_PID}"

echo "PASS：GNOME RDP VAAPI 合同通过，驱动已安装并重启 ${UNIT}"
echo "现在重新建立 RDP 连接，然后运行 scripts/check-gnome-rdp.sh"
