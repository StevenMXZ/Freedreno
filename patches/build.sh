#!/usr/bin/env bash
# Builds the freedreno gallium driver as a standalone xlib-GLX libGL for
# Winlator Bionic, aarch64-android, from an unpatched mesa checkout plus the
# patch series next to this script.
#
# The result is one file, usr/lib/libGL.so.1.5.0, packaged as
# opengl_freedreno.tzst.  There is no megadriver and no dri/ directory: with
# glx=xlib the driver is linked straight into libGL, so nothing is dlopened and
# MESA_LOADER_DRIVER_OVERRIDE plays no part.
#
# Runs on an x86_64 Linux host (ubuntu-24.04 on GitHub-hosted runners), and
# equally on a local Debian/Ubuntu box.  Mirrors the build in patches/README.md.
#
# Usage, from the mesa checkout root:
#   patches/build.sh deps    # host tools, needs sudo/apt (skip if you have them)
#   patches/build.sh build   # everything else, no root needed
#   patches/build.sh all     # both
#
# The two are split so CI can restore the NDK/sysroot cache in between.
set -euo pipefail

NDK_VERSION=${NDK_VERSION:-android-ndk-r27}
API_LEVEL=${API_LEVEL:-26}
ROOT=${ROOT:-$HOME/zinkbuild}
SYS=$ROOT/sysroot
TC=$ROOT/ndk/$NDK_VERSION/toolchains/llvm/prebuilt/linux-x86_64
SRCDIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

log() { printf '\n=== %s\n' "$*"; }

install_host_tools() {
  log "Installing host tools"
  sudo apt-get update
  sudo apt-get install -y --no-install-recommends \
    bison flex pkg-config patchelf zstd unzip curl binutils cmake python3-pip
  # Mesa 26.3 wants a newer meson than noble ships; take it from pip.
  pip3 install --break-system-packages --upgrade meson ninja mako pyyaml
  meson --version
  ninja --version
}

fetch_ndk() {
  log "Fetching the NDK"
  mkdir -p "$ROOT/dl" "$ROOT/ndk"
  if [ ! -d "$ROOT/ndk/$NDK_VERSION" ]; then
    curl -fL --retry 3 -o "$ROOT/dl/ndk.zip" \
      "https://dl.google.com/android/repository/$NDK_VERSION-linux.zip"
    unzip -q "$ROOT/dl/ndk.zip" -d "$ROOT/ndk"
  fi
  test -x "$TC/bin/clang"
}

