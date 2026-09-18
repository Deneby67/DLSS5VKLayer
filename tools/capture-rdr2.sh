#!/usr/bin/env bash
# One diagnostic launch; does not edit Steam launch options or replace any layer.
set -euo pipefail
root="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
steam="${DLSSFG_STEAM_ROOT:-$HOME/.steam/debian-installation}"
game="${DLSSFG_RDR2_DIR:-$steam/steamapps/common/Red Dead Redemption 2}"
proton="${DLSSFG_PROTON:-/opt/Proton-11.0-2c-Zen5-BP18-Mimalloc/proton}"
runtime="$steam/steamapps/common/SteamLinuxRuntime_4/_v2-entry-point"
rdoc="${DLSSFG_RENDERDOC:-$root/build/capture/renderdoc_1.46/bin/renderdoccmd}"
captures="${DLSSFG_CAPTURES:-$root/build/capture/frames}"
for file in "$game/PlayRDR2.exe" "$proton" "$runtime" "$rdoc"; do
  test -f "$file" || { echo "Missing: $file" >&2; exit 1; }
done
mkdir -p "$captures"
# A fresh process is needed: an existing launcher cannot inherit capture settings.
if pgrep -fi '(^|[/\\])RDR2\.exe( |$)' >/dev/null; then
  echo 'Close RDR2 before starting a diagnostic capture.' >&2
  exit 1
fi
export SteamAppId=1174180 SteamGameId=1174180 STEAM_COMPAT_APP_ID=1174180
export STEAM_COMPAT_CLIENT_INSTALL_PATH="$steam"
export STEAM_COMPAT_INSTALL_PATH="$game"
export STEAM_COMPAT_DATA_PATH="$steam/steamapps/compatdata/1174180"
export PROTON_ENABLE_NVAPI=1 PROTON_FORCE_LARGE_ADDRESS_AWARE=1
export ENABLE_VULKAN_RENDERDOC_CAPTURE=1
export DLSSNR_ENABLE=0 VKLayer_DLSS5=0 MANGOHUD=0
unset VK_INSTANCE_LAYERS VK_LOADER_LAYERS_ENABLE VK_LOADER_LAYERS_DISABLE
cd "$game"
exec "$rdoc" capture --opt-hook-children --wait-for-exit --capture-file "$captures/rdr2" \
  "$runtime" --verb=waitforexitandrun -- "$proton" waitforexitandrun \
  "$game/PlayRDR2.exe" -vulkan -width 3440 -height 1440 -cpuLoadRebalancing -ignorepipelinecache
