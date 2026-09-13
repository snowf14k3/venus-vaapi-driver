#!/usr/bin/env bash
set -Eeuo pipefail

RDP_USER="$(cat /var/tmp/grd-vaapi-user 2>/dev/null || echo user)"
RDP_UID="$(id -u "${RDP_USER}")"
UNIT="$(cat /var/tmp/grd-vaapi-unit)"
SINCE="$(cat /var/tmp/grd-vaapi-since)"
OUT="/var/tmp/grd-rdp-vaapi-result.log"

if (( EUID == 0 )); then
    journalctl -b --since "${SINCE}" \
        _UID="${RDP_UID}" \
        _SYSTEMD_USER_UNIT="${UNIT}" \
        --no-pager >"${OUT}"
else
    journalctl --user -b --since "${SINCE}" \
        -u "${UNIT}" --no-pager >"${OUT}"
fi

grep -E \
    'HWAccel.VAAPI|Did not initialize VAAPI|venus-vaapi:|EncodeFrame\[Times\]|Could not create VAAPI' \
    "${OUT}" || true

if grep -q \
       '\[HWAccel.VAAPI\] Successfully initialized VAAPI' \
       "${OUT}" &&
   grep -q \
       '\[HWAccel.VAAPI\] Created VAAPI encode session' \
       "${OUT}" &&
   grep -q 'venus-vaapi: encoder-open' "${OUT}" &&
   grep -q 'venus-vaapi: encoded buffer=' "${OUT}"; then
    echo "PASS：GNOME RDP 正在使用 Venus VAAPI H.264 硬件编码"
else
    echo "FAIL：GNOME RDP 尚未完成 Venus VAAPI 硬件编码"
    echo "完整日志：${OUT}"
    exit 1
fi

echo "完整日志：${OUT}"