build_sysroot() {
  log "Building the target sysroot from Termux packages"
  # libxcb carries libxcb-dri3/-present/-sync, which the direct presentation
  # path in xlib_sw_winsys.c links against; libx11 carries libX11-xcb, which is
  # how it reaches xcb from the Display.
  if [ ! -d "$SYS/lib" ]; then
    local DL=$ROOT/dl/termux STAGE
    mkdir -p "$DL"; cd "$DL"
    local BASE=https://packages.termux.dev/apt/termux-main
    curl -fsSL -o Packages "$BASE/dists/stable/main/binary-aarch64/Packages"
    for p in libx11 libxext libxcb libxau libxdmcp libdrm zlib zstd libxshmfence xorgproto; do
      local FN
      FN=$(awk -v pkg="$p" 'BEGIN{RS="";FS="\n"}{n="";f="";
          for(i=1;i<=NF;i++){if($i~/^Package: /){n=$i;sub(/^Package: /,"",n)}
                             if($i~/^Filename: /){f=$i;sub(/^Filename: /,"",f)}}
          if(n==pkg)print f}' Packages | head -1)
      test -n "$FN"
      curl -fsSL -o "$(basename "$FN")" "$BASE/$FN"
    done
    STAGE=$DL/stage; rm -rf "$STAGE"; mkdir -p "$STAGE"; cd "$STAGE"
    for deb in "$DL"/*.deb; do rm -f data.tar.*; ar x "$deb"; tar -xf data.tar.*; done
    mkdir -p "$SYS/lib" "$SYS/include"
    local TXP=data/data/com.termux/files/usr
    cp -a "$STAGE/$TXP/lib/."     "$SYS/lib/"
    cp -a "$STAGE/$TXP/include/." "$SYS/include/"
    # Termux .pc files hardcode the on-device prefix; repoint them at $SYS.
    sed -i "s|/data/data/com.termux/files/usr|$SYS|g" "$SYS"/lib/pkgconfig/*.pc
  fi
  # Fail early and loudly rather than at link time: these four are what the
  # DRI3 present path adds over the older MIT-SHM-only build.
  local l
  for l in libxcb-dri3.so libxcb-present.so libxcb-sync.so libX11-xcb.so; do
    test -e "$SYS/lib/$l" || { echo "sysroot is missing $l" >&2; exit 1; }
  done
}

build_sysvshm() {
  log "Building libandroid-sysvshm"
  # Winlator's shim (github.com/Pipetto-crypto/winlator, android_sysvshm/).
  # bionic declares but does not export shmget and friends, and the xlib winsys
  # still falls back to MIT-SHM whenever the server declines a buffer -- see
  # xlib_dri3_give_up().
  if [ ! -f "$SYS/lib/libandroid-sysvshm.so" ]; then
    local SRC=$ROOT/android_sysvshm
    mkdir -p "$SRC/sys"; cd "$SRC"
    local B=https://raw.githubusercontent.com/Pipetto-crypto/winlator/dev/android_sysvshm
    curl -fsSL -o android_sysvshm.c "$B/android_sysvshm.c"
    curl -fsSL -o sys/shm.h         "$B/sys/shm.h"
    "$TC/bin/aarch64-linux-android$API_LEVEL-clang" -Wall -std=gnu99 -shared -fPIC \
      -I"$SRC" -include string.h -Wl,-soname,libandroid-sysvshm.so \
      -o "$SYS/lib/libandroid-sysvshm.so" android_sysvshm.c
  fi
}

write_cross_file() {
  log "Writing the meson cross file"
  # No sys_root property: meson would then prefix our pkg-config -I/-L paths
  # with the NDK sysroot and break them.  The clang wrapper knows its own.
  cat > "$ROOT/cross-android-aarch64.ini" <<EOF
[binaries]
c           = '$TC/bin/aarch64-linux-android$API_LEVEL-clang'
cpp         = '$TC/bin/aarch64-linux-android$API_LEVEL-clang++'
ar          = '$TC/bin/llvm-ar'
strip       = '$TC/bin/llvm-strip'
ranlib      = '$TC/bin/llvm-ranlib'
objcopy     = '$TC/bin/llvm-objcopy'
pkg-config  = '$(command -v pkg-config)'
cmake       = '$(command -v cmake || echo /usr/bin/cmake)'

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
  cat "$ROOT/cross-android-aarch64.ini"
}

apply_patches() {
  log "Applying the patch series"
  cd "$SRCDIR"
  test -f patches/series || { echo "patches/series is missing from the checkout" >&2; exit 1; }
  local p
  while read -r p; do
    [ -n "$p" ] || continue
    test -f "patches/$p" || { echo "patches/$p listed in series but absent" >&2; exit 1; }
    if git apply --check "patches/$p" 2>/dev/null; then
      echo "applying $p"
      git apply "patches/$p"
    elif git apply --reverse --check "patches/$p" 2>/dev/null; then
      # Already in the tree: this branch may carry the series committed rather
      # than as files to apply.  Not an error.
      echo "skipping $p (already applied)"
    else
      echo "patches/$p does not apply to this checkout" >&2
      git apply --verbose "patches/$p" || true
      exit 1
    fi
  done < patches/series
  git --no-pager diff --stat | tail -1
}

configure() {
  log "Configuring"
  cd "$SRCDIR"
  # Flag notes:
  #  - freedreno is the only gallium driver, so GALLIUM_DRIVER=freedreno is
  #    mandatory at runtime: drivers[] in sw_screen_create_named() collapses to
  #    whatever that names, and freedreno is never an implicit fallback.
  #  - xlib-lease must be disabled explicitly: freedreno-kmds=kgsl selects
  #    with_dri_platform='pseudo-drm', which auto-enables it, and it then wants
  #    libxrandr purely for VK_EXT_acquire_xlib_display.
  #  - glx=xlib means no DRI loader: the driver is linked into libGL and the
  #    winsys talks DRI3 to the X server itself.
  meson setup build \
    --cross-file "$ROOT/cross-android-aarch64.ini" \
    --buildtype release -Db_ndebug=true \
    -Dgallium-drivers=freedreno \
    -Dvulkan-drivers= \
    -Dfreedreno-kmds=kgsl \
    -Dglx=xlib \
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

compile() {
  log "Compiling"
  ninja -C "$SRCDIR/build" -j"$(nproc)"
}

package() {
  log "Packaging"
  cd "$SRCDIR"
  mkdir -p out/usr/lib

  # meson names it libGL.so and bakes a build-tree rpath; the container wants
  # soname libGL.so.1 and no RUNPATH.  The file keeps the .so.1.5.0 name the
  # existing Winlator artifacts use.
  cp build/src/gallium/targets/libgl-xlib/libGL.so out/usr/lib/libGL.so.1.5.0
  "$TC/bin/llvm-strip" --strip-all out/usr/lib/libGL.so.1.5.0
  patchelf --set-soname libGL.so.1 out/usr/lib/libGL.so.1.5.0
  patchelf --remove-rpath          out/usr/lib/libGL.so.1.5.0

  tar --zstd -cf opengl_freedreno.tzst --owner=0 --group=0 --mode=0755 -C out usr
  tar --zstd -tvf opengl_freedreno.tzst
}

verify() {
  log "Verifying the artifact"
  cd "$SRCDIR"
  local RE=$TC/bin/llvm-readelf
  local NM=$TC/bin/llvm-nm
  local GL=out/usr/lib/libGL.so.1.5.0
  local BUILT=build/src/gallium/targets/libgl-xlib/libGL.so
  local fail=0
  check() { if [ "$2" = ok ]; then echo "  ok    $1"; else echo "  FAIL  $1"; fail=1; fi; }

  echo "DT_NEEDED:"; "$RE" -d "$GL" | grep NEEDED || true
  echo "checks:"

  [ -n "$("$RE" -d "$GL" | grep 'SONAME.*libGL\.so\.1\]')" ] && check "soname is libGL.so.1" ok || check "soname is libGL.so.1" no
  [ -z "$("$RE" -d "$GL" | grep -E 'RUNPATH|RPATH')" ] && check "no RUNPATH/RPATH" ok || check "no RUNPATH/RPATH" no
  [ "$(tar --zstd -tf opengl_freedreno.tzst | grep -c '^usr/lib/libGL\.so\.1\.5\.0$')" = 1 ] && check "tzst carries usr/lib/libGL.so.1.5.0" ok || check "tzst carries usr/lib/libGL.so.1.5.0" no

  # The DRI3 present path is what this build exists for; without these four the
  # library silently falls back to MIT-SHM everywhere.
  local l
  for l in libxcb-dri3.so libxcb-present.so libxcb-sync.so libX11-xcb.so; do
    [ -n "$("$RE" -d "$GL" | grep "NEEDED.*\[$l\]")" ] && check "links $l" ok || check "links $l" no
  done

  # Symbols have to be read from the build tree: the packaged copy is
  # --strip-all'd, so every lookup there would come back empty and "pass"
  # nothing.  Strings survive stripping, so those read the artifact itself.
  local SYMS sym
  SYMS=$("$NM" --defined-only "$BUILT")
  for sym in kgsl_device_new kgsl_pipe_new kgsl_bo_new kgsl_bo_from_dmabuf kgsl_bo_new_shared_image; do
    case $SYMS in
      *" $sym"$'\n'*) check "kgsl backend has $sym" ok ;;
      *) check "kgsl backend has $sym" no ;;
    esac
  done

  # grep must consume all of stdin here: -q and -m1 both close the pipe early,
  # strings takes SIGPIPE, and pipefail then reports the whole pipeline as
  # failed even though the string was found.
  local STR
  STR=$(strings "$GL")
  has() { [ "$(printf '%s\n' "$STR" | grep -cx -- "$1")" -gt 0 ] && echo ok || echo no; }
  check "opens /dev/kgsl-3d0"             "$(has '/dev/kgsl-3d0')"
  check "gralloc path present"            "$(has 'AHardwareBuffer_allocate')"
  check "Winlator HUD properties present" "$(has '_MESA_DRV')"

  for sym in glGetString glXMakeCurrent glXGetProcAddressARB glXSwapBuffers; do
    [ -n "$("$RE" --dyn-syms "$GL" | grep " $sym\$")" ] && check "exports $sym" ok || check "exports $sym" no
  done

  return $fail
}

do_build() {
  fetch_ndk
  build_sysroot
  build_sysvshm
  write_cross_file
  apply_patches
  configure
  compile
  package
  verify
}

case "${1:-all}" in
  deps)  install_host_tools ;;
  build) do_build ;;
  all)   install_host_tools; do_build ;;
  *) echo "usage: $0 {deps|build|all}" >&2; exit 2 ;;
esac
