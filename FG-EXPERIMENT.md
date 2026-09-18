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
machine. Use a normal Steam launch for the next gameplay test; no RDR2 capture or
validated game profile is claimed here.
