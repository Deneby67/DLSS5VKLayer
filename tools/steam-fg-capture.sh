#!/usr/bin/env bash
# Use as a Steam launch-options prefix: /absolute/path/to/this-script %command%
# The installed Vulkan manifest must point to rdr2_capture_gate.cpp's library.
# That library forwards Vulkan untouched for non-RDR2 processes and loads the
# real RenderDoc library only after matching the Windows executable basename.
set -euo pipefail
if (( $# == 0 )); then
  echo 'Use this script before %command% in the RDR2 Steam launch options.' >&2
  exit 2
fi
# NGX observations are scoped to RDR2 again inside the PE proxy. Keep the old
# always-on draw tracker out of this path; it caused a major gameplay slowdown.
if [[ "${DLSSFG_NGX_CAPTURE:-auto}" == 1 ]] ||
   [[ "${DLSSFG_NGX_CAPTURE:-auto}" == auto && -f "$HOME/.config/dlssnr/fg-ngx.enabled" ]]; then
  export DLSSFG_NGX_CAPTURE_DIR="Z:$HOME/.local/state/dlssnr/ngx"
  export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:+$WINEDLLOVERRIDES;}version=n,b"
  export DLSSFG_DISCOVERY=0
else
  unset DLSSFG_NGX_CAPTURE_DIR
fi
# Discovery is per-user opt-in, and the C++ layer independently matches RDR2.exe.
# The marker is installed with a backup/restore script by install-discovery.py.
if [[ "${DLSSFG_DISCOVERY:-auto}" == 1 ]] ||
   [[ "${DLSSFG_DISCOVERY:-auto}" == auto && -f "$HOME/.config/dlssnr/fg-discovery.enabled" ]]; then
  export DLSSFG_DISCOVERY_DIR="${DLSSFG_DISCOVERY_DIR:-$HOME/.local/state/dlssnr/discovery}"
  # The first 128 MiB missed the fragment shader paired with the camera pass.
  # Keep the larger dump bounded and preserve explicit user overrides.
  export DLSSFG_SHADER_LIMIT_MIB="${DLSSFG_SHADER_LIMIT_MIB:-512}"
else
  unset DLSSFG_DISCOVERY_DIR
fi
# Selective attachment passes isolated tests but RDR2 itself hangs during startup
# with this RenderDoc build. Keep normal game launch as the default until the
# game-specific incompatibility is diagnosed. Retain explicit developer opt-in.
if [[ "${DLSSFG_ENABLE_RENDERDOC:-0}" != 1 ]]; then
  unset ENABLE_VULKAN_RENDERDOC_CAPTURE
  export DISABLE_VULKAN_RENDERDOC_CAPTURE_1_46=1
  exec "$@"
fi
capture_dir="$HOME/.local/lib/dlssnr-fg/capture"
manifest="$HOME/.local/share/vulkan/implicit_layer.d/renderdoc_capture.json"
if [[ ! -r "$capture_dir/renderdoc.path" || ! -f "$capture_dir/libdlssfg_capture_gate.so" ]] ||
   ! grep -q 'DLSSFG_Negotiate' "$manifest"; then
  echo '[rdr2-capture] gate not installed; using normal launch' >&2
  unset ENABLE_VULKAN_RENDERDOC_CAPTURE
  export DISABLE_VULKAN_RENDERDOC_CAPTURE_1_46=1
  exec "$@"
fi
IFS= read -r DLSSFG_RENDERDOC_LIBRARY < "$capture_dir/renderdoc.path"
if [[ ! -f "$DLSSFG_RENDERDOC_LIBRARY" ]]; then
  echo '[rdr2-capture] backend missing; using normal launch' >&2
  unset ENABLE_VULKAN_RENDERDOC_CAPTURE
  export DISABLE_VULKAN_RENDERDOC_CAPTURE_1_46=1
  exec "$@"
fi
export DLSSFG_RENDERDOC_LIBRARY ENABLE_VULKAN_RENDERDOC_CAPTURE=1
unset DISABLE_VULKAN_RENDERDOC_CAPTURE_1_46
export VKLayer_DLSS5=0 DLSSNR_ENABLE=0
exec "$@"
