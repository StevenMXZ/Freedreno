#!/usr/bin/env bash
set -euo pipefail

SRCDIR=${1:?usage: build-freedreno-dri3-winlator.sh /path/to/mesa}
NDK_VERSION=${NDK_VERSION:-android-ndk-r27}
API_LEVEL=${API_LEVEL:-26}
ROOT=${ROOT:-$HOME/mesa-android}
SYS=$ROOT/sysroot
TC=$ROOT/ndk/$NDK_VERSION/toolchains/llvm/prebuilt/linux-x86_64
DL=$ROOT/dl/termux
BASE=https://packages.termux.dev/apt/termux-main

log() { printf '\n=== %s\n' "$*"; }

fetch_ndk() {
  log "Fetching Android NDK"
  mkdir -p "$ROOT/dl" "$ROOT/ndk"
  if [ ! -x "$TC/bin/clang" ]; then
    curl -fL --retry 3 -o "$ROOT/dl/ndk.zip" \
      "https://dl.google.com/android/repository/$NDK_VERSION-linux.zip"
    unzip -q "$ROOT/dl/ndk.zip" -d "$ROOT/ndk"
  fi
  test -x "$TC/bin/aarch64-linux-android$API_LEVEL-clang"
}

build_sysroot() {
  log "Preparing Termux X11 sysroot"
  mkdir -p "$DL" "$SYS/lib" "$SYS/include"
  curl -fsSL -o "$DL/Packages" \
    "$BASE/dists/stable/main/binary-aarch64/Packages"

  local packages=(
    libx11 libxext libxxf86vm libxcb libxau libxdmcp libdrm
    zlib zstd libxshmfence xorgproto
  )

  local p fn deb
  for p in "${packages[@]}"; do
    fn=$(awk -v pkg="$p" 'BEGIN{RS="";FS="\n"}{n="";f="";
      for(i=1;i<=NF;i++){
        if($i~/^Package: /){n=$i;sub(/^Package: /,"",n)}
        if($i~/^Filename: /){f=$i;sub(/^Filename: /,"",f)}
      }
      if(n==pkg)print f}' "$DL/Packages" | head -1)
    test -n "$fn" || { echo "Termux package not found: $p" >&2; exit 1; }
    deb="$DL/$(basename "$fn")"
    [ -f "$deb" ] || curl -fsSL -o "$deb" "$BASE/$fn"
  done

  rm -rf "$DL/stage"
  mkdir -p "$DL/stage"
  for deb in "$DL"/*.deb; do
    dpkg-deb -x "$deb" "$DL/stage"
  done

  local TXP="$DL/stage/data/data/com.termux/files/usr"
  cp -a "$TXP/lib/." "$SYS/lib/"
  cp -a "$TXP/include/." "$SYS/include/"

  if compgen -G "$SYS/lib/pkgconfig/*.pc" > /dev/null; then
    sed -i "s|/data/data/com.termux/files/usr|$SYS|g" "$SYS"/lib/pkgconfig/*.pc
  fi

  local lib
  for lib in libX11.so libX11-xcb.so libxcb.so libxcb-dri3.so \
             libxcb-present.so libdrm.so libxshmfence.so; do
    test -e "$SYS/lib/$lib" || { echo "sysroot is missing $lib" >&2; exit 1; }
  done
}

write_cross_file() {
  log "Writing Meson Android cross file"
  cat > "$ROOT/cross-android-aarch64.ini" <<EOF
[binaries]
c           = '$TC/bin/aarch64-linux-android$API_LEVEL-clang'
cpp         = '$TC/bin/aarch64-linux-android$API_LEVEL-clang++'
ar          = '$TC/bin/llvm-ar'
strip       = '$TC/bin/llvm-strip'
ranlib      = '$TC/bin/llvm-ranlib'
objcopy     = '$TC/bin/llvm-objcopy'
pkg-config  = '$(command -v pkg-config)'
cmake       = '$(command -v cmake)'

[built-in options]
c_args        = ['-I$SYS/include']
cpp_args      = ['-I$SYS/include']
c_link_args   = ['-L$SYS/lib']
cpp_link_args = ['-L$SYS/lib']

[properties]
needs_exe_wrapper = true
pkg_config_libdir = '$SYS/lib/pkgconfig'

[host_machine]
system     = 'android'
cpu_family = 'aarch64'
cpu        = 'aarch64'
endian     = 'little'
EOF
}

configure() {
  log "Configuring Mesa: Freedreno KGSL + GLX DRI"
  cd "$SRCDIR"
  rm -rf build out

  meson setup build \
    --cross-file "$ROOT/cross-android-aarch64.ini" \
    --prefix=/usr --libdir=lib \
    --buildtype=release -Db_ndebug=true \
    -Dgallium-drivers=freedreno \
    -Dvulkan-drivers= \
    -Dfreedreno-kmds=kgsl \
    -Dglx=dri \
    -Dplatforms=x11 \
    -Dxlib-lease=disabled \
    -Degl=disabled -Dgbm=disabled \
    -Dgles1=disabled -Dgles2=disabled -Dopengl=true \
    -Dglvnd=disabled -Dllvm=disabled \
    -Dxmlconfig=disabled -Dzstd=enabled \
    -Dlibunwind=disabled -Dvalgrind=disabled \
    -Dbuild-tests=false -Dandroid-libbacktrace=disabled \
    -Dvideo-codecs=
}

compile_install() {
  log "Building"
  cd "$SRCDIR"
  ninja -C build -j"$(nproc)"

  log "Installing to staging directory"
  DESTDIR="$SRCDIR/out" meson install -C build --no-rebuild
}

verify_package() {
  log "Verifying DRI3 zero-copy test build"
  cd "$SRCDIR"

  local RE=$TC/bin/llvm-readelf
  local GL GALLIUM sha
  GL=$(find out/usr -type f -name 'libGL.so*' | head -1)
  GALLIUM=$(find out/usr -type f \( -name 'libgallium*.so' -o -name 'gallium_dri.so' \) | head -1)

  test -n "$GL" || { echo "libGL was not installed" >&2; exit 1; }
  test -n "$GALLIUM" || { echo "Gallium DRI library was not installed" >&2; exit 1; }

  echo "libGL: $GL"
  "$RE" -d "$GL" | grep NEEDED || true
  echo "Gallium: $GALLIUM"
  "$RE" -d "$GALLIUM" | grep NEEDED || true

  # Refuse to publish an artifact if the selected ref does not contain both
  # performance experiments being tested.
  grep -q 'DRM_FORMAT_XRGB8888' src/loader/loader_dri_helper.c
  grep -q 'PIPE_FORMAT_R8G8B8X8_UNORM' src/loader/loader_dri_helper.c
  grep -q 'bool fence_await = false;' src/gallium/frontends/dri/loader_dri3_helper.c

  echo "Installed libraries:"
  find out/usr \( -type f -o -type l \) -print | sort

  sha=$(git rev-parse --short HEAD)
  tar --zstd -cf "opengl_freedreno-dri3-${sha}.tzst" \
    --owner=0 --group=0 -C out usr
  echo "$sha" > .freedreno-dri3-build-sha
}

fetch_ndk
build_sysroot
write_cross_file
configure
compile_install
verify_package
