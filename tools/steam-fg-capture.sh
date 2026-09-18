#!/usr/bin/env bash
# Use as a Steam launch-options prefix: /absolute/path/to/this-script %command%
# Capture is suspended: enabling RenderDoc for the process tree breaks the
# Rockstar Launcher's DXVK device creation on the tested system. Keep this
# wrapper as a transparent launch prefix so existing Steam options keep working.
set -euo pipefail
if (( $# == 0 )); then
  echo 'Use this script before %command% in the RDR2 Steam launch options.' >&2
  exit 2
fi
unset ENABLE_VULKAN_RENDERDOC_CAPTURE
export DISABLE_VULKAN_RENDERDOC_CAPTURE_1_46=1
# Preserve inherited NR settings and all game arguments. Do not re-enable
# process-tree capture until RDR2-only selection has been verified.
exec "$@"
