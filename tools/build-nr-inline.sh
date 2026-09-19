#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out="$PWD/build/nr-inline"
sysroot="${MINGW_SYSROOT:-$PWD/build/fg/sysroot/usr}"
llvm="${LLVM_BIN:-/opt/llvm-23.1.1-bp18/bin}"
gccdirs=("$sysroot"/lib/gcc/x86_64-w64-mingw32/*-posix)
gccdir="${gccdirs[0]}"
mkdir -p "$out"
"$PWD/build/capture/renderdoc_1.46/share/renderdoc/plugins/spirv/glslangValidator" -V ngx_capture/nr_color.comp -o "$out/nr_color.spv"
"$PWD/build/capture/renderdoc_1.46/share/renderdoc/plugins/spirv/glslangValidator" -V ngx_capture/nr_snapshot.comp -o "$out/nr_snapshot.spv"
python3 - "$out" <<'PY'
import struct,sys
from pathlib import Path
p=Path(sys.argv[1])
for name in ('nr_color','nr_snapshot'):
    b=(p/(name+'.spv')).read_bytes()
    words=struct.unpack('<'+'I'*(len(b)//4),b)
    (p/(name+'_spv.h')).write_text('#pragma once\n#include <cstdint>\nstatic const uint32_t '+name+'_spv[]={\n'+','.join(hex(w) for w in words)+'};\n')
PY
flags=(--target=x86_64-w64-mingw32 --sysroot="$sysroot/x86_64-w64-mingw32"
 -B"$gccdir/" -L"$gccdir" -fuse-ld=lld -static -femulated-tls -std=c++17 -O2 -fms-extensions
 -nostdinc++ -isystem "$gccdir/include/c++" -isystem "$gccdir/include/c++/x86_64-w64-mingw32"
 -isystem "$gccdir/include/c++/backward" -Icore -Istandalone_runner/third_party -Wl,--threads=24)
"$llvm/clang++" "${flags[@]}" test_layer/nr_hdr_probe.cpp core/ngx_snippet.cpp core/guard.cpp -lwinpthread -o "$out/nr_hdr_probe.exe"
native_loader="${DLSSNR_NATIVE_LOADER:-$HOME/.steam/debian-installation/steamapps/compatdata/1174180/pfx/drive_c/windows/system32/vulkan-1.dll}"
python3 - "$native_loader" "$out" <<'PY_NATIVE'
import hashlib,json,shutil,sys
from pathlib import Path
source=Path(sys.argv[1]);out=Path(sys.argv[2])
expected='9de5d9a7a1c14152bac98318c63d540520da16b8826bf96a76bef90a5d223906'
with source.open('rb') as f: actual=hashlib.file_digest(f,'sha256').hexdigest()
if actual!=expected: raise SystemExit('Native Vulkan loader differs from the verified RDR2 prefix DLL')
shutil.copy2(source,out/'dlssnr_system_vulkan.dll')
(out/'loader.json').write_text(json.dumps(dict(kind='rdr2_native_windows_loader',sha256=actual),indent=2)+'\n')
PY_NATIVE
"$llvm/llvm-readobj" --coff-exports "$out/dlssnr_system_vulkan.dll" > "$out/vulkan-exports.txt"
python3 - "$out" <<'PY'
import re,sys
from pathlib import Path
p=Path(sys.argv[1])
hooks={'vkGetInstanceProcAddr':'NrGipa','vkGetDeviceProcAddr':'NrGdpa','vkEnumeratePhysicalDevices':'NrEnumerate',
       'vkAllocateMemory':'NrAllocateMemory','vkEnumeratePhysicalDeviceGroups':'NrGroups','vkDestroyInstance':'NrDestroyInstance',
       'vkCreateDevice':'NrCreateDevice','vkCreateCommandPool':'NrCreatePool','vkDestroyCommandPool':'NrDestroyPool',
       'vkAllocateCommandBuffers':'NrAllocate','vkFreeCommandBuffers':'NrFree','vkBeginCommandBuffer':'NrBegin',
       'vkDestroyDevice':'NrDestroyDevice','vkCreateInstance':'NrCreateInstance',
       'vkCreateWin32SurfaceKHR':'NrCreateSurface','vkCreateSwapchainKHR':'NrCreateSwapchain',
       'vkQueueSubmit':'NrSubmit','vkWaitForFences':'NrWait',
       'vkAcquireNextImageKHR':'NrAcquire','vkQueuePresentKHR':'NrPresent'}
lines=['LIBRARY vulkan-1','EXPORTS']
for block in re.findall(r'Export \{(.*?)\}',(p/'vulkan-exports.txt').read_text(),re.S):
    name=re.search(r'Name: (\S+)',block)[1]; ordinal=re.search(r'Ordinal: (\d+)',block)[1]
    target=hooks.get(name,'dlssnr_system_vulkan.'+name)
    lines.append(f'{name}={target} @{ordinal}')
(p/'vulkan.def').write_text('\n'.join(lines)+'\n')
PY
"$llvm/clang++" "${flags[@]}" -shared ngx_capture/nr_vulkan.cpp core/ngx_snippet.cpp core/guard.cpp "$out/vulkan.def" -lwinpthread -o "$out/vulkan-1.dll"
"$llvm/clang++" "${flags[@]}" test_layer/nr_inline_probe.cpp -lwinpthread -o "$out/nr_inline_probe.exe"

"$llvm/clang++" "${flags[@]}" test_layer/nr_bootstrap_wsi.cpp -luser32 -lwinpthread -o "$out/nr_bootstrap_wsi.exe"
