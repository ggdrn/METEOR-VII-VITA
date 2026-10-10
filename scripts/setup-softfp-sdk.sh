#!/usr/bin/env bash
#
# Installs VitaSDK-softfp side by side with the regular (hardfp) VitaSDK.
#
# libjni_ff7.so is built with the softfp ABI, so the loader and every library
# linked to it (vitaGL, OpenSLES, libvpx...) must be softfp as well.
#
# Usage: scripts/setup-softfp-sdk.sh [install_dir]   (default: ~/vitasdk-softfp)
#
set -euo pipefail

INSTALL_DIR="${1:-$HOME/vitasdk-softfp}"
RELEASES="https://github.com/vitasdk-softfp/autobuilds/releases/download"
PKG_URL="https://github.com/vitasdk-softfp/packages/releases/download/master"

# Libraries needed by the loader (boilerplate + FF7-specific: libvpx for videos)
PACKAGES=(
  zlib minizip libpng
  libogg libvorbis flac opus mpg123 lame libsndfile
  opensles
  libvpx
  vitaShaRK SceShaccCgExt libmathneon
  kubridge taihen henkaku
)

case "$(uname -s)-$(uname -m)" in
  Darwin-*)
    # Last macOS build published by vitasdk-softfp (x86_64, runs under Rosetta on Apple Silicon)
    TOOLCHAIN="master-osx-v2.18/vitasdk-x86_64-apple-darwin-2024-04-25_22-01-08.tar.bz2"
    if [ "$(uname -m)" = "arm64" ] && ! arch -x86_64 /usr/bin/true 2>/dev/null; then
      echo "Rosetta 2 is required: softwareupdate --install-rosetta" >&2
      exit 1
    fi
    ;;
  Linux-x86_64)
    TOOLCHAIN="master-linux-v2.23/vitasdk-x86_64-linux-gnu-2026-03-29_13-36-57.tar.bz2"
    ;;
  *)
    echo "Unsupported host: $(uname -s)-$(uname -m)" >&2
    exit 1
    ;;
esac

if [ -x "$INSTALL_DIR/bin/arm-vita-eabi-gcc" ]; then
  echo "Toolchain already present in $INSTALL_DIR (delete it to reinstall)."
else
  echo "==> Downloading toolchain: $TOOLCHAIN"
  mkdir -p "$INSTALL_DIR"
  curl -fL --progress-bar "$RELEASES/$TOOLCHAIN" | tar xj -C "$INSTALL_DIR" --strip-components=1
fi

# The macOS build was linked against Intel Homebrew's zstd (/usr/local/opt/zstd).
# Build an x86_64 libzstd from source and relink the GCC binaries to it.
fix_macos_zstd() {
  local bad="/usr/local/opt/zstd/lib/libzstd.1.dylib"
  local lib="$INSTALL_DIR/lib/host/libzstd.1.dylib"
  local bins=(lib/gcc/arm-vita-eabi/10.3.0/cc1 lib/gcc/arm-vita-eabi/10.3.0/cc1plus
              lib/gcc/arm-vita-eabi/10.3.0/lto1 bin/arm-vita-eabi-lto-dump)

  otool -L "$INSTALL_DIR/${bins[0]}" | grep -q "$bad" || return 0
  [ -f "$bad" ] && return 0

  echo "==> Building x86_64 libzstd for the toolchain"
  local tmp; tmp="$(mktemp -d)"
  curl -fsL https://github.com/facebook/zstd/releases/download/v1.5.6/zstd-1.5.6.tar.gz | tar xz -C "$tmp"
  make -s -C "$tmp/zstd-1.5.6/lib" libzstd CC="clang -arch x86_64" -j8 >/dev/null
  mkdir -p "$(dirname "$lib")"
  cp "$tmp/zstd-1.5.6/lib/libzstd.1.5.6.dylib" "$lib"
  codesign -f -s - "$lib"
  rm -rf "$tmp"

  for b in "${bins[@]}"; do
    install_name_tool -change "$bad" "$lib" "$INSTALL_DIR/$b" 2>/dev/null
    codesign -f -s - "$INSTALL_DIR/$b"
  done
}
[ "$(uname -s)" = "Darwin" ] && fix_macos_zstd

PKG_LIST="$INSTALL_DIR/etc/vdpm/packages.list"
mkdir -p "$(dirname "$PKG_LIST")"
touch "$PKG_LIST"

for p in "${PACKAGES[@]}"; do
  if grep -q "^PACKAGE $p$" "$PKG_LIST"; then
    echo "==> $p already installed"
    continue
  fi
  echo "==> Installing package: $p"
  files="$(curl -fsL "$PKG_URL/$p.tar.xz" | tar -C "$INSTALL_DIR/arm-vita-eabi" -Jxvf - 2>&1)"
  printf 'PACKAGE %s\n%s\n\n' "$p" "$files" >> "$PKG_LIST"
done

# The macOS softfp build (2024) ships vita-headers too old for current vitaGL.
# Headers and syscall stubs don't depend on the float ABI, so take them from an
# up-to-date regular VitaSDK when available.
HARDFP_SDK="${VITASDK_HARDFP:-$HOME/vitasdk}"
sync_vita_headers() {
  local src="$HARDFP_SDK/arm-vita-eabi" dst="$INSTALL_DIR/arm-vita-eabi"
  local backup="$INSTALL_DIR/backup-vita-headers-2024"
  if [ ! -f "$src/include/psp2/gxm.h" ]; then
    echo "!! Regular VitaSDK not found at $HARDFP_SDK; vitaGL may fail to build." >&2
    echo "   Install it (https://vitasdk.org) or set VITASDK_HARDFP." >&2
    return 0
  fi
  grep -q SCE_GXM_INITIALIZE_FLAG_PB_LPDDR "$dst/include/psp2/gxm.h" && return 0

  echo "==> Updating vita-headers/stubs from $HARDFP_SDK"
  mkdir -p "$backup/include" "$backup/lib"
  cp -R "$dst"/include/{psp2,psp2common,psp2kern,vitasdk,vitasdk.h,vitasdkkern.h} "$backup/include/"
  cp "$dst"/lib/*_stub*.a "$backup/lib/"
  for d in psp2 psp2common psp2kern vitasdk; do
    rsync -a "$src/include/$d/" "$dst/include/$d/"
  done
  cp "$src"/include/vitasdk.h "$src"/include/vitasdkkern.h "$dst/include/"
  for f in "$src"/lib/*_stub*.a; do
    case "$(basename "$f")" in libkubridge*|libtaihen*) continue ;; esac
    cp "$f" "$dst/lib/"
  done
}

# The vitaShaRK package predates APIs used by current vitaGL: build it from source.
build_vitashark() {
  grep -q shark_set_shader_association_path "$INSTALL_DIR/arm-vita-eabi/include/vitashark.h" 2>/dev/null && return 0
  echo "==> Building vitaShaRK from source"
  local tmp; tmp="$(mktemp -d)"
  git clone -q --depth 1 https://github.com/Rinnegatamante/vitaShaRK "$tmp/vitaShaRK"
  VITASDK="$INSTALL_DIR" PATH="$INSTALL_DIR/bin:$PATH" make -s -C "$tmp/vitaShaRK" -j8 install
  rm -rf "$tmp"
}

if [ "$(uname -s)" = "Darwin" ]; then
  sync_vita_headers
  build_vitashark
fi

echo
echo "Done. Before building, run:"
echo "  source scripts/env.sh"
