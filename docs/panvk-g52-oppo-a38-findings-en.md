# PanVK on Mali-G52 MC2 — Oppo A38 (modern kernel) vs Redmi 13C (kernel 4.19)

Report from a testing session run natively in Termux, replicating and extending the work of
[LukeValen/panvk-mali-g52](https://github.com/LukeValen/panvk-mali-g52) on a second device with the
same SoC but a much newer kernel. Goal: isolate which parts of the work are kernel/DDK-dependent and
which are genuinely portable across devices sharing the same chip.

## Devices compared

| | Redmi 13C (original) | Oppo A38 (this report) | Infinix X6853 (G57 fork, reference) |
|---|---|---|---|
| SoC | MT6769V/CZ (mt6768) | MT6768 | Helio G99 |
| GPU | Mali-G52 MC2 | Mali-G52 MC2 | Mali-G57 MC2 (Valhall) |
| Kernel | 4.19.191 | **6.6.118-android15** | 6.12.38-android16 |
| Android | 14 (API 34) | 15 | 16 |
| Kbase UAPI | 11.38 | **11.46** | 11.46 |
| Root | No | No | No |

Key point: even though G52 and G57 are different chips, the Oppo A38 shares the **same UAPI (11.46)**
as the G57 device, not the 11.38 of the Redmi 13C — confirming that UAPI is determined by
kernel/DDK, not GPU model.

## Methodology

- Everything built 100% natively in Termux (clang 21.1.8, meson 1.12.0 via pip, ninja 1.13.2), no
  cross-compiling.
- Base: [funnymdzz/mesa](https://github.com/funnymdzz/mesa) fork (`kbase_kmod.c` backend, solves GPU
  discovery without a DRM node).
- Driver patches applied from LukeValen's `panvk-driver-patch/` (`apply_kbase_patch.py`,
  `fix_kbase_patch.py`, `gpu_variant_fix.patch`, `exec_init_order_fix.patch`), with device-specific
  adjustments where a script didn't apply cleanly (detailed below).
- Build targeted only at `src/panfrost/vulkan/libvulkan_panfrost.so` (not the full tree) to save
  time/battery on a low-end phone.

## Finding 1 — `base_jd_atom` stride (previously unreported data)

