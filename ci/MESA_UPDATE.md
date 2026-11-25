# Mesa 27 KGSL/Winlator port

Upstream snapshot: `2648b2059f1a412d64a8700dbbb75bbb4a768b0e`
(Mesa main, 2026-11-07, `27.0.0-devel`). This is a development snapshot.

Previous runtime: `6b0c6dd274e62df6ca463db0330efa0e48065ac6`, branch
`freedreno-dri-cmod-rgba-zero-copy`. Its upstream tree was from August 7.
The upstream August tree `525aac88c53be55f5958122e590fea2edcdc05d1`
is identical to the original base tree `3be29fcdf839e7f7b8bac7287c971163b000a1e2`;
the port reapplies the downstream commits onto the November snapshot.
The previous branch is retained for comparison and rollback.

## Preserved Android behavior

- KGSL BO allocation/import, timestamp fences, command submission and direct
  reset status; no DRM ioctls are substituted for KGSL ioctls.
- AHardwareBuffer allocation/export through the Winlator handle-socket transport.
- RGBA shared images and the conversion mirror for differing channel orders.
- DRI3/Present support without X SyncFence requests unsupported by Winlator.
- Mesa renderer/GPU properties for the HUD, Android seccomp handling and the
  fallback shader-cache key when a build-id is unavailable.

## Adaptations and upstream improvements

- IR3's disk cache is now C++; the Android fallback identifier uses the new
  language/API types, while retaining the upstream BLAKE3 cache scheme.
- The AHB resource path coexists with upstream multi-planar YUV allocation.
  Failure cleanup drops the plane chain and the optional shared mirror.
- Upstream A710/A720/A722 entries replace the old custom table. In particular,
  retain the upstream 710/720 bank bit 15 and corrected CCU counts instead of
  reapplying the old bit-16 override. Do not duplicate GPU IDs.
- Retain upstream `1f9c4d529`: requesting an out-fence alone is not evidence that
  a client supplies explicit synchronization. Do not set no_implicit_sync merely
  to recover submit merging on shared Android images.
- Retain upstream `05a33854`: imported syncobj FD double-close fix, together
  with the newer NIR/IR3 and A6xx/A7xx/A8xx fixes in the selected snapshot.
- Port the expired-deadline polling behavior of Turnip `e984ef29` to the
  Gallium KGSL wait path. Preserve ioctl errors, handle timestamp wraparound,
  and clamp very long finite waits instead of overflowing into an infinite wait.
- Adapt upstream `e62989401` stale IdleNotify protection to Present serials
  because Winlator has no DRI3 SyncFence XIDs. Match pixmap, 32-bit serial and
  the absent idle fence; preserve serial wraparound semantics.

MSM shared helpers introduced upstream are DRM-specific. They are not a drop-in
replacement for this backend's KGSL import/allocation/submission operations.

## Validation

Cross build configuration: NDK r27, aarch64-android API 26, Gallium Freedreno,
KGSL, GLX DRI, X11; EGL/GLES/Vulkan disabled, matching the previous DRI3 build.

Host regression test (no GPU required):

```sh
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  ci/test_kgsl_wait.c -o /tmp/test-kgsl-wait
/tmp/test-kgsl-wait
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  ci/test_dri3_idle.c -o /tmp/test-dri3-idle
/tmp/test-dri3-idle
```

It executes the production wait implementation with mocked time/ioctl inputs:
zero/sub-ms polls, success/timeout, timestamp wraparound, EINTR/EAGAIN expiry,
infinite/long finite deadlines and underlying ioctl failures.

Before treating this as device-validated, compare the previous and new ZIPs on
A619 and A830: GLX context creation, Mewgenics, the DX11 WineD3D cube, swap
interval 0/1, Java/X++ presentation, red/blue channel order and alpha, buffer
readback/reallocation, minimize/restore and repeated game launches. Record
FPS/frametimes under the same scene, resolution, settings and thermal state.
The build and host tests cannot establish GPU correctness or FPS improvements.

## Experimental Adreno 825 and 850

- Adreno 825: [Artem Peshkov's Mesa MR 46716](https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/46716) (patch 55d9da2239722cd2031965b7bb69ef85dde9b59d), replacing the earlier community profile. Backport the medium VPC cache group from [Rob Clark's MR 46240](https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/46240) with [Peshkov's MR 46551 correction](https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/46551). This older snapshot does not have the device-info `gmem_size` field; retain the kernel-reported GMEM size (2432 KiB on A825) instead. The original A825 profile by whitebelyash is superseded.
- Adreno 829: retain the separate [DVD cache configuration](https://github.com/whitebelyash/mesa-unified/commit/1ebd39a498786be2c3acfc7729c2c5bf0c21d7c8) and [VPC changes](https://github.com/whitebelyash/mesa-unified/commit/2a0281b44ed59d9f2d1fb6bb58ab830be3f7145b) from the previous port; these are distinct from MR 46716.
- Adreno 850: [lfdevs' experimental profile and KGSL UBWC 7.0 support](https://github.com/lfdevs/mesa-for-android-container/pull/104), based on [Qualcomm kernel support](https://patchwork.freedesktop.org/series/174781/). The CP, VSC, SP and CX_MISC register definitions are already present in this Mesa snapshot and are not duplicated.
- Gallium KGSL reads GMEM size from the kernel rather than hard-coding a device-specific size. It does not have Turnip's UBWC-version rejection switch; the UBWC 7.0 case is added to Turnip's KGSL configuration, reusing the existing eight-channel mode. The Winlator DRI3 build remains OpenGL-only.
- Global A8xx settings (including double threadsize and 32 KiB compute shared memory) remain unchanged. The experimental A829 TP UBWC flag hint is not enabled by this port.

These profiles are experimental; this downstream port has not been validated on an A825 or A850 device. A850-specific GMU programming and MALU support are outside this patch.
