# SDR FG ×2 development status

This branch is an **experimental backend and profile foundation**, not working
in-game frame generation. NR and the Rockstar Social Club renderer exclusion
remain available. No NVIDIA binaries are included in the repository.

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
No command recording, resource contents, barriers or submissions are intercepted
by this recorder. RenderPass2, dynamic rendering, descriptor update templates,
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
