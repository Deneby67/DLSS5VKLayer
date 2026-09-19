# Experimental RDR2 NR before SR

The active experiment is **DLSS 5 NR → the game's existing DLSS SR**. FG work is
deferred. Nothing creates generated frames or replaces the game's presentation.

**Runtime status, 2026-09-19: native-loader startup reached a real RDR2 scene;
NR bypassed safely.** The user reached a scene with the corrected native loader
chain. Arming NR reported an unavailable device context and left original SR
unchanged. A read-only snapshot showed all tracked command buffers belonged to
a device missing from the device table; the separately observed device had BDA
and one queue in family 0. NR was disarmed again and the user closed the game.
No NR-before-SR gameplay or visual improvement has been demonstrated yet.

The new candidate also tracks core/KHR physical-device-group enumeration and
removes mappings on instance destruction. An isolated groups-only enumeration
test reproduces the same bypass with the previous installed shim (zero Color
replacements); the corrected shim processes three frames. This establishes a
real coverage bug, while actual RDR2 use of that enumeration path still needs a
new launch to confirm. Device creation now records bounded per-creation context
and queue information, and bypass reasons distinguish missing device, BDA and
queue topology. No handle aliases or relaxed queue checks are introduced.

The shim preserves the game's native prefix loader using a private, hash-pinned
sibling copy, `dlssnr_system_vulkan.dll`; the prefix stays untouched. The two old
installers remain blocked. Use only `tools/install-nr-native.py` for this
candidate, which starts with NR disarmed and provides exact rollback.

The NGX observer dispatches known SuperSampling feature evaluations to an
application-local Vulkan forwarding shim. The shim records HDR encoding, NR,
HDR recomposition and a shader-read barrier into the same command buffer,
before forwarding the original SR evaluation exactly once. A 17-slot MSVC NGX
parameter overlay replaces only Color lookups. Original depth, motion, jitter,
exposure and other getters continue to read the game's parameter object.

The shim preserves the native loader's named exports and ordinals, forwarding
to its renamed sibling and wrapping device/command-buffer lifetime and
proc-address lookups. All handles stay in the same Windows Vulkan namespace. It does not send
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
# Additional native startup/arming gates are listed below.
python3 tools/install-nr-native.py --dry-run
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

## Native-loader installation and live control

Private test artifacts and proprietary DLLs remain outside Git. The installer
requires the pinned game executable, native loader, model, actual SR DLL and
matching successful validation artifacts for the exact adapter and fixtures:
`native-baseline`, `bootstrap-wsi`, `bootstrap-forward`, `bootstrap-track`,
`bootstrap-bda`, `launcher`, `disabled`, `missing-dll`, `bda-auto`, `armed-cycle`,
`real-sr`, `groups-core`, `groups-khr`, plus all six NGX observer gates.
All thirteen gates passed after the groups change with matching binary hashes;
the actual NR → SR fixture completed three frames without Vulkan
validation errors. The arming fixture verified missing and stale requests leave
pixels unchanged without loading NR, valid activation changes pixels, and
removing the request returns to unchanged forwarding.

```sh
python3 tools/run-nr-inline-probe.py --inline --validation --case native-baseline
python3 tools/run-nr-inline-probe.py --inline --validation --case armed-cycle
python3 tools/run-nr-inline-probe.py --inline --validation --case groups-core
python3 tools/run-nr-inline-probe.py --inline --validation --case groups-khr
python3 tools/install-nr-native.py --dry-run
python3 tools/install-nr-native.py
# After restart and an SR evaluation in a loaded scene:
python3 tools/control-nr-inline.py ~/.local/state/dlssnr/native-inline/run-XXXXXXXX/adapter.log status
python3 tools/control-nr-inline.py ~/.local/state/dlssnr/native-inline/run-XXXXXXXX/adapter.log on
python3 tools/control-nr-inline.py ~/.local/state/dlssnr/native-inline/run-XXXXXXXX/adapter.log off
```

