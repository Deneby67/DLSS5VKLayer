# Frame generation profiles (experimental schema 1)

Profiles describe Vulkan resources, not absolute process addresses. They remain
independent of ASLR and Vulkan handle allocation. An enabled profile matches the
executable basename **and exact SHA-256**, then identifies shader resources by
SPIR-V SHA-256, shader stage, descriptor set, binding and array element. Camera
matrices additionally specify a byte offset relative to the descriptor's buffer
range (including its dynamic offset) and row/column order. Resource lifetimes,
buffer bounds, image format and the producing submission still need live checks.

`rdr2.json` is a disabled calibration draft, **not a working RDR2 profile**.
Do not fill it with guessed addresses. Captured bindings must be verified during
camera movement, camera changes, menus and resolution changes before enabling it.

The current helper loads and validates `DLSSFG_PROFILE` once per second. A broken
edit disables that profile instead of retaining old bindings. The loader and
source-selection logic are implemented; GPU resource capture, profile selection
in the GUI and game FG presentation are not yet connected.

An enabled profile contains:

- `schema_version: 1`, `id`, `enabled: true`, `executable`, `executable_sha256`.
- `optical_flow_fallback`: whether to use the helper's existing flow when engine
  motion is unavailable. This does **not** replace missing depth or camera data.
- `motion`: `shader_sha256`, `stage` (`vertex`, `fragment`, `compute`), `set`,
  `binding`, `array_element`, `format` (`R16G16_SFLOAT`, `R32G32_SFLOAT`,
  `R16G16_SNORM`), `sample_grid: "current"`, `units` (`pixels`, `uv`, `ndc`),
  `direction` (`current_to_previous`, `previous_to_current`), `scale: [x,y]`,
  `includes_jitter`. Scale can flip Y. Previous-to-current describes the sign
  **at current-frame sample locations**; a field sampled on the previous frame
  cannot be converted by simply negating vectors and is rejected.
- `depth`: the same resource selector, depth `format`,
  `encoding: "hardware_zero_to_one"` and `reversed`.
- `camera`: `projection: "perspective_zero_to_one"` and matrices `world_to_view`,
  `view_to_clip`, `clip_to_previous_clip`. Each matrix has a shader resource
  selector, `byte_offset` and `order` (`row_major` or `column_major`).

Motion converts to current-to-previous displacement in motion-field pixels. UV
uses width/height, NDC uses half width/height. Jitter offsets passed to the
converter are pixel offsets in that same field; embedded previous-minus-current
jitter is removed. This first schema only supports a full-frame field with
camera motion included. Packed/custom motion encodings, partial viewports and
previous-frame sampling require additional conversion support.

Engine motion takes priority only when the profile and frame ID match. Optical
flow also needs the current frame ID. FG requires matching valid depth and camera
from that frame in either mode. NR remains independent of these requirements.

Run `bash tools/test-fg-profile.sh` to check parsing, version mismatches, unsafe
selectors, stale frames, fallback, vector conversion and invalid hot reloads.
