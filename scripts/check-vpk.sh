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

total=$(unzip -l "$VPK" | tail -1 | awk '{print $1}')
if [ "$total" -gt "$MAX_BYTES" ]; then
  echo "check-vpk: VPK is ${total} bytes (> ${MAX_BYTES}); game data may have been bundled" >&2
  fail=1
fi

[ $fail -eq 0 ] && echo "check-vpk: OK ($(unzip -Z1 "$VPK" | wc -l | tr -d ' ') files, ${total} bytes, no game data)"
exit $fail