Direct bisection against `/dev/mali0` via `KBASE_IOCTL_JOB_SUBMIT`, using a dependency-only atom
(`core_req=0`, doesn't touch hardware, safe to run):

```
stride=48 -> EINVAL   (the value that works fine on the Redmi 13C, UAPI 11.38)
stride=56 -> OK       (new, not reported by any project so far)
stride=64 -> OK       (the value documented by the G57 fork, UAPI 11.46)
```

No other value tested (0, 16, 40, 44, 52, 60, 72, 80) was accepted. Conclusion: on UAPI 11.46 the
kernel requires an atom of at least 56 bytes; 64 is the easiest value to justify given it's already
documented for G57.

## Finding 2 — GPU detection is identical across kernels

`vkEnumeratePhysicalDevices` returns `count=1`, `deviceID=0x74021000`, with the same two warnings as
on the Redmi 13C (`EXEC_INIT EPERM`, `unknown gpu_id`) before applying the Phase 5/6 fixes. **Device
discovery is not affected by the kernel difference** — only job submission (stride) is. This is a
useful, clean split: the detection part of the project is portable as-is; the job-submission part is
not.

## Finding 3 — `gpu_variant_fix` and `exec_init_order_fix` are 100% portable

Both patches, originally designed and tested on kernel 4.19, worked without any modification on kernel
6.6.118:
- After applying them, `"Mali unknown 0x74021000 MC2"` → `"Mali-G52 r1 MC2"` (correct model recognition).
- The `KBASE_IOCTL_MEM_EXEC_INIT failed: Operation not permitted` warning disappears completely.

## Finding 4 — Bug in `apply_kbase_patch.py`: `core_req` out of sync with the README

The script applies `core_req=0x04` for the vertex/tiler/compute chain. But Phase 4 of LukeValen's own
README documents that the value that actually worked was `0x16` (`T|CS|V` combined: Tiler `0x04` +
Compute `0x02` + Value-writeback `0x10`). `0x04` alone only carries the Tiler bit — insufficient. This
looks like an oversight in the repo (script not kept in sync with the final documented fix), not
something device-specific. Fixed with a targeted `sed` before continuing.

## Finding 5 — False positive from a silenced `assert()` in `release` builds

`-Dbuildtype=release` defines `NDEBUG`, which turns every `assert(!ret)` in the patched driver code
into a no-op. Observed result: `vkQueueSubmit ok` and `vkQueueWaitIdle ok` were printed **even when the
job had actually failed** (a silenced hardware fault). Without additional instrumentation (we added
`fprintf(stderr, ...)` printing the real `event_code`), any build using this same configuration would
incorrectly report success. Worth documenting as a methodology warning for anyone reproducing these
tests.

## Finding 6 — Real compute: `JOB_CONFIG_FAULT` (0x40), distinct from the original G52's fault

With stride=64 and `core_req=0x16` already corrected, and debug instrumentation active:

```
PANVK_KBASE_DEBUG: submitting jc=0x5f7fd1040 core_req=0x16
PANVK_KBASE_DEBUG: poll returned 1, revents=0x1
PANVK_KBASE_DEBUG: read 24 bytes, event_code=0x40 atom_number=0
```

`0x40 = JOB_CONFIG_FAULT`. This is a **different and earlier** fault in the pipeline than the
`0x58 DATA_INVALID_FAULT` LukeValen found in Phase 3 on the Redmi 13C: `0x58` means the job was
accepted and executed but the data didn't land correctly; `0x40` means the job was **rejected during
validation**, before touching hardware. Not previously reported in any known thread.

Reproduced with a simple `WRITE_VALUE` job (the same one LukeValen used for Phase 3), adapted to
stride=64: same result, `event_code=0x40`. This confirms the issue isn't specific to the driver's
complex compute path — it happens even with the simplest possible job, once expanded to 64 bytes.

### Byte-by-byte verification (rules out a simple offset bug)

Buffer dumps before and after submission are **identical** — the GPU never actually executed anything,
nor left any evidence of an attempt. Verified against the real `*_pack` macros generated by Meson for
this build (`v7_pack.h`):

- Header (bytes 0–31): `type=2` (WRITE_VALUE), `index=1`, `invalidate_cache=1`, `next=0`,
  `exception_status=0` — all exactly matching what the code intended to write.
- Payload (bytes 32–63): `address=0x7ffffe100` (the correct `target_addr`), `type=3` (`ZERO`),
  `immediate_value=0x1234567890ABCDEF` — also exactly matching.

**Conclusion: the byte encoding is correct.** The fault is not a simple userspace offset error.

### Reordering hypothesis — investigated and ruled out

The initial hypothesis (vaguely suggested by the G57 fork: a 64-byte `base_jd_atom` with fields
reordered relative to the 48-byte one) **was ruled out via direct bisection**. Methodology: with a
dependency-only atom, writing a nonzero value at various candidate offsets only changes the outcome
(away from the baseline instant `DONE` with everything zeroed) when written at **byte 44**. Writing the
same value at 48, 52, 56, or 60 has no such effect on its own. **Conclusion: `core_req` remains at byte
44, unreordered.** The original 48-byte struct stays intact; what changes on newer kernels/DDKs is that
bytes are **appended** at the end (48→56 or 48→64), not that existing fields are rearranged.

Also ruled out by direct test: the hypothesis that `compat_core_req` (bytes 34-35, a 16-bit field from
the original v2 struct) must match `core_req` — writing the same value there didn't change the result
(`0x40` either way).

### Real field located in the 64-byte tail: offset 52-55

Word-by-word bisection (0 vs. `0xFFFFFFFF`) across the 16 appended bytes (48-63), keeping a real, valid
`WRITE_VALUE` job in the rest of the atom:

```
byte 48-51 -> no effect (0x40 with either value)
byte 52-55 -> ONLY offset with an effect: 0x00000000 -> 0x40 (instant fault)
                                           0xFFFFFFFF -> timeout (no event within 1.5s)
byte 56-59 -> no effect (0x40 with either value)
byte 60-63 -> no effect (0x40 with either value)
```

**The only real, active field in the 64-byte extension sits at bytes 52-55.** The rest is padding with
no observable effect.

## Phase 2 — Reverse engineering against the real kernel binary

With no public documentation resolving the offset 52-55 field, the actual `mali_kbase` driver compiled
for UAPI 11.46 was located and statically analyzed (no root, module never loaded): the file
`mali_kbase_mt6768_r49.ko`, identified by name from the device's own properties and downloaded from the
published kernel source of a different device with the same SoC
(`linx3141/android_device_bluefox_NX1-kernel`, kernel 6.6.82 — not the A38's exact kernel, but the same
DDK revision, which is what determines the struct layout). Confirmed via `readelf -p .modinfo`:
`version=r49p1-03bet0 (UK version 11.46)` — an exact UAPI match.

Analysis with `nm`/`objdump -dr` (symbols not stripped, relocations intact):

- **Real struct size: 56 bytes, not 64.** In `kbase_jd_submit`, the instruction right before the actual
  atom copy (`bl _copy_from_user`) is `mov w2, #0x38` (56 in decimal). This retroactively and
  consistently explains all three earlier empirical findings: why the stride bisection accepted both 56
  and 64 (the kernel only ever reads the first 56 bytes of any declared stride ≥56); why byte 52-55 had
  an effect (it's the struct's last real field); and why 56-63 had no effect (never copied at all).
- **Real `core_req` mask: `0x61F`.** In `jd_run_atom`, `core_req` lives at internal struct offset
  `0x124`, and is masked against `0x61F` to decide whether the atom completes without touching hardware
  — a concrete value confirming what Luke described in the abstract about `BASE_JD_REQ_ATOM_TYPE`.
- **Bit 17 of `core_req` triggers "renderpass" classification.** Inside `kbasep_js_add_job`, bit 17
  (`0x20000`) activates a code branch that compares an internal atom field against the same field of its
  dependency atom (`pre_dep`) — the classic pattern for pairing atoms of the same rendering pass,
  consistent with the "renderpass_id" the G57 fork mentioned. **Confirmed in real code, not from
  memory/recollection.** However, our test job has that bit cleared and sets no dependencies — so this
  branch is skipped entirely in our case; it doesn't directly explain our fault.
- **No software rejects our specific atom.** The full code path a job like ours takes
  (`core_req=0x10`, no dependencies, no renderpass bits) was traced: `jd_run_atom` →
  `kbasep_js_add_job` (returns cleanly, no error call anywhere) → `kbase_backend_run_atom` (only manages
  the hardware queue's ring buffer) → `kbase_backend_slot_update` → `kbase_job_hw_submit`. This last
  function builds the hardware registers (`JS_HEAD`/`JS_AFFINITY`/`JS_CONFIG`) from `core_req` and
  writes them straight to the GPU via `kbase_reg_write32/64` — **with no explicit job-type validation
  anywhere along the way**. The only `dev_err` in this entire call chain corresponds to a register-read
  timeout, not invalid content.

**Conclusion of this phase:** no software checkpoint was found, anywhere along the path from ioctl entry
to the hardware register write, that rejects our specific atom. This tips the balance strongly toward
`JOB_CONFIG_FAULT` being a **genuine hardware fault** — the GPU itself rejecting a configuration it
already received — rather than a software validation readable in this binary. Going further would
require ARM's silicon architecture reference manual for this exact Mali-G52 variant (not public), or a
direct comparison against a kernel that does work.

### Official confirmation: the 56-byte struct was already correct

A third project (`Noysz/panvk-g99-jm`, investigating G57/v9 with the same UAPI 11.46) pointed out that
the `mali_kbase` kernel driver is released by ARM under GPLv2 — the kernel side itself, separate from
the closed userspace blob. The corresponding official header (`jm/mali_base_jm_kernel.h`, via a public
mirror of ARM's source) was located, confirming `struct base_jd_atom_v2` field by field:

```
jc            @ 0   (8 bytes)
udata         @ 8   (16 bytes)
extres_list   @ 24  (8 bytes)
nr_extres     @ 32  (2 bytes)
jit_id[2]     @ 34  (2 bytes)   — replaced "compat_core_req" as of UK 11.20
pre_dep[2]    @ 36  (4 bytes)
atom_number   @ 40  (1 byte)
prio          @ 41  (1 byte)
device_nr     @ 42  (1 byte)
jobslot       @ 43  (1 byte)    — new, only relevant with BASE_JD_REQ_JOB_SLOT
core_req      @ 44  (4 bytes)
renderpass_id @ 48  (1 byte)    — new, only relevant with START/END_RENDERPASS
padding[7]    @ 49  (7 bytes)   — "Unused. Must be zero."
──────────────────────────────
TOTAL: 56 bytes
```

**This confirms, with official ARM source rather than just reverse engineering, that the 56-byte struct
and `core_req`'s position at byte 44 were already correct.** It also clears up two things that got
misidentified during the reverse-engineering session:

- The `core_req` bit 17 observed in `kbasep_js_add_job` **is not renderpass** — it's
  `BASE_JD_REQ_JOB_SLOT` (explicit hardware slot selection). Real renderpass uses bits 18 and 19
  (`BASE_JD_REQ_START/END_RENDERPASS`), which our job never set.
- The "real field" the bisection located at bytes 52-55 **does not correspond to `renderpass_id`**
  (a single byte at offset 48, not a word at 52-55) — it falls within the padding documented as "must
  be zero". The behavior change observed when writing `0xFFFFFFFF` there was almost certainly undefined
  behavior from violating reserved padding, not a real functional field. With padding correctly zeroed
  (as it was in most tests), the atom was already 100% correctly formed from the start.

**Conclusion of this phase (superseded by the finding below):** with the userspace struct now confirmed
correct, `JOB_CONFIG_FAULT` couldn't be explained by an atom formatting error — at this point in the
investigation that pointed toward a hardware/silicon-level limit. **It wasn't one.** The real cause was
found right after: see "Root cause found and fixed" below.

## Root cause found and fixed: the declared `stride`, not the atom's content

To close the investigation reproducibly, `definitive_test.c` was built: a test using the **official**
`base_jd_atom_v2` struct exactly as ARM defines it (real field names, not a single hand-computed offset,
with `_Static_assert(sizeof(...) == 56)` to catch any discrepancy at compile time). With
`stride = sizeof(struct base_jd_atom_v2)` (56, computed by the compiler, not hardcoded), the result was:

```
event_code=0x58 (BASE_JD_EVENT_DATA_INVALID_FAULT)
```

**Not `0x40`.** `0x58` is a real hardware fault — the job reached the GPU, executed, and the data didn't
land correctly — exactly the same code LukeValen documented in his own Phase 3, the step *right before*
achieving real rendering on his device. To isolate the variable unambiguously, the test was repeated
changing **only** the `stride` from 56 to 64 (everything else — struct, fields, values — bit-for-bit
identical):

```
stride=56 -> event_code=0x58  (DATA_INVALID_FAULT — the job DOES reach hardware)
stride=64 -> event_code=0x40  (JOB_CONFIG_FAULT   — rejected before touching hardware)
```

**The declared `stride` in the ioctl, by itself, decides whether the kernel takes the real hardware
submission path or an early-rejection path** — not the atom's content, which was identical in both
tests. The value `64`, used for most of this session (inherited without question from the G57 fork,
whose 64-byte struct is valid for *its* UAPI but not necessarily for every revision), turned out to be
exactly the cause that kept us stuck. The correct value for UAPI 11.46 on this device is the struct's
real, official size: **56**.

### Confirmed against the real driver, not just the isolated test

The same fix (`stride = 64` → `stride = 56`) was applied directly to the already-patched PanVK driver
(`panvk_vX_gpu_queue.c`, function `panvk_kbase_submit_and_wait`), rebuilt, and the real Vulkan compute
test (`panvk_compute_test.c` — full pipeline: `vkCreateInstance` → `vkCreateComputePipelines` →
`vkQueueSubmit` → `vkQueueWaitIdle` → buffer readback) was re-run:

```
PANVK_KBASE_DEBUG: submitting jc=0x5f7fd1040 core_req=0x16
PANVK_KBASE_DEBUG: read 24 bytes, event_code=0x1 atom_number=1
buffer after dispatch, first u32=0x00000309 (expected 0x00000309 = 777)
```

**`event_code=0x1` (real `DONE`, not the silenced-`assert()` false positive documented earlier) and the
buffer holding exactly the expected value (777).** Real GPU compute, verified end to end through actual
PanVK/Vulkan — not an isolated harness — on a Mali-G52 MC2 running kernel 6.6.118-android15, UAPI 11.46.
This is the same milestone LukeValen reached on the Redmi 13C (kernel 4.19.191, UAPI 11.38),
independently reproduced on a second device with a different DDK revision, thanks to combining two
fixes: `core_req=0x16` (Finding 4, corrected earlier in this session) and `stride=56` (this finding).

### Recommendation: stop hardcoding the stride

The correct value (56) is specific to this UAPI/DDK — the Redmi 13C itself uses 48. Hardcoding any fixed
value is fragile by design; it's literally the same kind of bug that caused this entire investigation,
just with a different number. The recommendation for the project is a runtime probe: on opening
`/dev/mali0`, submit a harmless atom (`core_req=0`, doesn't touch hardware) with candidate strides in
ascending order, and keep the first one the kernel accepts without `EINVAL` — already empirically
confirmed that the kernel validates stride against a set of known sizes, not "anything large enough"
(our own bisection rejected 60 but accepted both 56 and 64). This would make the driver portable across
kernels without tripping over this same bug again.

### The attempt to get a Redmi 13C (UAPI 11.38) `.ko`

A `mali_kbase` module matching the Redmi 13C's DDK revision (UAPI 11.38) was sought without success,
originally to directly diff the validation code between both versions. Xiaomi has not published kernel
source for the Redmi 13C 4G (codename `gale`/`gust`) — confirmed by open, unanswered community requests
on `MiCode/Xiaomi_Kernel_OpenSource`'s GitHub issue tracker. With the root cause resolved another way,
this was no longer necessary to close the investigation.

## Phases 4-9: replicating Luke's methodology (Oppo A38)

With the stride bug resolved, the rest of the path Luke documented in his own repository was
replicated, phase by phase, with practically identical results:

- **Phase 4 (real compute via PanVK)**: confirmed — see the finding above.
- **Phase 5 (model recognition)** and **Phase 6 (EXEC_INIT/JIT_INIT ordering)**: Luke's same patches
  (`gpu_variant_fix.patch`, `exec_init_order_fix.patch`), applied unmodified, same results
  (`"Mali-G52 r1 MC2"`, `EXEC_INIT` warning gone).
- **Phase 7 (real offscreen draw call)**: Luke's `panvk_draw_test.c`, without touching a single line —
  triangle drawn, center pixel exactly `RGBA(255,0,0,255)`, matching his documented result.
- **Phase 8 (X11 WSI)**: deliberately skipped — not a prerequisite for Android WSI, which is the real
  goal for Winlator.
- **Phase 9 (Android WSI / AHardwareBuffer)**: Luke's `android_wsi_ahb_fixes.patch` applied cleanly.
  Building the Android platform variant did require one additional fix Luke didn't document (see the
  next section). Once resolved, same result as his — `AHardwareBuffer_allocate()` against the real
  system `libnativewindow.so`, `vkGetAndroidHardwareBufferPropertiesANDROID()` against the driver,
  returning correct values (`allocationSize=16384`, exact for a 64×64 RGBA8 buffer;
  `memoryTypeBits=0x5`, nonzero).

### New finding: build binaries misaligned for Bionic's loader

The first time building with `-Dplatforms=android`, several codegen tools used during the build itself
(`mesa_clc`, `vtn_bindgen2`, `panfrost_compile` — generate code from OpenCL/SPIR-V kernels as part of
the build process, not part of the final driver) failed to *run* with
`error: executable's TLS segment is underaligned... needs to be at least 64 for ARM64 Bionic`. This is
a toolchain issue documented across the Termux ecosystem for years (same error reported in unrelated
packages like Valgrind, PHP, Go), with no clean compiler-flag-level fix — it's resolved by patching the
already-built ELF, fixing both `p_align` and the "skew" (`p_vaddr % 64`) of the `PT_TLS` segment, not
just `p_align` (a partial fix leaves the binary equally broken, with a different "skew" error).
Separately, `AHardwareBuffer_acquire`/`_release` (API 26+) showed up as "unavailable" despite compiling
with the correct API level declared — Clang's availability check is driven by the compiler's *target
triple*, not the `__ANDROID_API__` macro passed via `-D`; had to force
`-target aarch64-linux-android35` explicitly.

Not documented by Luke in his own repository — possibly a toolchain version difference between his
session and this one, or he already had those tools built from an earlier session without relinking
them from scratch.

## Beyond Luke's documented point: `panvk_android_import_ahb_memory()` fixed

Luke explicitly left this open in his own commit: *"Not yet done: `panvk_android_import_ahb_memory()`
in `panvk_android.c` still assumes `data[0]` for the actual `vkAllocateMemory` import path — same class
of bug, same fix needed there next."* The same pattern he already used and validated in
`vk_common_GetAndroidHardwareBufferPropertiesANDROID` (`src/vulkan/runtime/vk_android.c`) was applied
here: instead of assuming the dma-buf fd lives at `handle->data[0]`, probe each fd with
`lseek(SEEK_END)` (which only succeeds on a real dma-buf) and keep the first one that confirms it.

With debug instrumentation added to verify this unambiguously, the result on this device:

```
dma_buf_index=1 (de 3 fds) dma_buf_fd=6 dma_buf_size=16384 data[0]=5
vkAllocateMemory (import AHB real) ok
```

**`data[0]` (fd=5) was not the real dma-buf** — confirming the exact same hardware finding Luke made on
his own device, now independently reproduced on a Mali-G52 with a different gralloc fallback. The real
dma-buf was at `data[1]` (fd=6, 16384 bytes — exactly matching the expected size of the 64×64 RGBA8
buffer). Without the fix, `panvk_android_import_ahb_memory()` would have tried the wrong fd and failed.
With the fix, `vkAllocateMemory` importing the real `AHardwareBuffer` succeeded end to end — the first
time this specific path has been confirmed on either related project (G52 or G57).

## Second bug found and fixed: unhandled `DRM_FORMAT_MOD_INVALID` in `panvk_image_get_explicit_mod()`

With `panvk_android_import_ahb_memory()` already fixed, the natural next step was confirming the
imported memory is actually useful: binding it to a real `VkImage` (`vkCreateImage` +
`VkExternalMemoryImageCreateInfo` + `vkAllocateMemory` with `VkMemoryDedicatedAllocateInfo`) and
rendering something into it. This exercises a completely different code path from the one already
tested (`panvk_android_ahb_image_init()` → `vk_android_get_ahb_layout()` → shared Mesa code), never
before exercised by any related project — the first attempt ended in `Segmentation fault` (`exit code
139`).

The cause was traced using the same method as the stride bug at the start of the session: debug
instrumentation added level by level, marker by marker, until isolating the exact line. The full chain:
`panvk_android_ahb_image_init()` → `vk_android_get_ahb_layout()` (shared Mesa code, not PanVK-specific)
→ `vk_gralloc_to_drm_explicit_layout()` — all confirmed correct, returning sane data (`num_planes=1`,
`stride=256`, format `DRM_FORMAT_ABGR8888`) — down to `panvk_image_init()` →
`panvk_image_get_explicit_mod()`, where:

```c
uint64_t mod = explicit->drmFormatModifier;   // = DRM_FORMAT_MOD_INVALID
...
assert(panvk_image_can_use_mod(image, iusage, mod, false));  // almost certainly fails, but a no-op in release
return mod;   // returns DRM_FORMAT_MOD_INVALID anyway
```

**Without a real vendor gralloc** (`"No gralloc hwmodule detected"`, present in every run this session),
Mesa's fallback reports `DRM_FORMAT_MOD_INVALID` as the modifier — a legitimate sentinel meaning "no real
tiling modifier is known", but one this function never expected to receive: there's no `if` catching it,
only an `assert()` as the sole defense — the exact same NDEBUG-silences-asserts pattern already
documented as Finding 5 earlier in the session, now found a second time, in a completely different part
of the driver.

**Fix applied:** substitute `DRM_FORMAT_MOD_INVALID` with `DRM_FORMAT_MOD_LINEAR` (the standard
"no tiling" convention, universally supported) before using it:

```c
if (mod == DRM_FORMAT_MOD_INVALID)
   mod = DRM_FORMAT_MOD_LINEAR;
```

**Result, with the fix applied — full end-to-end validation:**

```
vkCreateImage (AHB-backed) ok
vkAllocateMemory (dedicated, bound to VkImage) ok
vkBindImageMemory ok
vkQueueSubmit ok
vkQueueWaitIdle ok
center pixel after clear: R=0 G=0 B=255 A=255 (expected: 0,0,255,255 = blue)
```

Exact match. A `VkImage` backed by imported `AHardwareBuffer` memory, cleared to pure blue by a real GPU
command and read back correctly — the first confirmation, across any of the related projects (G52, G57,
G99, G720), that an AHB-backed image is a genuinely functional GPU render target through this path.

### ⚠️ Important correction: the `DRM_FORMAT_MOD_LINEAR` fix is not confirmed correct

The G720 project (`wonderkast02/panvk-g720-kbase-csf`), considerably further along than this work,
includes an explicit warning in its own README: *"Never convert `DRM_FORMAT_MOD_INVALID` to
`DRM_FORMAT_MOD_LINEAR` by assumption."* — exactly the fix applied above. The test that validated it
(clearing the image to a solid color and reading it back) is too simple to catch an incorrect layout:
reading back through the same driver that made the assumption is self-consistent even if `LINEAR` isn't
actually the layout the hardware or an outside consumer (a real texture with mipmaps, a compressed
format, or anything outside this same process) would expect. **This finding is marked as a tentative
fix, not a confirmed solution** — it unblocked progress and got things this far, but the root cause of
why `LINEAR` might be wrong (and what the correct substitution would be) was uninvestigated at that point (see the follow-up below).

## Follow-up: measuring the gralloc handle instead of assuming it (patch 0004)

The warning from the G720 project was right, and the open question turned out to be measurable: what does the allocator actually produce, and where does the handle say so?

**Where the investigation pointed.** [JimVulkan/mesa-unified-drivers](https://github.com/JimVulkan/mesa-unified-drivers) parses ARM's gralloc handle directly. A 64-bit "alloc format" holds the HAL format in its low word and layout bits (AFBC) in its high word; MediaTek's allocator on a Galaxy A31 was matched against `0x311`. It keeps `LINEAR` only as a fallback for handles it does not recognise. Its parser looks for the plane info at magic+15 (Exynos) or magic+21 (A31). On this phone it logs "unknown handle layout" (Winlator, `mali-panvk` r3), because here the plane info sits at magic+16.

**Method.** Small programs allocate `AHardwareBuffer`s through the system allocator and dump the native handle (`AHardwareBuffer_getNativeHandle`): a first probe (`tests/panvk-driver-patch/ahb_handle_probe.c`: 64x64 and 500x500, three usages) and `tests/panvk-driver-patch/ahb_probe2.c` (200x100, RGBA8 and RGBX8, eight usage combinations).

**Handle layout on the A38** (3 fds, 62 ints). fd0 reports no size (`-1`; JimVulkan's source notes that MediaTek puts `-1` first), fd1 is the buffer, fd2 is a 6480-byte region in every sample. Among the ints: magic `0x3141592` at `ints[18]`, the u64 alloc format at `ints[29..30]`, and plane 0 `{byte stride, width, height}` at `ints[34..36]`. Other ints also change for compressed buffers (for example `ints[14]` and `ints[30]`); they were not decoded.

**Results** (200x100):

| usage | alloc format (high word) | buffer size | layout |
|---|---|---|---|
| `GPU_SAMPLED` only | `0` | 86016 | linear |
| `SAMPLED + COLOR` with CPU read, CPU write or `COMPOSER_OVERLAY` | `0` | 86016 | linear |
| `COLOR` only, `SAMPLED + COLOR`, `SAMPLED + COLOR + NO_AFBC` | `0xc410000` | 135168 | compressed |

The high word separates the two cases in 16 of 16 samples (RGBA8 and RGBX8 gave identical results). The first probe shows the same pattern: 64x64 with CPU read usage is 16384 bytes, GPU-only is 69632 (128x128x4 + 4096); 500x500 is 1024000 versus 1064960 (512x512x4 + 16384).

The arithmetic fits: the linear buffer has 208 px x 4 = 832 bytes of stride, 83200 bytes, rounded up to 21 pages. The other is 256x128 (128-aligned) x 4 = 131072 bytes of body plus a 4096-byte header, consistent with AFBC (16x16 superblocks, tiled headers). The actual modifier flags were **not** verified. The private `NO_AFBC` bit (`1 << 29`) has no effect on this allocator; CPU or composer usage is enough to keep a buffer linear, which agrees with JimVulkan's notes on MediaTek's allocator.

**Patch 0004** (`src/util/u_gralloc/u_gralloc_fallback.c`) acts only when the handle matches that fingerprint (3 fds, 62 ints, magic at `ints[18]`). High word `0` plus a plane stride equal to `pixel_stride x bpp` gives `DRM_FORMAT_MOD_LINEAR`. A non-zero high word is refused with an explicit error. Any other handle behaves as before.

Verified on the device:

```
info.modifier=0x0
pixel central despues del clear: R=0 G=0 B=255 A=255        (panvk_ahb_image_test, CPU + composer usage)

MESA: error: ARM gralloc: compressed buffer (alloc format 0xc410000:00000001) is not handled
FAILED vkAllocateMemory ...: VkResult=-1000072003            (panvk_ahb_image_test_gpuonly, VK_ERROR_INVALID_EXTERNAL_HANDLE)
```

**Dead ends and corrections.**
- It was suggested that the original test buffer was probably AFBC and that `LINEAR` was therefore wrong. It was not: the test requests `CPU_READ_RARELY`, which keeps the buffer linear. Patch 0003 was right for that buffer, only unproven.
- Symbol interposition of `hw_get_module` inside a real Android process was considered as the reason another project's gralloc worked. Ruled out: that driver also logs "Using fallback gralloc implementation" inside Winlator.
- Adding `NO_AFBC` to the test's usage was unnecessary on this allocator (see above).

**Still an assumption.**
- Patch 0003 is still needed when the caller passes an explicit `DRM_FORMAT_MOD_INVALID`, as Winlator's wrapper does according to LukeValen's README. There is no handle to inspect in that path, and `LINEAR` would be wrong for a GPU-only buffer.
- The fingerprint is confirmed only on the A38.
- The "linear" evidence is the measured correlation; the pixel readback still goes through the same driver. An independent `AHardwareBuffer_lock()` check is pending.
- Compressed buffers are refused, not supported.

**Related observations on the same device** (details in the README, "Field notes"): FristOneRR's driver independently chooses stride 56 on the A38 and renders D3D9 to D3D11 test cubes in Winlator; LukeValen's README documents the wrapper-supplied `INVALID` modifier and a `SYNC_FD` fix; JimVulkan's notes say MediaTek's kernel rejects the DRM sync ioctls.

## Status: active investigation

With this, this report doesn't just reach parity with the public state of Luke's project — it goes
beyond it on one concrete, verified point. The gralloc follow-up builds on JimVulkan's earlier analysis of ARM's handle layout; what is new here is the A38 measurement table and the handle fingerprint. Other projects have since credited or independently reproduced parts of this work (see the README, "Field notes" and "Credits").

## Suggested next steps

1. **Report the stride fix to LukeValen and to the maintainer of `apply_kbase_patch.py`** (FristOneRR's driver already probes the stride at runtime and credits this repo) — this is the most
   important finding of the whole session: the value `64` (copied from the G57 fork without checking it
   against the real struct size for this UAPI) is the cause of `JOB_CONFIG_FAULT`, not any hardware
   issue. Worth implementing the runtime probe suggested above instead of hardcoding 56 as the new fixed
   value.
2. Done: Luke's Phase 7 (real offscreen draw call) was replicated on this device with no changes (see "Phases 4-9" above).
3. Report the `core_req=0x04` bug in `apply_kbase_patch.py` to the maintainer (already corrected to
   `0x16` for this session).
4. Suggest adding a README note about the effect of `NDEBUG` on the verification `assert()` calls —
   without the manual instrumentation added here, this exact fix would have been invisible (the
   `release` build reported success even with the wrong stride).

5. Verify the linear case independently (`AHardwareBuffer_lock()` on a CPU-readable buffer) instead of reading back through the same driver.
6. Decide what to do with an explicit `DRM_FORMAT_MOD_INVALID` supplied by a wrapper (patch 0003 is still an assumption there); one option is to check the imported handle when the memory is bound.
7. Share the A38 handle layout (plane info at magic+16, alloc format at magic+11, the usage table) with the projects whose parsers do not recognise it.

## Reference environment (for reproduction)

- Termux, clang 21.1.8, meson 1.12.0, ninja 1.13.2, libclc 21.1.3-1
- `meson setup build -Dvulkan-drivers=panfrost -Dgallium-drivers= -Dplatforms= -Dbuildtype=release -Dbuild-tests=false -Dpanfrost-kmds=kbase,panthor`
- `ninja -C build -j1` (or `-j4` for small incremental rebuilds)
- Resulting `libvulkan_panfrost.so`: ~20MB, 1014 targets, no errors
- Fixes applied on top of `funnymdzz/mesa`: vertex/tiler/compute chain `core_req` corrected from `0x04`
  to `0x16`; `KBASE_IOCTL_JOB_SUBMIT` `stride` corrected from `64` to `56`
- Android platform build (`-Dplatforms=android`): see the README, "Build configuration that worked".
