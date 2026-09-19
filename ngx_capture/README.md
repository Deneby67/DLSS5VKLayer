# RDR2 NGX observer

By default this is a diagnostic **observer**. The active experiment now adds
**DLSS 5 NR → the game's DLSS SR** through an explicitly selected Vulkan shim;
see [NR-BEFORE-SR.md](NR-BEFORE-SR.md). FG is deferred. The new path has isolated
GPU validation, including the game's actual SR DLL, but hung RDR2 before its
main menu. It has been rolled back and reinstallation is disabled pending startup
diagnosis. An SDR display does not imply that the pre-SR color is SDR.

The application-local `version.dll` forwards all 16 exports to a local copy of
the selected Proton's original version library, `dlssfg_system_version.dll`.
On process attach it starts a bounded observer only when the executable basename
is exactly `RDR2.exe` (case insensitive) and `DLSSFG_NGX_CAPTURE_DIR` is an absolute
Windows drive path. The worker runs outside the loader lock, waits at most two
minutes for `nvngx.dll` / `_nvngx.dll`, and exits after attachment. It validates
the complete x64 indirect-export stub shape, initialization gate and module-local,
aligned pointer slots. A slot is replaced atomically only if its current target
is exactly the matching export of the loaded `_nvngx.dll`. Unknown layouts and
preexisting redirects are left untouched and reported as an attachment timeout.

Only Vulkan NGX CreateFeature, CreateFeature1, EvaluateFeature and ReleaseFeature
dispatch pointers are wrapped. No driver/Proton files, code bytes or game imports
are changed. This avoids RDR2's startup import rewriting, which defeated an
initial IAT-based observer despite passing isolated tests. The DLL must remain
loaded for the process lifetime; the application-local version import provides
that lifetime. Every wrapper forwards original arguments exactly once, outside
diagnostic exception guards and locks. Callback pointers, return values and
LastError are preserved. Direct calls to `_nvngx.dll` or feature DLLs bypassing
the outer loader are outside this diagnostic subset.

When inline NR is explicitly enabled, a known SR evaluation is delegated to the
tested shim, which forwards SR with a Color-only overlay after recording NR.
The default observer tests still require unchanged pointers and parameters;
separate GPU gates validate the opt-in replacement and bypass behavior.

A feature created before attachment has an unknown feature ID/generation. Its
evaluation can still be observed, but is not labelled as verified SR. No feature
identity or creation flags are guessed. Attachment events, actual evaluation
records and successful results must be checked in the game, not inferred merely
from the DLL being mapped.

## Observations

Creation records keep a bounded feature-handle generation map. Evaluations are
observed only after an explicit request; idle evaluations poll the small request
file at most four times per second. Each window is limited to 256 evaluations /
30 seconds, and each process log is capped at 8 MiB. Failure to open/write logs
does not prevent the real DLSS call. The application's own NGX exceptions are
not caught. Short descriptor reads and faults in diagnostic getters are rejected.

The observer reads the 17-slot **MSVC** NGX parameter ABI explicitly, avoiding
MinGW's different overload ordering. It only calls Get and never Set or Reset.
Named fields fall back to SDK encoded aliases where available. Missing fields
retain the NGX error result; there are no invented depth/camera/jitter defaults.

Captured metadata includes named Color/Output/Depth/MotionVectors descriptors,
dimensions/subrects, feature flags, jitter, reset, motion scale/offset, exposure,
frame duration and optional inverse-view-projection / clip-to-previous matrices.
Resource structs and optional matrix arrays are copied with fault-contained CPU
reads. Image pixels are not read back. Handles belong to the **Windows NGX
caller's namespace**; do not assume they are native Linux Vulkan handles. A
successful evaluate return means recording succeeded, not GPU work completion.
The private logs contain process-local identities and must not be published.

## Build, verify, install

```sh
bash tools/build-ngx-capture.sh
python3 tools/test-ngx-capture.py
python3 tools/install-ngx-capture.py
```

Builds use Clang/LLD and at most 24 workers. Tests use their own Proton prefix.
They exercise real forwarded version exports, a synthetic indirect loader table,
exact NGX arguments/callbacks and
failures with a synthetic backend, resource and parameter fault rejection,
handle recreation, idle behavior and launcher/disabled exclusion. A separate
case allocates a parameter object from the **real installed NVIDIA loader**,
round-trips scalars and resource descriptors, and destroys it through NVIDIA's
API, and verifies all four dispatch hooks attach to that real loader. These tests
do not establish real DLSS GPU evaluation or RDR2 compatibility.

On the development host, a 100,000-call synthetic microbenchmark measured about
72 ns/evaluate through the idle observer vs 15 ns for its trivial unwrapped fake
backend. This is not a game FPS or GPU-performance measurement.

The installer requires matching passing test artifacts, refuses unknown existing
game DLLs, backs up every changed selection file and emits a rollback script.
It installs no fake NGX DLL. It disables the old always-on draw-discovery marker:
the user reported approximately 20 FPS with that diagnostic layer. The new
Steam wrapper prevents that tracker from being enabled alongside NGX observation.
NR settings and the selected Proton are retained. Changes apply after a normal
game restart. `DLSSFG_NGX_CAPTURE=0` disables observation on the next launch.

