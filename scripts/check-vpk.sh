#!/usr/bin/env bash
#
# Fails if a VPK contains anything other than the port's own files.
# Game data (APK, OBB, .so, assets) must only ever live in ux0:data/ffvii-vita/.
#
# Usage: scripts/check-vpk.sh build/ff7_vita.vpk
#
set -euo pipefail

VPK="${1:?usage: $0 file.vpk}"

# Allow-list: only the loader, LiveArea and (eventually) precompiled shaders
ALLOWED='^(eboot\.bin|cpuinfo|meminfo|urandom|sce_sys/.*|shaders/.*\.gxp)$'
MAX_BYTES=$((16 * 1024 * 1024))

fail=0
while read -r name; do
  if ! [[ "$name" =~ $ALLOWED ]]; then
    echo "check-vpk: unexpected file in VPK: $name" >&2
    fail=1
  fi
done < <(unzip -Z1 "$VPK")

# LiveArea images: the Vita refuses to install the VPK (error 0x8011013D) unless
# they have these exact sizes and are 8-bit paletted PNGs
check_png() { # path-in-vpk width height
  local b
  b=($( { unzip -p "$VPK" "$1" 2>/dev/null || true; } | head -c 26 | od -An -tu1))
  if [ ${#b[@]} -lt 26 ]; then
    echo "check-vpk: $1 missing" >&2; fail=1; return
  fi
  local w=$(( (b[16] << 24) | (b[17] << 16) | (b[18] << 8) | b[19] ))
  local h=$(( (b[20] << 24) | (b[21] << 16) | (b[22] << 8) | b[23] ))
  if [ "$w" -ne "$2" ] || [ "$h" -ne "$3" ] || [ "${b[24]}" -ne 8 ] || [ "${b[25]}" -ne 3 ]; then
    echo "check-vpk: $1 is ${w}x${h}, bit depth ${b[24]}, color type ${b[25]}; must be $2x$3 8-bit paletted (color type 3)" >&2
    fail=1
  fi
}
check_png sce_sys/icon0.png 128 128
check_png sce_sys/pic0.png 960 544
check_png sce_sys/livearea/contents/bg0.png 840 500
check_png sce_sys/livearea/contents/startup.png 280 158

total=$(unzip -l "$VPK" | tail -1 | awk '{print $1}')
if [ "$total" -gt "$MAX_BYTES" ]; then
  echo "check-vpk: VPK is ${total} bytes (> ${MAX_BYTES}); game data may have been bundled" >&2
  fail=1
fi

[ $fail -eq 0 ] && echo "check-vpk: OK ($(unzip -Z1 "$VPK" | wc -l | tr -d ' ') files, ${total} bytes, no game data)"
exit $fail
