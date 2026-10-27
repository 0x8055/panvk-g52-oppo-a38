# PanVK on Mali-G52 MC2 — Oppo A38 (MT6768, modern kernel)

Investigation & testing notes: getting PanVK (Mesa's open-source Vulkan driver) working on a
**Mali-G52 MC2** via a **modern kernel** (6.6.118-android15, UAPI 11.46), built natively on-device in
Termux — no root, no PC, no NDK download.

This started as a replication of [LukeValen/panvk-mali-g52](https://github.com/LukeValen/panvk-mali-g52)
(the original G52 project, on a Redmi 13C running kernel 4.19.191 / UAPI 11.38) on a second device with
a much newer kernel. It found and fixed the real root cause of a blocker that affects **any** device
whose kernel/DDK differs from the one a given `stride`/offset value was hardcoded for, then went on to measure how MediaTek's ARM gralloc lays out
`AHardwareBuffer`s on this device (Finding 3), building on the gralloc analysis in JimVulkan's Mesa fork.

Full detail (English + Spanish), including the complete debugging methodology and dead ends: see
[`docs/`](docs/).

⚠️ Experimental. `panvk` itself prints `WARNING: panvk is not a conformant Vulkan implementation,
testing use only.` No claim of conformance or game compatibility is made here.

**Note on authorship:** this research is AI-assisted. Every measurement was run on the author's device, and the community projects named below are key references. Read the claims as reports and verify them before relying on them.

**Reproducing note:** `-Dbuildtype=release` defines `NDEBUG`, which turns every `assert()` in the patched driver code into a no-op. In our runs `vkQueueSubmit` and `vkQueueWaitIdle` reported success even when a job had faulted, until the real `event_code` was printed. Check the event code, not only the Vulkan result.

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
- ✅ Finding 3: on the A38 the buffer layout is now read from the gralloc handle (measured, patch 0004) instead of assumed; compressed GPU-only buffers are refused with an explicit error.
- ⚠️ Finding 3 still contains one assumption: an explicit `DRM_FORMAT_MOD_INVALID` supplied by a caller or wrapper is still mapped to LINEAR (patch 0003). See that section.
- ❌ Not yet attempted with this build: on-screen WSI (`ANativeWindow` swapchain), packaging as a real
  Android Vulkan HAL, loading it in Winlator. Other projects' drivers tested on this phone are summarised in
  [Field notes](#field-notes-third-party-drivers-on-the-a38).

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

**Caveat:** patch 0001 hardcodes `56`, which is right for UAPI 11.46 (this device) but not for every kernel: the Redmi 13C (UAPI 11.38) uses 48. A runtime probe is the portable approach, and FristOneRR's driver implements one (see Field notes).

### 2. `panvk_android_import_ahb_memory()`: dma-buf fd not always at `data[0]`

LukeValen's own `android_wsi_ahb_fixes.patch` fixed this exact class of bug in
`vk_common_GetAndroidHardwareBufferPropertiesANDROID` (shared Mesa code), but left a note that the same
fix was still needed in `panvk_android_import_ahb_memory()` (PanVK-specific, the real `vkAllocateMemory`
import path). Applied the same fix here: probe each fd via `lseek(SEEK_END)` instead of assuming
`data[0]`. Confirmed on this device that `data[0]` is *not* the real dma-buf fd (it's at index 1) —
independent confirmation of the same real-hardware finding on a different device/gralloc fallback.

**Patch:** `patches/0002-fix-ahb-dma-buf-fd-index.patch` (touches `src/panfrost/vulkan/panvk_android.c`).

### 3. `DRM_FORMAT_MOD_INVALID` from the gralloc fallback: measured on the A38, one assumption left

Without a real vendor gralloc HAL (`"No gralloc hwmodule detected"`, expected when testing from Termux), Mesa's fallback gralloc always reports `DRM_FORMAT_MOD_INVALID` as the buffer's modifier, whatever the buffer really is. `panvk_image_get_explicit_mod()` had no handling for it, only an `assert()` that is a no-op in release builds (`NDEBUG`), so it crashed further down in `panvk_image_init_layouts()`.

**First attempt (patch 0003, kept).** Substitute `DRM_FORMAT_MOD_LINEAR`. A sibling project ([wonderkast02/panvk-g720-kbase-csf](https://github.com/wonderkast02/panvk-g720-kbase-csf)) warns against converting `INVALID` to `LINEAR` by assumption, and rightly: our test (clear an image, read it back through the same driver) cannot tell a wrong layout from a right one.

**What was measured (patch 0004).** On the A38, MediaTek's ARM gralloc handle has 3 fds and 62 ints. The magic `0x3141592` is at `ints[18]`, a u64 *alloc format* at magic+11 (`ints[29..30]`: low word = HAL format, high word = layout bits) and the plane-0 `{byte stride, width, height}` at magic+16 (`ints[34..36]`). Allocating the same 200x100 buffer with different usages (`tests/panvk-driver-patch/ahb_probe2.c`, RGBA8 and RGBX8 gave identical results):

| usage | alloc format (high word) | buffer size | layout |
|---|---|---|---|
| `GPU_SAMPLED` only | `0` | 86016 | linear |
| `SAMPLED + COLOR` with CPU read, CPU write or `COMPOSER_OVERLAY` | `0` | 86016 | linear |
| `COLOR` only, `SAMPLED + COLOR`, `SAMPLED + COLOR + bit 29` | `0xc410000` | 135168 | compressed |
| `SAMPLED + COLOR + bit 57` | `0` | 86016 | linear |
| `SAMPLED + COLOR + bit 57 + bit 60` | `0xc410000` | 135168 | compressed |

- The high word separates the two cases in 16 of 16 samples.
- The sizes match: linear is 208 px x 4 = 832 bytes of stride, 83200 bytes, rounded up to 21 pages. The other case is 256x128 (128-aligned) x 4 = 131072 bytes of body plus a 4096-byte header. That is consistent with AFBC (16x16 superblocks, tiled headers), but the actual modifier flags were **not** verified.
- The old-style private `NO_AFBC` usage bit (bit 29) has no effect on this allocator; CPU or composer usage keeps a buffer linear, as in [JimVulkan](https://github.com/JimVulkan/mesa-unified-drivers)'s notes on MediaTek's allocator on a Galaxy A31. The device's own properties name another bit: `ro.vendor.arm.gralloc.no_afbc_usage_flags` is `0x0200000000000000` with mask `0x1200000000000000`. Requesting bit 57 (`AHARDWAREBUFFER_USAGE_VENDOR_13`) gives a linear GPU-only buffer (86016 bytes, alloc format high word `0`), and adding bit 60 brings the compressed layout back, which fits a rule of the form `(usage & mask) == flags`. Measured on RGBA8 200x100 only.

**Patch 0004** (`u_gralloc_fallback.c`) only acts when the handle matches that fingerprint. High word `0` plus a matching plane stride gives `DRM_FORMAT_MOD_LINEAR`, measured and no longer assumed. A non-zero high word is refused with an explicit error (`vkAllocateMemory` returns `VK_ERROR_INVALID_EXTERNAL_HANDLE`) instead of being silently misread. Any other handle behaves exactly as before.

**Verified on the device:** `panvk_ahb_image_test` (CPU and composer usage) now reports `info.modifier=0x0`, no `INVALID` message, and the expected blue pixel. `panvk_ahb_image_test_gpuonly` (GPU-only usage) fails with the explicit "compressed buffer ... is not handled" error.

**What is still an assumption:**
- Patch 0003 is still needed when the *caller* passes an explicit `DRM_FORMAT_MOD_INVALID`, as Winlator's wrapper does according to [LukeValen's README](https://github.com/LukeValen/panvk-mali-g52). In that path there is no handle to inspect, `LINEAR` is unverified, and it would be wrong for a GPU-only buffer.
- The fingerprint is only confirmed on the A38 (the dump logged by a build of JimVulkan's `mesa-unified-drivers` on this same phone has the same shape).
- The "linear" evidence began as the measured correlation, with the pixel readback going through the same driver. It was then checked independently with `AHardwareBuffer_lock()` (`tests/panvk-driver-patch/ahb_layout_check.c`): on a 200x100 buffer with CPU and composer usage (row stride 208 pixels), a per-pixel pattern written by the CPU is read correctly by the GPU, and patterns and clears written by the GPU are read correctly by the GPU and by the CPU, with 0 of 20000 pixels differing in every case. This covers linear buffers only; compressed buffers are refused, not verified. The test needs a HOST_COHERENT memory type for its staging buffers: with the first host-visible type (cached, not coherent) it reports false failures.
- Compressed buffers are refused, not supported: no verified AFBC modifier exists for this allocator revision.

**Patches:** `patches/0003-fix-invalid-drm-modifier-TENTATIVE.patch` (`src/panfrost/vulkan/panvk_image.c`) and `patches/0004-parse-arm-gralloc-handle-a38.patch` (`src/util/u_gralloc/u_gralloc_fallback.c`).

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

**Update (Oct 2026):** a configuration that takes prebuilt host tools from `PATH` (`-Dmesa-clc=system -Dprecomp-compiler=system`, see the collapsed build configuration below) never relinks them, so it did not hit this error and needed no `tls_align_fix2.py` run. The tools themselves must come from an earlier build.

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

   ```sh
   meson setup build-android -Dvulkan-drivers=panfrost -Dgallium-drivers= -Dplatforms=android \
     -Dandroid-stub=true -Degl=disabled -Dbuildtype=release -Dbuild-tests=false \
     -Dpanfrost-kmds=kbase,panthor -Dc_args="-D__ANDROID_API__=35 -target aarch64-linux-android35"
   ```
3. Apply this repo's `patches/` on top, then rebuild `src/panfrost/vulkan/libvulkan_panfrost.so`. If
`ninja` fails on a host tool with a TLS alignment error, run
`python3 tools/tls_align_fix2.py <path-to-the-failing-binary>` and retry — repeat for each tool ninja
reports (they get relinked, and need re-patching, whenever build flags change).

<details>
<summary>Build configuration that worked in Oct 2026 (Termux, native)</summary>

This minimal configuration builds `libvulkan_panfrost.so` (about 20 MB) natively in Termux and passes the two AHB tests of Finding 3 (`panvk_ahb_image_test` and its GPU-only variant) on the Oppo A38, and LukeValen's compute test (`0x309`) and offscreen draw test (red center pixel). It takes prebuilt host tools (`mesa_clc`, `vtn_bindgen2`, `panfrost_compile`) from `PATH`, so it never relinks them and needed no `tls_align_fix2.py` run (the tools themselves must come from an earlier build). It targets API 31, unlike step 2 above (API 35).

An older `build-android` directory could not be regenerated by `ninja`: the meson log shows that an NDK's `bin` directory was in `PATH`, so meson picked the NDK's `llvm-config` and failed to find the LLVM modules it asked for. If you have an NDK installed, keep it out of `PATH` when building this tree (`command -v llvm-config` should not point into it).

```sh
PATH=<dir with mesa_clc>:<dir with vtn_bindgen2>:<dir with panfrost_compile>:$PATH
meson setup build-min -Dbuildtype=release \
  -Dplatforms=android -Dandroid-stub=true -Dplatform-sdk-version=31 \
  -Dvulkan-drivers=panfrost -Dpanfrost-kmds=kbase,panthor -Dgallium-drivers= \
  -Dllvm=disabled -Dmesa-clc=system -Dprecomp-compiler=system \
  -Dglx=disabled -Dgbm=disabled -Degl=disabled -Dgles1=disabled -Dgles2=disabled \
  -Dtools= -Dvideo-codecs= -Dbuild-tests=false \
  -Dzstd=disabled -Dlmsensors=disabled -Dperfetto=false \
  -Dallow-fallback-for=libdrm --force-fallback-for=expat,libdrm,zlib \
  -Dlibdrm:default_library=static -Dexpat:default_library=static -Dzlib:default_library=static \
  -Dc_args="--target=aarch64-linux-android31 -ftls-model=global-dynamic -D__ANDROID_API__=31" \
  -Dcpp_args="--target=aarch64-linux-android31 -ftls-model=global-dynamic -D__ANDROID_API__=31"
ninja -C build-min -j4 src/panfrost/vulkan/libvulkan_panfrost.so
meson configure build-min | grep panfrost-kmds
```

`panfrost-kmds` must include `kbase`; the default is `[panfrost, panthor]`. Without it the driver still builds and creates an instance, but `vkEnumeratePhysicalDevices` returns no devices and a test crashes in `vkCreateDevice`. The `-D__ANDROID_API__=31` define is redundant with `--target` (the compiler warns that it is redefined); it was kept because it has not been tested without it. The configuration was validated with `-Dpanfrost-kmds` applied through `meson configure` after `meson setup`, which gives the same option state.

</details>

## Tests

- `tests/kbase-jm-prototype/` — raw `KBASE_IOCTL_JOB_SUBMIT` harnesses, independent of Mesa. Start with
`definitive_test.c` (uses ARM's official `struct base_jd_atom_v2`, stride computed by the compiler,
not hardcoded) to reproduce Finding 1 from scratch.
- `tests/panvk-driver-patch/` — real driver tests, via the actual built `libvulkan_panfrost.so`.
`panvk_ahb_test.c` exercises Finding 2; `panvk_ahb_image_test.c` exercises Finding 3 (real GPU clear + readback through an AHB-backed image with CPU usage, so the buffer is linear). `panvk_ahb_image_test_gpuonly.c` is the same test with GPU-only usage: on the A38 that buffer is compressed and the driver must fail with an explicit error. `ahb_probe2.c` dumps the gralloc handle and prints the alloc format and plane info for a grid of usages and formats. `ahb_handle_probe.c` is the first, simpler version of that probe. `ahb_layout_check.c` crosses the driver with `AHardwareBuffer_lock()` using per-pixel patterns in both directions; it takes the path of the driver `.so` as an optional argument. Each test `dlopen()`s the driver by an absolute path written in the file (search for `libvulkan_panfrost.so`); edit it to match your build directory.

## Open questions / help wanted

- Which DRM modifier (AFBC mode flags) corresponds to the A38's compressed buffers (alloc format high word `0xc410000`)? JimVulkan verified `0x311` pixel by pixel on a Galaxy A31, but nothing equivalent exists for this allocator revision. The stock Vulkan driver exposes `VK_EXT_image_compression_control` and `VK_EXT_image_drm_format_modifier`, so it could in principle act as a reference decoder (untested).
- Should an explicit `DRM_FORMAT_MOD_INVALID` coming from a wrapper (patch 0003) be mapped to LINEAR at all, or checked against the imported handle when the memory is bound?
- On-screen WSI via `ANativeWindow` — untested here.
- Packaging as a real Android Vulkan HAL (`vulkan-android.sym` symbol scheme) instead of loading via
`dlopen()` with an absolute path — untested here.
- Anyone with a Mali-G52 (or other Bifrost/JM) device on a *different* kernel/UAPI willing to run
`tests/kbase-jm-prototype/definitive_test.c` and report the `stride`/`event_code` result?

## Field notes: third-party drivers on the A38

What happened when other projects' drivers were run on this phone (Winlator forks, AIO Graphics Test; the fork is named where it mattered). These are reports, not endorsements, and the drivers change quickly.

| Driver | What happened |
|---|---|
| [JimVulkan/mali-panvk](https://github.com/JimVulkan/mali-panvk) release zips r3 and r4 (Winlator-Ludashi) | Swapchain creation fails with DXVK 1.x and 2.x. The log shows `ARM gralloc: no plane layout matches the handle (454x5, format 0xb32)` twice, then `invalid dmabuf size`. These builds refuse a handle they do not recognise. The numbers printed are the buffer's height, HAL format and usage read as width, height and format, which suggests the A38 header fields sit one int earlier than the parser expects. |
| Build of [JimVulkan/mesa-unified-drivers](https://github.com/JimVulkan/mesa-unified-drivers) at commit `a5010bb` (package from a Telegram channel; its `NOTICE.txt` credits JimVulkan) | Logs `unknown handle layout ... taken as linear` for this phone (see Finding 3). In Winlator-Ludashi it renders the AIO test with DXVK 1.x and 2.x. In Bannerlator the D3D11 cube does not render: the X server returns a `BadMatch` error (request major code 153) at the first present; with `MESA_VK_WSI_DEBUG=sw` it renders (about 50 FPS), while `linear` and `noshm` still fail. |
| [FristOneRR/FristOneRR-Panvk-Driver](https://github.com/FristOneRR/FristOneRR-Panvk-Driver), first test | D3D9, D3D10 and D3D11 test cubes render with DXVK 1.x. DXVK 2.x does not start: `VK_EXT_robustness2` is not exposed on Bifrost. |
| FristOneRR 3.0 (driver + wrapper, Oct 2026) | D3D9 to D3D11 cubes render. The log shows the runtime atom-stride probe choosing 56, "Using fallback gralloc implementation" and no gralloc warnings. |

The factory driver (Mali `r49p1`, Vulkan 1.3.278) does not expose `robustness2` either, according to this device's report on [FauxVkDB](https://fauxvkdb.canonspike.com/es/device/oppo-cph2579).

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
- [JimVulkan/mali-panvk](https://github.com/JimVulkan/mali-panvk) and [JimVulkan/mesa-unified-drivers](https://github.com/JimVulkan/mesa-unified-drivers): Mesa 26.3 PanVK ports for Bifrost with MediaTek support. Its ARM gralloc handle analysis (the alloc-format AFBC bit, the `NO_AFBC` + composer usage observations) pointed the Finding 3 investigation in the right direction. Neither the `mali-panvk` release builds (r3, r4) nor `mesa-unified-drivers` at `a5010bb` recognise the A38 handle layout (plane info at magic+16, not +15/+21; width, height and format one int earlier): the releases refuse the handle, the `a5010bb` build takes it as linear. That is why the measurements are published in Finding 3.
- [FristOneRR/FristOneRR-Panvk-Driver](https://github.com/FristOneRR/FristOneRR-Panvk-Driver): Mali-G57-focused PanVK/kbase driver with runtime atom-stride probing. It credits this repo for the 56-byte stride finding, and its log on the A38 independently arrives at 56.
- Icecream95 and the Panfrost/PanVK contributors — the underlying driver this all sits on top of.

## License

This repository contains only patches, test harnesses, a standalone ELF-patching tool, and
documentation — no copy of Mesa itself. Mesa is MIT-licensed; see
https://gitlab.freedesktop.org/mesa/mesa. `funnymdzz/mesa`, the fork everything here builds on, is also
Mesa-derived (MIT).
