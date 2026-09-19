# NR input-color investigation — 2026-09-19

The RDR2 session remained running throughout this investigation. No installed
DLLs, launch settings, Rendering values or arm state were changed. The existing
NGX observer recorded 12 fresh calls without a restart; all returned success.
This proves current dispatch/parameter observations, not visual quality.

## Reproducible offline test

```sh
bash tools/build-nr-color-study.sh
python3 tools/run-nr-color-study.py /path/to/run-SESSION \
  --output build/nr-color-study/STUDY --frames 16
```

The executable uses a separate Proton prefix, Vulkan Validation Layers and the
same pinned NVIDIA NR DLL as the live capture. It takes RGBA16F input, repeats
the static image with zero motion/jitter and no depth, and saves frames 0, 8 and
15. Create tuning and sharpness come from the capture metadata. Each variant
gets a fresh process/feature. Input/output hashes, model/executable hashes,
metadata provenance and logs are retained under `build/`, never published as
part of the source repository. The final-screen control accepts
`--display-image /path/to/screenshot.png --cases display-0`; it preserves aspect
ratio and records that this is a different frame with unknown original NR state.

`--verbose` records parameter reads in the offline process. `--flags 0x60`
compares the public SR SDK numbering with the legacy NR adapter numbering;
this override does not exist in the installed game adapter.

## Observed results

The live capture was 2293x960, exposure 143.3792877, pre-exposure 1, intensity
1.85, tone 1.3, structure 1.8, skin -0.25, preset/style 0, automask 1, sharpness
0.1. Its alpha ranged from -2.65625 to 9776 and is an auxiliary engine channel.

- Baseline vs alpha=1: the model outputs were **bit-identical** at frame 15.
  Alpha is not the cause of the observed weakness in this captured scene.
- Exposure -4/-2/+2 EV, clipping + sRGB, a generic filmic fit, and omitted sRGB
  encoding changed NR's response but produced no clearly justified replacement
  for the current input mapping. Larger pixel differences are not better quality.
- Direct exposed linear HDR, even with requested HDR flags, produced severe
  color distortion and RGB output clamped at 1. Creation and dispatch success
  did not validate an HDR contract.
- Verbose reads included Sharpness and LocalToneStrength, but no HDR/SDR,
  exposure or generic Feature_Flags getters. Public 0x60 vs legacy 0x0c create
  flags gave bit-identical output in both SDR and requested-HDR controls.
  This is evidence for the pinned DLL/inputs only, not all NVIDIA versions.
- An additional final-SDR screenshot control changed clothing/skin texture and
  local contrast; it was not just a uniform color transform. Its different frame,
  unknown original NR state and missing temporal history prevent a causal
  comparison with the live pre-SR capture.
- The user's two 15:35 PNG files have different file hashes but **identical
  decoded pixels**. They cannot establish a visible on/off difference.

Nine input variants, two screenshot controls, two ABI checks, four explicit flag
controls, and two post-fix verbose repeats completed without Vulkan validation
errors. The earlier live HDR bridge analysis retained an edit projection of
0.684 overall (0.465 in its brightest bin); this is not a percentage of perceived
quality. These results do not establish a superior color mapping, a validated
HDR path, or equivalence with the game's final tonemapping. No new rendering
formula was installed on their basis.

## Confirmed API diagnostic bug

The adapter used three arguments and an invented output structure for
`NVSDK_NGX_VULKAN_GetFeatureRequirements`. NVIDIA's public Vulkan header defines
four arguments: instance, physical device, discovery info, requirement output.
The output contains platform-support reasons, minimum architecture and OS,
**not HDR feature flags**. Both the successful-init query and failed-create
fallback are now corrected in source. Discovery info identifies Feature 18,
the existing application ID and API 0x14.

The corrected call returned `0x1`, support=0, architecture=0x1b0, SEH=0 on the
same RTX 5090 where the old query returned `0xbad00005`. An independently
specified ABI in the study executable cross-checks those fields. The corrected
query produced bit-identical NR images in the baseline and HDR experiments;
it fixes a misleading diagnostic, not the visual weakness. All 26 native-loader
regression gates also passed with the corrected source. The live game keeps its
existing DLL; installing this diagnostic-only correction is not required for
continued investigation and would not change the current process.

Sources:
- https://github.com/NVIDIA/DLSS/blob/main/include/nvsdk_ngx_vk.h
- https://github.com/NVIDIA/DLSS/blob/main/include/nvsdk_ngx_defs.h

Private run evidence on the development machine:
- `build/nr-color-study/rdr2-20260919/manifest.json`
- `build/nr-color-study/rdr2-display-controls-20260919/manifest.json`
- `build/nr-color-study/requirements-check-20260919/manifest.json`
- `build/nr-color-study/flags-public-20260919/manifest.json`
- `build/nr-color-study/flags-existing-20260919/manifest.json`
- `build/nr-color-study/query-fixed-20260919/manifest.json`
- `build/nr-inline/requirements-fixed-gates/`