The installer backs up every replaced file and writes a hash-checked restore
script before changes. Unknown existing Vulkan DLLs are never overwritten.
Stable Proton, the prefix loader and Steam launch options stay unchanged. New
DLLs take effect on a normal user-controlled restart. The emitted backup's
`restore.py` restores the exact previous working observer and launch wrapper.

The wrapper disables presentation NR, draw discovery and RenderDoc. Device
tracking and supported BDA augmentation run at startup; NR remains off until a
request matches the current Windows PID and session token. Each launch gets a
private log/control directory. The controller additionally verifies that one
live RDR2 process has the matching environment and holds that log open. Requests
are checked during SR evaluation at most every 250 ms. Removing the request or
selecting `off` stops NR recording, retaining resources for already queued work
until device teardown. This control cannot recover a blocked render thread.

`recorded NR-before-SR calls` reports CPU recording, not measured GPU completion
or confirmed visual quality. Successful RDR2 startup, scene stability and a
quality/performance comparison remain required.

## Historical startup isolation (superseded direct-winevulkan candidates)

`tools/install-nr-bootstrap.py` prepares a **forward-only** diagnostic independently
of the blocked inline installer. It keeps the confirmed working NGX observer and
Proton, backs up the launch wrapper, installs the tested Vulkan DLL, forces
`DLSSNR_INLINE=0` and selects `DLSSNR_BOOTSTRAP=forward`. `DlssNrEvaluate` also
unconditionally bypasses NR in every bootstrap mode, even if invoked by a caller.
Device creation receives the original pointers and values; no BDA augmentation
or command tracking runs in forward mode. Presentation NR is disabled during
this startup diagnostic. Each launch gets its own private log directory.

The other isolated fixture modes are `bootstrap-track` (device/command tracking
only) and `bootstrap-bda` (tracking plus BDA augmentation); neither runs NR.
They are not selected by the diagnostic installer. The old inline installation
block remains in place. Forward-only startup success will not prove the full
NR path works, nor identify BDA or tracking as the cause without another test.

A direct exported `vkCreateInstance` originally bypassed the shim's delayed
user32 initialization. The first diagnostic routed it through resolution, but
that still initialized user32 on the first Vulkan caller's thread. The current
candidate imports USER32.dll/GetDpiForSystem statically and calls it on process
attach, matching Wine's builtin vulkan-1 DllMain. Resolution no longer loads or
initializes user32 lazily. This closes an initialization-order difference,
**not a confirmed explanation of the RDR2 hang**.
The new `bootstrap-wsi` fixture creates an actual Win32 window using that direct
export before GIPA, creates a swapchain, and presents three frames. Together
with the existing GIPA-based HDR fixture it covers both entry routes.

Startup tracing records only the first 32 calls per API, with thread, timestamp,
entry and result, including instance/device creation, surface/swapchain,
submission, waits and presentation. It does not capture images, intercept draws,
suspend threads or insert GPU waits. The isolated WSI test waits for its own
queue between frames. Use `tools/analyze-nr-bootstrap.py LOG` to inspect pending
entries and completed calls. Silence after the limit is not evidence of a hang.

```sh
python3 tools/run-nr-inline-probe.py --inline --validation --case bootstrap-forward
python3 tools/run-nr-inline-probe.py --inline --validation --case bootstrap-track
python3 tools/run-nr-inline-probe.py --inline --validation --case bootstrap-bda
python3 tools/run-nr-inline-probe.py --inline --validation --case bootstrap-wsi
python3 tools/run-nr-inline-probe.py --inline --validation --case launcher
python3 tools/run-nr-inline-probe.py --inline --validation --case disabled
python3 tools/install-nr-bootstrap.py --dry-run
```

Installation requires matching hashes for all six successful validation gates,
the known game executable, working observer and recorded launch wrapper. The
rollback script is written before changes and refuses to overwrite later edits.
A real RDR2 restart and menu check is still required for this diagnostic build.

