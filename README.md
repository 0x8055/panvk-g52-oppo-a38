# PanVK on Mali-G52 MC2 — Oppo A38 (MT6768, modern kernel)

Investigation & testing notes: getting PanVK (Mesa's open-source Vulkan driver) working on a
**Mali-G52 MC2** via a **modern kernel** (6.6.118-android15, UAPI 11.46), built natively on-device in
Termux — no root, no PC, no NDK download.

This started as a replication of [LukeValen/panvk-mali-g52](https://github.com/LukeValen/panvk-mali-g52)
(the original G52 project, on a Redmi 13C running kernel 4.19.191 / UAPI 11.38) on a second device with
a much newer kernel. It found and fixed the real root cause of a blocker that affects **any** device
whose kernel/DDK differs from the one a given `stride`/offset value was hardcoded for, then went beyond
that into AHardwareBuffer/gralloc territory not yet documented by any of the related projects.

Full detail (English + Spanish), including the complete debugging methodology and dead ends: see
[`docs/`](docs/).

⚠️ Experimental. `panvk` itself prints `WARNING: panvk is not a conformant Vulkan implementation,
testing use only.` No claim of conformance or game compatibility is made here.

## Device

| | Redmi 13C (original project) | **Oppo A38 (this repo)** | Infinix X6853 (G57 reference) |
|---|---|---|---|
| SoC | MT6769V/CZ (mt6768) | MT6768 | Helio G99 |
| GPU | Mali-G52 MC2 | Mali-G52 MC2 | Mali-G57 MC2 (Valhall) |
| Kernel | 4.19.191 | 6.6.118-android15 | 6.12.38-android16 |
| Kbase UAPI | 11.38 | **11.46** | 11.46 |
| Root | No | No | No |

## Status

- ✅ Native Termux build of `funnymdzz/mesa` (kbase backend), Vulkan-only, no root.
- ✅ Device enumeration, real compute dispatch, real offscreen graphics draw (matches LukeValen's Phase
  4/7, once the stride fix below is applied).
- ✅ Android platform build (`-Dplatforms=android`) working, incl. a Termux/Bionic toolchain issue not
  documented elsewhere (see Finding 4).
- ✅ Real `AHardwareBuffer` import via `vkAllocateMemory`, and a real `VkImage` backed by that memory
  successfully cleared by the GPU and read back correctly.
- ⚠️ One fix (Finding 3) is **tentative** — flagged as likely wrong by a more advanced sibling project.
  See that section before relying on it.
- ❌ Not yet attempted: on-screen WSI (`ANativeWindow` swapchain), packaging as a real Android Vulkan
  HAL, Winlator integration.

## Findings in this repo

### 1. Root cause of `JOB_CONFIG_FAULT`: wrong `KBASE_IOCTL_JOB_SUBMIT` stride

The single biggest blocker. A hardcoded `stride = 64` (copied from a G57 project without checking it
against this device's real struct size) caused every job submission to be rejected by the kernel
*before* reaching hardware (`event_code=0x40`, `JOB_CONFIG_FAULT`). The real, official
`struct base_jd_atom_v2` size for UAPI 11.4x (confirmed against ARM's own GPLv2 kernel source) is **56
bytes**, not 64. With the correct stride, jobs reach real hardware
(`event_code=0x01`/`DONE`) — isolated and reproduced with `tests/kbase-jm-prototype/definitive_test.c`
and `definitive_test_stride64.c` (identical atom content, only the stride differs).

Also found along the way: a `core_req` value hardcoded as `0x04` in the original driver patch script
should have been `0x16` (`T|CS|V` combined) per that same project's own documentation — already
corrected in `patches/0001-fix-kbase-stride-and-core-req.patch`.

**Patch:** `patches/0001-fix-kbase-stride-and-core-req.patch` (touches
`src/panfrost/vulkan/jm/panvk_vX_gpu_queue.c`).

### 2. `panvk_android_import_ahb_memory()`: dma-buf fd not always at `data[0]`

LukeValen's own `android_wsi_ahb_fixes.patch` fixed this exact class of bug in
`vk_common_GetAndroidHardwareBufferPropertiesANDROID` (shared Mesa code), but left a note that the same
fix was still needed in `panvk_android_import_ahb_memory()` (PanVK-specific, the real `vkAllocateMemory`
import path). Applied the same fix here: probe each fd via `lseek(SEEK_END)` instead of assuming
`data[0]`. Confirmed on this device that `data[0]` is *not* the real dma-buf fd (it's at index 1) —
independent confirmation of the same real-hardware finding on a different device/gralloc fallback.

**Patch:** `patches/0002-fix-ahb-dma-buf-fd-index.patch` (touches `src/panfrost/vulkan/panvk_android.c`).

### 3. ⚠️ TENTATIVE: `panvk_image_get_explicit_mod()` crashes on `DRM_FORMAT_MOD_INVALID`

Without a real vendor gralloc HAL (`"No gralloc hwmodule detected"` — expected when testing via Termux
without a full vendor Android image), Mesa's fallback gralloc reports `DRM_FORMAT_MOD_INVALID` as the
buffer's tiling modifier. `panvk_image_get_explicit_mod()` had no handling for this — only a guarding
`assert()`, silently a no-op in release builds (`NDEBUG`) — so it crashed further downstream in
`panvk_image_init_layouts()`.

**The fix applied here (substituting `DRM_FORMAT_MOD_LINEAR`) is explicitly flagged as likely wrong** by
a more advanced sibling project
([wonderkast02/panvk-g720-kbase-csf](https://github.com/wonderkast02/panvk-g720-kbase-csf)), whose own
README warns: *"Never convert `DRM_FORMAT_MOD_INVALID` to `DRM_FORMAT_MOD_LINEAR` by assumption."* The
test that validated this fix (clear an image to a solid color, read it back through the same driver) is
too simple to catch a genuinely wrong layout — it's self-consistent even if wrong. **Treat this patch as
a starting point for further investigation, not a confirmed-correct fix.**

**Patch:** `patches/0003-fix-invalid-drm-modifier-TENTATIVE.patch` (touches
`src/panfrost/vulkan/panvk_image.c`).

### 4. Toolchain: Termux-built host tools fail Bionic's strict TLS alignment check

Building with `-Dplatforms=android` pulls in host-side codegen tools (`mesa_clc`, `vtn_bindgen2`,
`panfrost_compile`) that run *during* the build itself. On this toolchain, they linked with a `PT_TLS`
segment that Bionic's dynamic linker rejects at execution time
(`executable's TLS segment is underaligned... needs to be at least 64 for ARM64 Bionic`) — a known,
long-standing issue across the wider Termux ecosystem (also seen with unrelated packages: Valgrind, PHP,
Go), with no clean compiler-flag fix. Fixing only `p_align` is not enough — it must be fixed together
with the "skew" (`p_vaddr % 64`), or Bionic rejects the binary with a different error.

**Tool:** `tools/tls_align_fix2.py` — patches a built ELF binary's `PT_TLS` program header in place.
Usable for any Termux-built binary hitting this same error, not just this project.

Also needed: an explicit `-target aarch64-linux-androidNN` (not just `-D__ANDROID_API__=NN`) for Clang's
`__INTRODUCED_IN()` availability checks (e.g. `AHardwareBuffer_acquire`, API 26+) to resolve correctly —
the macro alone isn't what Clang's availability checker reads.

## Prerequisites (from LukeValen's repo — apply these first)

This repo only contains patches *on top of* LukeValen's own work, not a full rebuild from scratch.
Before applying anything here:

1. Follow [LukeValen/panvk-mali-g52](https://github.com/LukeValen/panvk-mali-g52) through Phase 6
   (`funnymdzz/mesa` clone, `patches/funnymdzz-mesa-termux-fixes.patch`,
   `panvk-driver-patch/apply_kbase_patch.py` + `fix_kbase_patch.py`, `gpu_variant_fix.patch`,
   `exec_init_order_fix.patch`).
2. For the Android platform build (Findings 2-4): also apply LukeValen's
   `panvk-driver-patch/android_wsi_ahb_fixes.patch`, install Termux's `ndk-sysroot` package, and
   reconfigure with:
   meson setup build-android -Dvulkan-drivers=panfrost -Dgallium-drivers= -Dplatforms=android 
-Dandroid-stub=true -Degl=disabled -Dbuildtype=release -Dbuild-tests=false 
-Dpanfrost-kmds=kbase,panthor -Dc_args="-D__ANDROID_API__=35 -target aarch64-linux-android35"
3. Apply this repo's `patches/` on top, then rebuild `src/panfrost/vulkan/libvulkan_panfrost.so`. If
`ninja` fails on a host tool with a TLS alignment error, run
`python3 tools/tls_align_fix2.py <path-to-the-failing-binary>` and retry — repeat for each tool ninja
reports (they get relinked, and need re-patching, whenever build flags change).

## Tests

- `tests/kbase-jm-prototype/` — raw `KBASE_IOCTL_JOB_SUBMIT` harnesses, independent of Mesa. Start with
`definitive_test.c` (uses ARM's official `struct base_jd_atom_v2`, stride computed by the compiler,
not hardcoded) to reproduce Finding 1 from scratch.
- `tests/panvk-driver-patch/` — real driver tests, via the actual built `libvulkan_panfrost.so`.
`panvk_ahb_test.c` exercises Finding 2; `panvk_ahb_image_test.c` exercises Finding 3 (real GPU
clear + readback through an AHB-backed image).

## Open questions / help wanted

- What's the *correct* modifier substitution for `DRM_FORMAT_MOD_INVALID` (Finding 3)? G720's README
flags the current fix as wrong but doesn't say what to use instead.
- On-screen WSI via `ANativeWindow` — untested here.
- Packaging as a real Android Vulkan HAL (`vulkan-android.sym` symbol scheme) instead of loading via
`dlopen()` with an absolute path — untested here.
- Anyone with a Mali-G52 (or other Bifrost/JM) device on a *different* kernel/UAPI willing to run
`tests/kbase-jm-prototype/definitive_test.c` and report the `stride`/`event_code` result?

## Credits / prior art

- [LukeValen/panvk-mali-g52](https://github.com/LukeValen/panvk-mali-g52) — the original G52 project
this repo is built directly on top of.
- [funnymdzz/mesa](https://github.com/funnymdzz/mesa) — the `kbase_kmod.c` backend (device discovery
without a DRM node) everything here depends on.
- [wonderkast02/panvk-g720-kbase-csf](https://github.com/wonderkast02/panvk-g720-kbase-csf) — CSF/G720
bring-up; flagged the modifier issue in Finding 3.
- [Noysz/panvk-g99-jm](https://github.com/Noysz/panvk-g99-jm) — G57/G99 (Valhall v9) JM bring-up;
pointed to ARM's official GPLv2 kernel source, used to confirm Finding 1.
- ARM Limited — `mali_kbase` kernel driver source (GPLv2), used to confirm the real
`struct base_jd_atom_v2` layout.
- Icecream95 and the Panfrost/PanVK contributors — the underlying driver this all sits on top of.

## License

This repository contains only patches, test harnesses, a standalone ELF-patching tool, and
documentation — no copy of Mesa itself. Mesa is MIT-licensed; see
https://gitlab.freedesktop.org/mesa/mesa. `funnymdzz/mesa`, the fork everything here builds on, is also
Mesa-derived (MIT).
