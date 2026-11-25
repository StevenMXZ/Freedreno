# Freedreno over KGSL for GLX Xlib (Winlator/Bionic) — minimal patch set

12 patches grouped by directory. This is a reduced version of the
`freedreno-dri3-present` branch: of its 31 original commits, only the changes
needed to **build and run Freedreno with the Xlib target** were retained.
The result is the `freedreno-xlib-minimal` branch (a single commit based on
merge base `780727e68ad`).

All patches were checked with `git apply --check` against the HEAD of this
tree, individually and together, without errors.

## Apply

```bash
while read p; do git apply "patches/$p"; done < patches/series
```

## Build

The complete build process (NDK, Termux sysroot, `libandroid-sysvshm`, cross
file, patches, compilation, packaging and verification) is implemented in
`patches/build.sh`, the same script used by the GitHub Actions workflow:

```bash
patches/build.sh all
```

The output is `opengl_freedreno.tzst` at the root of the tree.
`patches/build.sh deps` installs host tools (requires apt/sudo), while
`patches/build.sh build` handles the remaining steps without root.

Equivalent manual configuration:

```bash
meson setup build --cross-file cross-android-aarch64.ini --buildtype release -Db_ndebug=true -Dgallium-drivers=freedreno -Dvulkan-drivers= -Dfreedreno-kmds=kgsl -Dglx=xlib -Dplatforms=x11 -Dxlib-lease=disabled -Degl=disabled -Dgbm=disabled -Dgles1=disabled -Dgles2=disabled -Dopengl=true -Dglvnd=disabled -Dllvm=disabled -Dxmlconfig=disabled -Dzstd=enabled -Dlibunwind=disabled -Dvalgrind=disabled -Dbuild-tests=false -Dandroid-libbacktrace=disabled -Dvideo-codecs=
```

The output library is `src/gallium/targets/libgl-xlib/libGL.so`.
Rename it to `libGL.so.1`, set the SONAME with patchelf and remove the RPATH.
Build from a native WSL clone, not from `/mnt/c` (CRLF breaks Python shebangs).

## Patches

| # | Patch | Description |
|---|-------|-------------|
| 01 | `meson.build` | Enables `glx=xlib` with Freedreno (previously required swrast); detects `sync_wait`; adds `dep_x11_xcb` |
| 02 | `src_util` | Provides a `sync_wait()` fallback using `poll()` when unavailable in the NDK; links `liblog` on Android regardless of `-Dplatforms` |
| 03 | `src_freedreno_drm` | **KGSL backend** (`kgsl/*`), per-backend feature bits, direct reset status, opens `/dev/kgsl-3d0` when the supplied fd is not KGSL, allocates shareable images through gralloc, and reports probe failures |
| 04 | `src_freedreno_fdl` | Honors `-Dbuild-tests=false` to prevent build failures |
| 05 | `src_freedreno_ir3` | Avoids dereferencing a NULL build ID in the disk cache |
| 06 | `src_gallium_auxiliary_target-helpers` | Allows `sw_screen_create_named()` to accept `"freedreno"` |
| 07 | `src_gallium_drivers_freedreno` | `fd_screen_create_sw()`, scanout resources through the shared image allocator, DRI3 presentation and software fallback |
| 08 | `src_gallium_frontends_glx_xlib` | Avoids CPU/GPU serialization on every swap; two-back-buffer ring in `xm_st` |
| 09 | `src_gallium_targets_libgl-xlib` | Links `driver_freedreno` and `idep_xmlconfig`; Bionic flags (`-landroid-sysvshm`, `-lc++_shared`, `-llog`, `-lsync`, `--undefined-version`) |
| 10 | `src_gallium_winsys_sw_xlib` | Direct DRI3/Present presentation in the Xlib winsys |
| 11 | `src_mesa_main` | Stops `check_compatible()` from comparing depth and stencil when binding a drawbuffer |
| 12 | `src_gallium_frontends_glx_xlib-hud` | Publishes `_MESA_DRV`, `_MESA_DRV_ENGINE_NAME` and `_MESA_DRV_GPU_NAME` on the drawable in `glXMakeCurrent`, enabling the Winlator X server HUD. Cosmetic; can be omitted without affecting the rest |

## What was removed and why

19 commits were removed from the original branch:

- **Zink** (`zink_screen.c`, `zink_resource.c`, `zink_types.h` and enabling Zink for the Xlib target): a separate backend not required by Freedreno. References to Zink in patches 06 and 09 were rewritten for Freedreno.
- **`dri` target** (`targets/dri/meson.build`, `dri.sym.in`, `dri_sw_winsys.c`): this build uses `glx=xlib`, so the DRI target is not built.
- **DRI HUD path** (`src/glx/glxcurrent.c`): the DRI frontend allocates a GLXWindow distinct from the Window and must resolve the drawable before setting the property. This file is not compiled with `glx=xlib`; patch 12 covers the relevant Xlib path.
- **`dri_context.c`** (propagating make_current failures): diagnostic change in the DRI frontend, outside this target.
- **CI and documentation** (`.github/workflows/*`, `reproduce-*.sh`, `STATUS.md`): not part of the build. The configuration above came from `reproduce-freedreno-xlib.sh`.
- A commit and its revert (`freedreno: wait for the frame…`) that canceled each other out.

If HUD support is needed for the DRI path, refer to commit `5c7117e` from the
`freedreno-dri3-present` branch, specifically the changes in
`src/glx/glxcurrent.c`.

## Credits

- [Innersargento](https://github.com/Innersargento?tab=repositories) — original Freedreno DRI3/Present and Winlator/Bionic work on which this patch set is based.
- Vauzi — Adreno A710/A720/A722 support used in the earlier Freedreno branch.
- [The412Banner](https://github.com/The412Banner) — KGSL zero-timeout polling fix.
- Lucas Fryzek — original Freedreno KGSL backend series.
- Mesa contributors — upstream graphics driver development.

