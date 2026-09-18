#!/usr/bin/env bash
# Use as a Steam launch-options prefix: /absolute/path/to/this-script %command%
# Enable the registered native Vulkan capture layer without injecting RenderDoc
# into the Steam runtime launcher via LD_PRELOAD. Steam keeps control of Proton.
set -euo pipefail
if (( $# == 0 )); then
  echo 'Use this script before %command% in the RDR2 Steam launch options.' >&2
  exit 2
fi
export ENABLE_VULKAN_RENDERDOC_CAPTURE=1
unset DISABLE_VULKAN_RENDERDOC_CAPTURE_1_46
# Inspect engine resources before NR composition. These settings affect only
# this game launch; the running helper and its saved configuration are intact.
export VKLayer_DLSS5=0 DLSSNR_ENABLE=0
exec "$@"