### Launcher startup observation

The first two diagnostic launches stopped before RDR2/PlayRDR2. A live debugger
snapshot of Proton's steam.exe showed both threads waiting on win32u's
`display_lock`; its owner was the main thread, which was itself blocked trying
to acquire it. The application-local Vulkan DLL and winevulkan PE module were
not mapped. The previous native DLSSNR Vulkan layer was mapped, alongside the
Steam overlay. This identifies a display initialization self-deadlock, but not
its trigger or a causal link to either layer. Private stacks remain under build/.
The diagnostic launch now disables presentation NR as well (`VKLayer_DLSS5=0`,
`DLSSNR_ENABLE=0`) to isolate that path on the next user-controlled restart.
The previous working launch can still be restored exactly using its backup.

### Live game capture after disabling presentation NR

RDR2 reached Vulkan device/swapchain creation and presented a frame with forward
mode and both NR paths disabled. A later present returned
`VK_ERROR_OUT_OF_DATE_KHR`; all captured Vulkan calls had returned. That return
code alone does not explain the hang. Three Linux stack snapshots were saved,
and read-only inspection of the saved Windows syscall contexts and PE unwind
metadata located the main thread waiting in RtlEnterCriticalSection on a game
lock. Its owner was another game thread waiting in WaitForSingleObject. The
Wine display_lock was free. Thus the earlier steam.exe display self-deadlock did
not recur, and this game-level wait must not be conflated with it. The startup
cause remains unresolved; NR and BDA augmentation were not active. The next
candidate changes only user32 initialization order, with the inline NR installer
still blocked. Raw game addresses/stacks stay private in build/.

### Process-attach candidate result and rollback

Matching builtin user32 initialization did not fix RDR2 startup: the next live
run again presented one frame, returned OUT_OF_DATE on a subsequent present,
and remained in a game-level wait with display_lock free. Three further stacks
were saved. No NR or BDA augmentation ran. The application-local Vulkan DLL was
removed and launch selection changed to builtin Vulkan (`vulkan-1=b`), retaining
the working observer and disabling both NR paths. The already-running process
still mapped the deleted experimental file; only a full game restart applies
this rollback. The separate bootstrap installer is now blocked as well. A fresh
menu check is pending; no successful NR-before-SR gameplay result is claimed.

### Working control and loader identity (confirmed)

The user confirmed working startup after removing the application-local DLL.
Inspection of the live process showed **the native Windows Vulkan loader from
the game's prefix**, not Wine's builtin vulkan-1. Proton's `nativevulkanloader`
compatibility setting appends `vulkan-1=n` after the wrapper's attempted builtin
override, so the final override wins. The prefix DLL is 1,006,904 bytes, SHA-256
`9de5d9a7a1c14152bac98318c63d540520da16b8826bf96a76bef90a5d223906`;
Proton's builtin DLL is 53,248 bytes, SHA-256
`47e1f9fe11a05b7aaa21133272f49eede4ed6baf2635eeb80c45705f4763107f`.

**Architecture mismatch:** the experimental shim forwarded directly to winevulkan
and its synthetic fixtures tested that route. It therefore bypassed the native
loader used by the working RDR2 configuration. The successful standalone tests
were not testing the same loader chain. This is a verified discrepancy and a
strong investigation lead, not proof of every step causing the game's lock wait.
Any replacement must preserve the actual native loader chain and validate it
in the isolated fixtures before another installation. Repeating the direct
winevulkan-forwarding experiment is not an appropriate next test.

The working control retains the previous NGX observer and Stable Proton. Both
presentation NR and inline NR remain disabled. NR-before-SR gameplay is still
unimplemented as a validated working integration. The diagnostic record's old
`builtin_vulkan...` label describes the intended override, **not the observed
runtime loader**, and must not be used as evidence of builtin Vulkan loading.
