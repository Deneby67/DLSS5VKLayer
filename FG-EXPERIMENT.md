# SDR FG ×2 development status

This branch is an **experimental backend and profile foundation**, not working
in-game frame generation. NR and the Rockstar Social Club renderer exclusion
remain available. No NVIDIA binaries are included in the repository.

**Current scope: FG is deferred at the user's request.** Active development is
[DLSS 5 NR → the game's DLSS SR](ngx_capture/NR-BEFORE-SR.md). This opt-in chain
passed isolated GPU validation with the actual RDR2 SR DLL, but caused a hang
before RDR2's main menu and was rolled back. Startup diagnosis remains open. The FG
research below is retained for reference. The expensive draw-discovery path
remains disabled.

## Verified gate

On RTX 5090, driver 615.71.09, custom Stable Proton
`Proton-11.0-2c-Zen5-BP18-Mimalloc`, the direct Vulkan NGX Feature 11 test creates
the feature, evaluates it, reads back the generated image and verifies the
moving object's midpoint against the adjacent real frames. The perspective scene
has actual foreground/background depth and known camera/motion data.

Latest run: **9/9 verified intermediate frames, 0 Vulkan Validation errors**.
Validation used an explicit debug messenger, including resource teardown.
This verifies the backend at 1280×720 SDR; it does not measure game frame pacing,
latency, HUD quality or RDR2 compatibility. The installed-helper NR regression
test also processed 6/6 synthetic frames, including a scene cut, with optical
flow enabled. JSON parser/source-selection tests pass independently.

The production `bin/x64/nvngx_dlssg.dll` comes from NVIDIA's official
[Streamline SDK v2.14.1](https://github.com/NVIDIA-RTX/Streamline/releases/tag/v2.14.1).
Its version is 310.9.1 and SHA-256 is
`ff6e90eb78b827927dff5b4ecc6b1c870c2e9bca29ed9f48c7d348cc9e170b82`.
`tools/import-fg-sdk.py` verifies both archive and member hashes and writes a local
manifest. Acquire and use the SDK under NVIDIA's applicable terms.

Backend fixes required for this gate:

- Separate FG parameter adapter matching the 17-slot MSVC NGX ABI. NR's existing
  adapter is retained for compatibility.
- Correct `Init_Ext2` signature and the existing caller-identity mechanism.
- Buffer device addresses enabled; interpolation cancellation is a writable
  storage buffer with device-address-capable memory, not an image.
- Track the input color's post-evaluation GENERAL layout before its next upload.
- Read Windows Unicode arguments for DLL paths, including Cyrillic directories.

## Game integration still pending

Prefer validated **engine motion vectors**, with optical flow as the motion-only
fallback. [JSON profiles](profiles/fg/README.md) pin executable/shader hashes and
descriptor locations/offsets. Invalid or mismatched profiles disable game FG;
fallback cannot supply missing depth/camera. RDR2's profile is intentionally
disabled until a real gameplay capture verifies its bindings.

Still required: Vulkan resource/submission tracking and preservation, RDR2 camera
and motion calibration, independent optical-flow service, per-process/session
frame ring, virtual swapchain/paced presentation, game GUI controls and metrics,
resize/failure recovery, and NR/FG/combined comparisons in RDR2. The current
helper only loads profiles and reports their status; it does not enable FG in
games. Its `--fg-self-test DLL` option runs the offscreen gate.

## Local build and tests

`bash tools/build-fg-probe.sh` compiles the backend, probe and updated NR helper
with Clang/LLD and at most 24 workers. It expects a MinGW x86-64 POSIX development
sysroot at `build/fg/sysroot/usr` (or `MINGW_SYSROOT`) and Clang at
`/opt/llvm-23.1.1-bp18/bin` (or `LLVM_BIN`). The local build used mingw-w64 13.0.0
headers and GCC 13.2 POSIX runtime libraries. The usual Windows Meson build also
contains the new sources.

```sh
python3 tools/import-fg-sdk.py /path/to/streamline-sdk-v2.14.1.zip
bash tools/build-fg-probe.sh
bash tools/test-fg-profile.sh
python3 tools/run-fg-probe.py --proton /path/to/proton
```

For `--validation`, place the Khronos validation library/manifest under
`build/fg/validation/root/usr/share/vulkan/explicit_layer.d`, using a library path
visible inside the Steam runtime. Tested layer version: 1.4.357. The runner
archives old logs before each run. A requested but unavailable debug messenger
fails the test.

NR regression (uses separate SHM and prefix, never the active game's channel):

```sh
c++ -std=c++17 -O2 tools/shm_frames.cpp -pthread -o build/fg/shm-frames
python3 tools/run-nr-smoke.py --proton /path/to/proton --binaries /path/to/NR/binaries
```

`tools/install-fg-helper.py` backs up the existing installation/configuration,
installs the helper under `~/.local/lib/dlssnr-fg`, and creates per-user launcher
and desktop overrides. Its timestamped backup includes `restore.py`; stop the
helper and close its window before rollback, then reopen the window. The system
package and current NR DLL are preserved. This diagnostic install does not change
game launch options or install game FG presentation.

The old direct `tools/capture-rdr2.sh` attempt is retained only as a diagnostic
record: wrapping the entire runtime in RenderDoc did not launch RDR2 on the test
machine. A normal Steam launch with RenderDoc enabled for the child process tree
also failed: Rockstar Launcher threw an exception in D3D11 device discovery,
while RenderDoc reported failure to create the Vulkan device chain. This occurred
before RDR2 started. `tools/steam-fg-capture.sh` therefore now disables RenderDoc
and otherwise passes through the original command and NR settings, restoring
normal launch for users who already added that prefix.

### Selective capture gate

`tools/rdr2_capture_gate.cpp` now checks Wine's executable basename before loading
RenderDoc, including the pre-instance extension-enumeration entry point. It
forwards Vulkan unchanged for excluded processes. Rejecting layer negotiation
was tested first but caused instance creation to fail on this loader, so exclusion
uses transparent instance/device dispatch instead. A missing backend also falls
back to normal device creation. Matching does not accept `RDR2.exe.bak` or an
ancestor directory containing `RDR2.exe`.

Verified with native Vulkan device tests and the custom Stable Proton:

- Launcher, Social Club and other names create devices without mapping RenderDoc.
- A Vulkan test named RDR2 loads RenderDoc and presents four frames successfully.
- The actual Rockstar Launcher now completes device discovery without RenderDoc;
  the actual RDR2 process loads RenderDoc and initializes a capture device.

**Real gameplay capture still fails:** RDR2 hangs during startup with RenderDoc
1.46 in this environment. Temporarily disabling DLSS and Reflex did not resolve
the hang; original settings were restored. No 3D gameplay frame was obtained.
Attaching the capture backend is therefore not treated as a successful capture.

The installed Steam wrapper leaves capture **off by default**, preserving normal
NR/game launch. `DLSSFG_ENABLE_RENDERDOC=1` is reserved for explicit developer
diagnosis, not a recommended game launch option. Further work must diagnose the
game-specific hang or collect resources through our own Vulkan tracking hooks.

```sh
bash tools/build-capture-gate.sh
python3 tools/test-capture-gate.py --backend /path/to/renderdoc/lib/librenderdoc.so
python3 tools/install-capture-gate.py --backend /path/to/renderdoc/lib/librenderdoc.so
```

The installer expects an already-registered per-user RenderDoc manifest, backs
it up, replaces its entry points with the gate, and installs the inactive Steam
wrapper. Source and runtime binary are kept separate; RenderDoc is not vendored.

### RDR2 metadata discovery without RenderDoc

The native layer now has an opt-in metadata recorder, scoped to the exact
`RDR2.exe` process basename. It records shader modules (SPIR-V plus SHA-256),
image/view/buffer descriptions, descriptor/pipeline layouts, graphics/compute
pipeline shader associations, descriptor writes/copies and object lifetimes.
Descriptor pool reset/destruction invalidates the recorded set generations.
Present markers are **CPU observations, not GPU completion or frame identity**.
The Rockstar renderer exclusion is retained.

This is an inventory stage, **not engine depth/motion/camera capture or game FG**.
The inventory recorder does not capture command recording or resource contents.
The separately requested CPU snapshot probe described below observes selected
command bindings and submissions without changing GPU work. RenderPass2, dynamic rendering, descriptor update templates,
shader objects and inline shader modules are not covered. Partial pipeline
creation failures are not recorded. The session header declares these gaps.
The recorder adds no GPU commands, waits, usage flags or synchronization, but
synchronous metadata/shader writes can slow loading and rendering while active.
Do not use this diagnostic run for performance comparisons.

Per process, JSONL is limited to 64 MiB, shader files to 128 MiB and live tracked
objects to 500,000. Descriptor writes/copies have a separate 16 MiB share of
the JSONL budget. Exhausting shader storage records `binary_saved=false` while
continuing hashes and resource metadata. Exhausting descriptor logging suppresses
further update records while inventory continues. Reaching the overall log/object
limit stops recording; ordinary Vulkan dispatch continues.
`DLSSFG_SHADER_LIMIT_MIB` can set the shader budget from 0 to 1024 MiB; invalid
values retain the 128 MiB default. An unwritable directory or absent `libcrypto.so.3` disables discovery
without preventing device/resource creation, including hooks cached before the
device existed. Captures use private directories/files and are not published.

Build the native layer with up to 24 jobs, then run the isolated GPU test:

```sh
CXX=/path/to/clang++ bash tools/build-discovery.sh
python3 tools/test-discovery.py \
  --layer build/discovery/libVkLayer_NV_dlssnr.so \
  --probe build/discovery/discovery-probe \
  --validation /path/to/VkLayer_khronos_validation.json
python3 tools/install-discovery.py --layer build/discovery/libVkLayer_NV_dlssnr.so
```

Validation manifests must reference an absolute library path. Tests exercise real
Vulkan shader/pipeline/resource creation, descriptor reset/free and generations,
concurrent lifetimes, independently checked SHA-256/deduplication, excluded
process names, absent output directory, disabled capture and log-limit fallback.
The analyzer also rejects corrupted shader files. The test checks that the
validation library is actually mapped when requested.

The installer requires the existing per-user layer manifest and Steam wrapper.
It installs a separate library at `~/.local/lib/dlssnr-fg/layer`, backs up the
manifest, wrapper, diagnostic marker and previous library, and supplies a
`restore.py`. Selection uses atomic file replacement; a running game retains
its old mapping. The normal Steam wrapper enables metadata discovery when
`~/.config/dlssnr/fg-discovery.enabled` exists. `DLSSFG_DISCOVERY=0` or removing
the marker disables it. RenderDoc remains disabled by default. The helper,
Stable Proton, NR parameters and game settings are unchanged.

After restarting RDR2 through Steam, sessions appear under
`~/.local/state/dlssnr/discovery/rdr2-PID-TIMESTAMP`. Load a save and move the
camera, then inspect the inventory:

```sh
python3 tools/analyze-discovery.py /path/to/session --output /private/path/report.json
```

The report lists depth attachment candidates, **unverified** motion-format
candidates, shader bindings and top-level matrix member offsets. Reflection is
limited to direct SPIR-V decorations. It never writes or enables a game profile:
formats and matrix types alone cannot establish motion direction, camera
semantics, current-frame ownership or depth correctness. Real RDR2 startup and
inventory beyond loading still need a fresh game run with the revised budgets.


#### First real RDR2 inventory

RDR2 reached gameplay with the recorder loaded and without RenderDoc. The first
recorder stopped after 6,186 unique shader binaries exhausted its 128 MiB budget,
at CPU present marker 8 during loading. The native layer itself continued running.
The budget handling above fixes this diagnostic failure: shader/descriptor caps
no longer stop the rest of resource discovery. Tests force a zero-byte shader
budget and overflow the descriptor budget, then verify continued resource lifetime
records and normal device destruction with Vulkan validation enabled.

The loading inventory includes depth attachments at 2293×960 and 3440×1440. A
uniform block at set 0/binding 29 contains seven matrix members at byte offsets
0, 64, 128, 192, 272, 336 and 400; the same layout appears in 4,083 saved shaders.
Another group uses binding 28 with the same offsets. These are **camera candidates,
not confirmed camera data**. Shader debug names are absent. The analyzer now groups
these structural candidates and reports omitted binaries explicitly. It still
never enables a game profile, and no motion resource or gameplay buffer contents
have been verified. Private captures/reports are excluded from the repository.

#### Relating the second inventory

The revised recorder continued through CPU present marker 3843, recording about
14,000 images and 2,900 graphics pipelines before its overall 64 MiB log limit.
Descriptor logging had reached its separate budget at marker 1196. These markers
are not evidence of a particular gameplay scene or GPU completion.

`tools/relate-discovery.py` joins shader layouts, pipeline layouts, descriptor
writes and image/buffer generations. It requires exact recorded set-layout
identity, leaves descriptor copies unresolved and does not treat a descriptor
write or framebuffer creation as proof of execution. It reports candidates only.

```sh
python3 tools/relate-discovery.py /private/session \
  --report /private/report.json --output /private/resource-links.json
python3 tools/test-relate-discovery.py
```

The RDR2 inventory contains 464-byte uniform-buffer slices associated with the
seven-matrix layout at vertex-stage set 0/binding 29 (another variant uses binding
31). Their offsets change within a 128 MiB buffer. The profile therefore needs
descriptor-relative selection rather than a fixed process/buffer address.
Render-size RG16F images also appear, but the recorded descriptor window has no
confirmed link identifying one of them as the motion field. Actual buffer values,
draw/submission state and GPU resource readback still need to be captured and
validated before any camera/depth/motion profile can be enabled.

### Requested CPU uniform snapshots

The native layer now maintains a separate, bounded camera-probe state even after
inventory logging stops. It observes memory allocation/mapping, buffer memory
bindings, descriptor-set lifetimes and graphics descriptor bindings in primary
and secondary command buffers. A request made after loading the game records up
to eight distinct 464-byte slices per 100 ms sampling interval from set 0,
bindings 29/31. This selection is a diagnostic hypothesis from the inventory,
not a verified camera profile or proof of shader consumption.

Only existing HOST_VISIBLE + HOST_COHERENT mappings are read. The address accounts
for allocation binding offset, descriptor offset and mapping offset; allocation,
buffer and set generations prevent reuse of stale handles. Reads first use a
self `process_vm_readv` call. NVIDIA HOST_VISIBLE + DEVICE_LOCAL mappings may
reject that call with EFAULT, so a failed/short read falls back to a private
nonblocking pipe: a bounded write copies the source through kernel
`copy_from_user`, then a read retrieves the bytes. This does not pin device
pages, directly dereference the source or install signal handlers. The pipe
is emptied after each successful read and discarded on any short/error result.
Unreadable mappings produce a skipped sample rather than a process fault.
Noncoherent/unmapped/out-of-range memory is skipped. No
memory is newly mapped, flushed or invalidated and no GPU commands or waits are
added. The probe records bytes before the application's queue submission and
records the submission result separately. GPU writes, concurrent host writes,
actual draw consumption and completion are **not** established by these samples;
observations may be stale or torn. They cannot yet be inputs to game FG.

Commands reset/free/pool reset discard prior bindings; secondary command
references are generation-checked. Descriptor copies and update templates
invalidate candidate slices instead of guessing their contents. Spill/array
writes are conservatively unresolved. Dynamic uniform descriptors, newer
CmdBindDescriptorSets2 commands and push descriptors are not decoded. Ordinary
legacy bindings are recorded as historical references within a command buffer,
not as a full draw-state reconstruction. Tracking errors disable only the probe.
A device supports at most 500,000 tracked objects and 262,144 command/set
references. Output is capped at 8 MiB per request, 1,024 samples and 30 seconds.
At most 128 candidate reads are attempted per sampling interval; unsuccessful
reads also advance the 100 ms throttle. Each snapshot records its read method
and the original syscall errno; failed reads retain both error codes.

The `camera/support-DEVICE.json` files identify capable devices in the session.
No buffer contents are written without an explicit request:

```sh
python3 tools/request-camera.py /private/discovery/session --duration-ms 5000 --samples 256
python3 tools/analyze-camera.py /private/discovery/session/camera/samples-DEVICE-ID.jsonl \
  --output /private/matrices.json
```

The request tool defaults to the presenting device found in the inventory and
requires a live process. It atomically replaces `camera/request.json`; request
IDs prevent replay. More windows can be requested without restarting the game.
If the game stops making Vulkan calls, the window's end record is written when
calls resume; it will not capture after its deadline. The analyzer interprets
matrix offsets 0, 64, 128, 192, 272, 336 and 400 as column-major based on the
previous SPIR-V inspection and retains `camera_verified=false`.

Validation covers exact sampled bytes with nonzero memory-binding, mapping and
descriptor offsets, primary/secondary commands, QueueSubmit/QueueSubmit2, unmap,
command-pool reset and operation after the overall inventory log fills. A separate
state test covers reuse of memory/buffer/set handles, invalid pointers, a real
partially inaccessible guarded mapping, recovery after read failures,
noncoherent mappings and partial mapping bounds. The command-dispatch fallback
is also exercised with an unwritable discovery directory. The pipe fallback
also permits this test in sandboxes denying the self-process read syscall:

```sh
build/discovery/camera-state-test /private/new-empty-test-directory
```

The first RDR2 gameplay window completed but returned zero samples and 183,446
CPU mapping read failures. The old log did not record errno, so the exact game
failure cannot be established from it. An isolated reproduction on the RTX 5090,
both on the host and inside SteamLinuxRuntime_4, found that memory type 4
(HOST_VISIBLE | HOST_COHERENT | DEVICE_LOCAL) returns EFAULT from
`process_vm_readv`, while ordinary host-visible types 2/3 succeed. All types
return the expected 464 bytes through the pipe reader. See
`test_layer/mapping_read_probe.cpp` for the standalone owned-allocation test.
The full capture regression now runs on both ordinary and device-local coherent
allocations, checking exact bytes, Vulkan validation and failure recovery.
This fixes the independently reproduced reader limitation; a new gameplay
window is still needed to verify RDR2 camera values and their meaning.

`DLSSFG_METADATA_LIMIT_MIB=1..64` can lower the inventory limit for this regression
(default 64). It does not disable the separate camera probe. Real RDR2 snapshots
still require restarting once with this updated library, then arming a window.
The helper, NR settings, game profile and FG presentation remain unchanged.

### RDR2 CPU camera windows after the reader fix

Three requested gameplay windows now returned 222, 1,024 and 912 samples,
respectively. All used the pipe fallback after `process_vm_readv` returned
EFAULT, and all were associated with successful CPU queue submission returns.
There were no CPU memory-read failures. These observations still do not establish
GPU completion, shader consumption or consistent per-frame data.

The 464-byte blocks mix object and view data. Among perspective-shaped blocks,
the matrix at byte 64 is orthonormal, and byte 192 is its exact transpose.
Relative rotation spans approximately 0.105 degrees in the requested idle
window, 78.08 degrees while turning, and 0.317 degrees while walking. The matrix
at byte 128 implies an aspect ratio of about 2.38889 and vertical field of view
of 51.282 degrees, consistent with the 3440x1440 display. Its depth coefficients
change during the turn: a fixed near-plane assumption would be incorrect.
The vector at byte 256 varies with movement, but its coordinate system and sign
are unverified. Byte 0 varies across objects, so treating the whole first matrix
as a world-to-view camera matrix would be incorrect.

Byte offsets 272/336/400 resemble temporal counterparts, but they are not yet
matched to a known preceding frame. Do not infer motion-vector conventions or
clip-to-previous-clip matrices from these observations. Exact layout links still
reference shader hash
`f41986d573368f56720f8f14139359a3b316652ea6cee2ede5f30506c14e760a`;
they do not prove that a particular snapshot was consumed by that shader.

Private raw captures and comparison reports remain outside published sources.
Reproduce the bounded mathematical analysis with:

```sh
python3 tools/compare-camera.py baseline=/private/baseline.jsonl \
  turn=/private/turn.jsonl walk=/private/walk.jsonl --output /private/comparison.json
PYTHONDONTWRITEBYTECODE=1 python3 tools/test-compare-camera.py
```

The comparison groups by projection shape and checks matrix transpose/rotation
relations; it never enables a profile. RDR2's profile remains disabled with
status `camera_candidates_captured_unverified`. Verified shader/draw association,
camera translation and temporal pairing, plus depth and motion image captures,
remain necessary before connecting the game to FG.

### Requested draw associations

The next diagnostic layer keeps shader hashes, pipeline/layout lifetimes and
graphics command state independently of the inventory log budget. A camera
window can now require a recorded draw association:

```sh
python3 tools/request-camera.py /private/discovery/session --require-draw \
  --duration-ms 5000 --samples 256
```

This requires a fresh launch with the updated library. The support file reports
whether the mode is available; an older layer is rejected by the request tool.
Each sample retains `draw_links`, also passed through by `analyze-camera.py`.
Links include the pipeline's stage hashes/entry points, whether specialization
was supplied, command/pipeline/set lifetime generations, descriptor revision,
draw kind and bounded ordinal examples. No raw game shaders are published.
Same-set image descriptor examples include view/image generations, format,
extent, usage and subresource range. They are **not** GPU image contents or a
claim that a particular shader actually sampled that binding.

The tracker distinguishes binding from drawing. It records legacy direct,
indexed and indirect draw calls; indirect counts remain unknown. Zero direct
draws are excluded. Legacy and maintenance6 descriptor binds are observed.
It requires exact pipeline-layout lifetime identity, conservatively dropping
compatible-but-distinct layouts and dynamic-offset binds. Descriptor writes,
copies or templates after recording invalidate older associations by revision.
Push-descriptor calls clear the tracked set binding. Secondary commands are
traversed only when referenced by a submitted primary, with generation checks;
no graphics binding state is assumed after executing secondary commands.

These conservative choices follow Vulkan's
[descriptor layout compatibility rules](https://docs.vulkan.org/spec/latest/chapters/descriptorsets.html)
and [secondary command buffer state rules](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdExecuteCommands.html).
Draw associations are disabled when shader objects, descriptor buffers,
device-generated commands or command-buffer inheritance extensions are enabled.
Other unobserved draw forms are omitted. Graphics pipeline libraries without
resolvable shader stages and inline modules are unresolved. This is a diagnostic
subset, not a complete Vulkan execution trace.

The tracker retains at most 1,024 distinct association examples per command
buffer and 262,144 globally, 4 link examples per set per sampled submission, and
16 single-element image bindings per set. Array/spill descriptors and other set
indices are not reconstructed. Repeated secondary executions and GPU ordering
are not reconstructed. The request end record includes lifetime CPU draw
counters to distinguish missing pipelines/sets from layout mismatches. Output
byte-limit exhaustion ends the request cleanly; it does not disable the probe.

Validation uses an owned vertex shader (`test_layer/camera_draw.vert`) that reads
binding 29, actual graphics draws in primary/secondary commands, device-local
coherent memory and Vulkan validation. It checks the exact expected shader hash
even after shader-module destruction and after the metadata log fills. State
tests cover descriptor revisions, handle reuse, image/view lifetime checks,
layout mismatch, dynamic offsets, zero draws, secondary resets and unsupported
mode. Diagnostic I/O failure must still forward the real graphics draw normally.

The first game draw-associated window returned 234 samples, all linked to
recorded draws and successful CPU submission results. The native mode was
available on the game's presenting device; unsupported extensions disabled it
on auxiliary devices as intended. Camera/depth/motion frame identity and image
preservation, NGX game integration and FG presentation remain unimplemented;
the RDR2 FG profile remains disabled.

Static analysis of the observed vertex shader
`2004b4b6a04e99218882922aa276f7c8fa72c4e12c66bbf4f33fd9eca02d226f`
confirms that matrices at byte offsets 128, 64 and 0 multiply the input position
in that left-to-right order before BuiltIn Position. A second chain at 400,
336 and 272 contributes to output location 8, with additional scaling using
other resources. This is static dataflow evidence, not established temporal
pairing or motion-vector encoding. The paired fragment shader did not fit in
the initial 128 MiB dump. The diagnostic Steam wrapper now defaults to a bounded
512 MiB shader budget; explicit `DLSSFG_SHADER_LIMIT_MIB` overrides still win.
Research-only JSON evidence lives in `profiles/research/`; the profile loader
does not use it, and it contains neither game shader binaries nor process addresses.

### Render targets at recorded draws

The next layer records color/depth attachment references for legacy render
passes, RenderPass2 and dynamic rendering. It tracks framebuffer/view/image
generations, imageless attachment selection, subpass transitions and the scope
active at a draw. Secondary legacy render passes resolve against the primary's
scope recorded at execution, requiring exact render-pass lifetime/subpass and
matching framebuffer if inheritance specified one. Compatible-but-distinct
render passes and dynamic-rendering inheritance are conservatively unresolved.

Each draw link now includes `render_targets`: render area, subpass, declaration
layouts/load/store operations, image/view generations, formats, extents, usage
and subresource ranges. Destroyed/reused resources are marked stale or invalid.
These are references, **not pixel captures, current image layouts, completed GPU
writes or a resource history**. Resolve/input/preserve attachments are omitted.
For combined depth/stencil attachments, load/store describes the depth aspect.
Dynamic color attachment indices do not establish shader output locations under
optional location remapping. Repeated secondary executions remain bounded
examples rather than a complete ordered trace.

State tests cover subpass changes, scope termination, imageless/inherited/dynamic
targets and framebuffer/view reuse. Real GPU tests exercise actual RG16F color
and D32 depth attachments in primary/secondary commands with both RenderPass and
RenderPass2, inside Steam Runtime with Vulkan Validation Layers. Dynamic target
resolution currently has state-test coverage only. Gameplay target selection,
motion encoding and depth readback are still required.

The expanded shader dump contains the paired fragment shader
`e2a7fb68d2df93d2f1aefae1d6e862e6f903eb5c6a0d14c1b6060658d1ed1e58`.
Static dataflow shows a two-component output at location 4: input location 8
XY divided by W, plus half of storage set 0 binding 98 element 0 byte offset
24 XY, minus the current fragment coordinate. The vertex stage scales that
input by the same resource and flips Y; the fragment origin is upper-left.
This identifies a pixel-space displacement candidate. The resource's actual
value, previous-frame identity, jitter convention and image contents are still
unverified. The shader binaries and decompiled sources remain private build
artifacts; only hashes and structural research evidence are recorded here.

### RDR2 target evidence and NGX integration direction

Two completed scene windows contain 532 successful CPU submission samples.
Five draw links with the researched vertex/fragment pair, at five distinct CPU
present markers, reference the same framebuffer and resource generations.
Color slot 4 is RG16F and depth is D32F+S8, both 2293x960. Both declare STORE and
TRANSFER_SRC usage. These are recorded references, not readback or proof of
GPU completion; final layout, queue ownership and copy timing remain unresolved.
`tools/analyze-render-targets.py` reproducibly selects that exact legacy pass
signature, rejects stale/partial/failed observations, and keeps process sessions
separate. Reports with session identities stay private under `build/`.

The preferred next interception point is the game's **existing Vulkan NGX DLSS
EvaluateFeature call**, rather than extending heuristic attachment capture first.
The running process loads the driver's `nvngx.dll` and `_nvngx.dll`, plus the
application's `nvngx_dlss.dll`. The loader exports the Vulkan evaluate entry point.
Loading alone does not prove which entry point is called. The NGX observer has
now confirmed 32 successful SuperSampling evaluations in a loaded RDR2 scene,
paired with the intercepted feature creation. Address-free evidence is recorded
in `profiles/research/rdr2-ngx-sr-evidence.json`.
The [official Vulkan DLSS helper](https://github.com/NVIDIA/DLSS/blob/main/include/nvsdk_ngx_helpers_vk.h)
passes named color/output/depth/motion resources, jitter, motion scale and reset
through the parameter object. Observe inputs before forwarding the real call;
output availability is GPU-ordered after the recorded DLSS work, not simply after
the CPU function returns. Keep native resource ownership and Wine handle wrapping
explicit. An application-local diagnostic proxy is now implemented; see
`ngx_capture/README.md` for tests, scope, installation and remaining limitations.

This route requires the game's DLSS path to execute. It does not supply every FG
camera constant automatically, nor solve HUD composition, tone mapping or paced
presentation. Existing shader/camera evidence remains useful for missing data
and corroboration. Optical flow remains the intended fallback for unavailable
engine motion, not a replacement for valid depth/camera/frame identity.

The observed NGX inputs are RGBA16F color, D32F+S8 depth and RG16F motion at
2293x960, with RGBA16F SR output at 3440x1440. There is an R32F 1x1 exposure
texture and an R8 bias-current-color mask. Create flags declare HDR input,
low-resolution motion and inverted depth. NR before SR must preserve that HDR
and exposure contract even with an SDR display. Both queried optional camera
matrices are absent; frame-time is reported as zero. Actual pixels, camera
semantics, GPU resource transfer and paced FG presentation remain unverified.
The runtime profile remains disabled. The expensive draw discovery stays off.
