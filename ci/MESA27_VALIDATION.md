# Updated DRI3 source

The default source is `freedreno-dri3-mesa-27`, based on upstream Mesa main
`2648b2059f1a412d64a8700dbbb75bbb4a768b0e` (2026-11-07, 27.0.0-devel).
The complete port notes and host regression tests live in that branch's `ci/`.
Use `freedreno-dri-cmod-rgba-zero-copy` in the workflow's source_ref field to
build the previous runtime for comparison. Its history has not been rewritten.

Source commit `259b78ca0e` was cross-compiled and installed successfully using
NDK r27 / aarch64-android API 26 and this helper's Meson options. Both libGL
and libgallium were linked. Their DT_NEEDED lists add no runtime dependencies
relative to the previously bundled DRI3 driver.

The three-file ZIP was generated and passed the actual Ludashi OpenGL import
manager in a host harness with real ARM64 libraries: package classification,
ELF pairing, alias creation, persistent selection, and rejection of duplicate
or missing versions. KGSL deadline and DRI3 IdleNotify host tests passed under
AddressSanitizer/UndefinedBehaviorSanitizer.

These checks do not execute GPU commands. A619/A830 correctness and game
performance still need device comparison. Development snapshots are published
as prereleases automatically; this is not a device-validated stable release.