Once in a real scene with the game's DLSS enabled:

```sh
python3 tools/request-ngx-capture.py ~/.local/state/dlssnr/ngx/ngx-WINDOWS_PID-TICK.jsonl --samples 32 --duration-ms 5000
```

Logs appear below `~/.local/state/dlssnr/ngx/`, with one process-specific control
file. Select the current log; the request tool verifies that a live RDR2 process
holds it open. Windows process IDs in logs differ from Linux `/proc` IDs.
`mode=observe_only` means no processing stage has been inserted. Opt-in inline
NR records `mode=nr_before_sr_requested`; the separate NR journal reports whether
replacement was actually recorded or why it was bypassed. The original helper
keeps its previous presentation path unless inline NR is selected by the wrapper.

## Verified RDR2 scene

The corrected dispatcher observer intercepted all four entry points in RDR2.
A completed 32-call window links every evaluation to a successful creation of
feature 1 (SuperSampling); all 32 evaluations returned success. Evidence without
process addresses is stored in `profiles/research/rdr2-ngx-sr-evidence.json`.

| NGX input/output | Observed descriptor |
| --- | --- |
| Color | RGBA16F, 2293×960 |
| Depth | D32F+S8, 2293×960 |
| MotionVectors | RG16F, 2293×960 |
| ExposureTexture | R32F, 1×1 |
| Bias-current-color mask | R8 UNORM, 2293×960 |
| Output | RGBA16F, 3440×1440 |

Create flags `0x2b` declare HDR color, low-resolution motion, inverted depth and
sharpening; the motion-jitter flag is clear. Motion scales are both 1, but this
does not independently verify raw texel units or temporal direction. Jitter
varies between evaluations, reset is zero, all observed subrect origins are zero,
and pre-exposure is 1. The actual exposure texel has not been read. The frame-time
parameter is present but zero, so cannot serve as our timing source.

`InvViewProjectionMatrix` and `ClipToPrevClipMatrix` return
`NVSDK_NGX_Result_FAIL_UnsupportedParameter` (`0xbad00010`); no values were obtained
through these getters. Missing camera data still requires a separate validated engine
path. HDR input/exposure must be preserved by NR before SR; the final SDR output
restriction does not permit treating the game's pre-SR buffer as SDR.

Reproduce the metadata analysis with:

```sh
python3 tools/test-ngx-analysis.py
python3 tools/analyze-ngx-capture.py /path/to/current-ngx-log.jsonl --output build/ngx-capture/scene.json
```

The analyzer rejects incomplete/mixed windows, duplicate results and evaluations
without a matching known SR creation. Only successful paired evaluations enter
resource summaries. Its explicit field allowlist excludes handles and addresses.
None of this proves GPU contents, completion or a working FG presentation path.
The user confirmed ordinary gameplay FPS with the corrected hooks attached;
the older draw tracker remains disabled. This is a subjective gameplay check,
not a controlled benchmark of observer overhead.

### In-game NR controls (Rendering and F2)

The native-loader adapter reads **Rendering → Neural rendering** directly from
its GUI mapping (`DLSSNR_INLINE_SHM`, set by the installer). Enabled, style,
preset, intensity, local structure/tone, skin structure, auto skin mask and
sharpness apply to NR before SR. The mapping is opened existing, header-only;
no pixel transport or external helper is started. Cost, per-pass settings,
model resolution, Quality and Composition still belong to the external helper.
The inline path runs one pass at the game's native DLSS input resolution.

F2 toggles the current session when the game owns the foreground window; holding
F2 does not repeat. The game resolves already-loaded user32 exports during SR
evaluation, without adding loader-time dependencies. It atomically updates the
PID/token arm request and the GUI Enabled field. The Rendering checkbox and the
Before DLSS checkbox can also arm a connected session. Every new game remains
disarmed until an explicit request; opening the GUI does not enable processing.

Creation settings debounce for 750 ms; sharpness applies at evaluation. Old
feature histories remain alive for recorded/in-flight GPU commands, with up to
eight distinct configurations per session. Returning to a cached configuration
reuses it with a history reset. A ninth configuration or any allocation/evaluate
failure disables NR with an explicit log/GUI reason until restart. No per-frame
queue/device wait was added. Model creation can briefly stall the recording
thread, and retained histories consume extra VRAM.

The old bridge could silently stop at 64 unique command buffers (observed live:
66 accepted frames). The descriptor budget now covers 4096 buffers, including
RDR2's observed pool of 2052; exhaustion is reported. The `descriptor-stress`
gate checks actual modified readback for 130 distinct buffers. The
`rendering-settings` gate checks create-time changes, reuse, Enabled bypass and
invalid-settings bypass with Vulkan Validation Layers. These synthetic gates
are not a sustained RDR2 benchmark or a validation of physical F2 input.
