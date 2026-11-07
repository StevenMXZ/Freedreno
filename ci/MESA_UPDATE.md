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
