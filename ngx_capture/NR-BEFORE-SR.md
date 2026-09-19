# Experimental RDR2 NR before SR

The active experiment is **DLSS 5 NR → the game's existing DLSS SR**. FG work is
deferred. Nothing creates generated frames or replaces the game's presentation.

**Runtime status, 2026-09-19: rolled back.** RDR2 hung before the main menu with
the Vulkan shim installed. Its journal reached successful BDA-enabled device
creation, and the NGX observer attached all four hooks, but no SR creation or
NR processing was recorded. The process had already exited when inspected, so
no blocked-thread stack was available. The exact cause is unresolved; the
offscreen tests below do not establish startup compatibility with the game.
The previous observer and launch wrapper were restored and the application-local
Vulkan DLL/inline selection marker removed. Reinstallation through the installer
is disabled until startup has been diagnosed and validated.

The NGX observer dispatches known SuperSampling feature evaluations to an
application-local Vulkan forwarding shim. The shim records HDR encoding, NR,
HDR recomposition and a shader-read barrier into the same command buffer,
before forwarding the original SR evaluation exactly once. A 17-slot MSVC NGX
parameter overlay replaces only Color lookups. Original depth, motion, jitter,
exposure and other getters continue to read the game's parameter object.

The shim forwards the selected Proton's complete vulkan-1 export surface to
winevulkan, wrapping only device/command-buffer lifetime and proc-address
lookups. All handles stay in Wine's Windows Vulkan namespace. It does not send
pixels to the external helper, read them on the CPU, submit work on another
queue, intercept draws or wait for GPU completion during normal evaluation.
Device destruction waits before releasing the experiment's own resources.

## Color contract

The observed RDR2 input is pre-exposed RGBA16F HDR even with SDR display output.
The existing NR invocation accepted float images but clipped synthetic values
above white to 1. Passing that output directly to SR lost highlights. The new
bridge therefore exposes color using the supplied 1x1 exposure texture and
pre-exposure parameter, compresses the positive RGB peak with `x/(1+peak)`, and
encodes an sRGB proxy for NR. Both proxy and NR output use RGBA16F.

Recomposition carries the model's linear-color difference back onto the
original HDR signal using the original compression scale. It preserves alpha,
retains negative source channels in the baseline and limits edit amplification;
it is a bounded residual composition, not an unbounded inverse tone map. An
unchanged proxy/model pair reproduces the tested HDR source bit-for-bit. This
does not establish artistic correctness, exposure stability or motion quality
in RDR2; those require an actual scene comparison.

NR uses the game's RG16F motion at scale 1 and real jitter/reset. Depth is
currently omitted from NR's optional input, not replaced by a fabricated plane.
The SR invocation retains the game's real depth. There is no camera requirement
for this path and no new optical-flow estimation. Current NR tuning is one pass
with the adapter defaults, independent of the external helper GUI.

## Selection and lifetime limits

Only RDR2.exe with `DLSSNR_INLINE=1` processes frames; the installer pins the
researched executable SHA-256. Inputs must match the observed HDR, inverted-depth,
unjittered low-resolution motion contract, formats, full extents, zero input
subrect origins and finite scalar values. Unknown inputs leave SR unchanged.

One known SR feature, one graphics queue family with one enabled queue, one NR
resolution and one device are supported per process. Changed feature/size/device
disables NR until restart. Resources are retained until device teardown so
already-recorded commands cannot reference freed intermediates. At most 64
command-buffer descriptor pairs are retained. Only the first eligible evaluation
in each non-simultaneous primary command recording is replaced; descriptor sets
are updated only after a new recording begins. Queued work on that single queue
is ordered with barriers around shared intermediates.

NR needs `bufferDeviceAddress`. If absent from the game's feature declarations,
the shim queries support and adds the KHR feature/extension without modifying
the application's structures. An explicit existing false declaration is
respected. A failed augmented device creation retries the original configuration.
Unsupported capabilities disable NR. The driver must still support the existing
NGX NVX device extensions used by SR.

## Build and gates

```sh
bash tools/build-nr-inline.sh
python3 tools/run-nr-inline-probe.py --bridge --validation
python3 tools/run-nr-inline-probe.py --inline --validation --case active
python3 tools/run-nr-inline-probe.py --inline --validation --case launcher
python3 tools/run-nr-inline-probe.py --inline --validation --case disabled
python3 tools/run-nr-inline-probe.py --inline --validation --case missing-dll
python3 tools/run-nr-inline-probe.py --inline --validation --case bda-auto
python3 tools/run-nr-inline-probe.py --inline --validation --case real-sr
bash tools/build-ngx-capture.sh
python3 tools/test-ngx-capture.py
# Installation is currently blocked by the known game-startup regression.
```

Builds use at most 24 linker workers. Tests use a separate prefix and synthetic
images. The inline forwarding fixture copies the received Color to a readback
target in place of SR; it checks actual modified HDR pixels, alpha, parameter
immutability, exact call count, callback/LastError forwarding and validation.
Launcher, disabled and missing-model cases must yield unchanged pixels.
`--case real-sr` additionally exercises the game's SR DLL on the isolated scene;
its result must be reported separately from the forwarding fixture.

On RTX 5090 / driver 615.71.09 / custom Stable Proton, all six inline cases
passed with zero validation errors. The real SR case created SuperSampling and
completed three NR-preprocessed evaluations from 1280×720 to 1920×1080, with
finite HDR output. The bridge gate also preserved all synthetic pixels exactly
when the model edit was replaced by an identity. These are offscreen synthetic
checks, not a successful RDR2 gameplay run or a quality/performance benchmark.

Tested NR DLL SHA-256:
`e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e`.
Tested game SR DLL SHA-256:
`3975567b8943c53acce397f2b72380092f84f162d00b0d2c7d08a1025c563983`.

Private test artifacts and NVIDIA DLLs remain outside Git. Once startup is fixed,
installation requires
matching successful validation artifacts and backs up every replaced file,
including the previous NGX observer, wrapper and selection markers. Unknown
existing Vulkan DLLs are never overwritten. The selected Proton is unchanged.

The Steam wrapper disables the old presentation-time NR and draw discovery when
inline NR is selected. The separate helper may remain open, but does not process
this game's presentation. On a normal game restart the new libraries take effect.
`DLSSNR_INLINE=0` before the wrapper disables the new selection; the emitted
backup `restore.py` restores the exact previous installation. The journal is
`~/.local/state/dlssnr/nr-inline.log`. `recorded NR-before-SR calls` reports CPU
recording, not measured GPU completion or confirmed visual quality in the game.
