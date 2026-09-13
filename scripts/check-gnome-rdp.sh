#!/usr/bin/env bash
set -Eeuo pipefail

RDP_USER="$(cat /var/tmp/grd-vaapi-user 2>/dev/null || echo user)"
RDP_UID="$(id -u "${RDP_USER}")"
UNIT="$(cat /var/tmp/grd-vaapi-unit)"
SINCE="$(cat /var/tmp/grd-vaapi-since)"
OUT="/var/tmp/grd-rdp-vaapi-result.log"
KERNEL_OUT="/var/tmp/grd-rdp-vaapi-kernel.log"
HEADER_TAG=

if (( EUID == 0 )); then
    USER_SYSTEMCTL=(
        systemctl --machine="${RDP_USER}@.host" --user
    )
    journalctl -b --since "${SINCE}" \
        _UID="${RDP_UID}" \
        _SYSTEMD_USER_UNIT="${UNIT}" \
        --no-pager >"${OUT}"
    journalctl -k -b --since "${SINCE}" \
        --no-pager >"${KERNEL_OUT}"
else
    USER_SYSTEMCTL=(systemctl --user)
    journalctl --user -b --since "${SINCE}" \
        -u "${UNIT}" --no-pager >"${OUT}"
    : >"${KERNEL_OUT}"
fi

grep -E \
    'HWAccel.VAAPI|Did not initialize VAAPI|venus-vaapi:|EncodeFrame\[Times\]|Could not create VAAPI' \
    "${OUT}" || true

HEADER_LINE="$(
    grep -m1 'venus-vaapi: encoded part .*complete=0' \
        "${OUT}" || true
)"
if [[ -n "${HEADER_LINE}" ]]; then
    HEADER_TAG="$(
        printf '%s\n' "${HEADER_LINE}" |
            sed -nE 's/.*driver-tag=(0x[0-9a-f]+).*/\1/p'
    )"
fi

if grep -q \
       '\[HWAccel.VAAPI\] Successfully initialized VAAPI' \
       "${OUT}" &&
   grep -q \
       '\[HWAccel.VAAPI\] Created VAAPI encode session' \
       "${OUT}" &&
   grep -Eq \
       'venus-vaapi: encoder-open .*bitrate=[0-9]+ qp=22 range=20\.\.24' \
       "${OUT}" &&
   [[ -n "${HEADER_TAG}" ]] &&
   grep -Fq \
       "driver-tag=${HEADER_TAG} complete=1" "${OUT}" &&
   grep -Eq \
       'venus-vaapi: encoded buffer=.*packets=2 .*complete=1' \
       "${OUT}" &&
   ! grep -Eq \
       'encoder-(open|submit|pump) failed|end-picture encode failed|Failed to (sync surface|map output buffer)' \
       "${OUT}" &&
   ! grep -Eq \
       'qcom-venus.*session error|IRIS1 encoder.*failed' \
       "${KERNEL_OUT}" &&
   "${USER_SYSTEMCTL[@]}" is-active --quiet "${UNIT}"; then
    echo "PASS：GNOME RDP 收到同帧聚合的完整 Venus H.264 码流"
else
    echo "FAIL：GNOME RDP 的 Venus H.264 完整帧合同未通过"
    if [[ -s "${KERNEL_OUT}" ]]; then
        grep -E \
            'qcom-venus.*session error|IRIS1 encoder.*failed' \
            "${KERNEL_OUT}" || true
    fi
    echo "完整日志：${OUT}"
    echo "内核日志：${KERNEL_OUT}"
    exit 1
fi

echo "完整日志：${OUT}"
echo "内核日志：${KERNEL_OUT}"
