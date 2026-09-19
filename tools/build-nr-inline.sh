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
python3 - "$out" <<'PY'
import struct,sys
from pathlib import Path
p=Path(sys.argv[1]); b=(p/'nr_color.spv').read_bytes()
words=struct.unpack('<'+'I'*(len(b)//4),b)
(p/'nr_color_spv.h').write_text('#pragma once\n#include <cstdint>\nstatic const uint32_t nr_color_spv[]={\n'+','.join(hex(w) for w in words)+'};\n')
PY
flags=(--target=x86_64-w64-mingw32 --sysroot="$sysroot/x86_64-w64-mingw32"
 -B"$gccdir/" -L"$gccdir" -fuse-ld=lld -static -femulated-tls -std=c++17 -O2 -fms-extensions
 -nostdinc++ -isystem "$gccdir/include/c++" -isystem "$gccdir/include/c++/x86_64-w64-mingw32"
 -isystem "$gccdir/include/c++/backward" -Icore -Istandalone_runner/third_party -Wl,--threads=24)
"$llvm/clang++" "${flags[@]}" test_layer/nr_hdr_probe.cpp core/ngx_snippet.cpp core/guard.cpp -lwinpthread -o "$out/nr_hdr_probe.exe"
"$llvm/llvm-readobj" --coff-exports /opt/Proton-11.0-2c-Zen5-BP18-Mimalloc/files/lib/wine/x86_64-windows/vulkan-1.dll > "$out/vulkan-exports.txt"
python3 - "$out" <<'PY'
import re,sys
from pathlib import Path
p=Path(sys.argv[1])
hooks={'vkGetInstanceProcAddr':'NrGipa','vkGetDeviceProcAddr':'NrGdpa','vkEnumeratePhysicalDevices':'NrEnumerate',
       'vkCreateDevice':'NrCreateDevice','vkCreateCommandPool':'NrCreatePool','vkDestroyCommandPool':'NrDestroyPool',
       'vkAllocateCommandBuffers':'NrAllocate','vkFreeCommandBuffers':'NrFree','vkBeginCommandBuffer':'NrBegin',
       'vkDestroyDevice':'NrDestroyDevice'}
lines=['LIBRARY vulkan-1','EXPORTS']
for block in re.findall(r'Export \{(.*?)\}',(p/'vulkan-exports.txt').read_text(),re.S):
    name=re.search(r'Name: (\S+)',block)[1]; ordinal=re.search(r'Ordinal: (\d+)',block)[1]
    target=hooks.get(name,'winevulkan.'+name)
    lines.append(f'{name}={target} @{ordinal}')
(p/'vulkan.def').write_text('\n'.join(lines)+'\n')
PY
"$llvm/clang++" "${flags[@]}" -shared ngx_capture/nr_vulkan.cpp core/ngx_snippet.cpp core/guard.cpp "$out/vulkan.def" -lwinpthread -o "$out/vulkan-1.dll"
"$llvm/clang++" "${flags[@]}" test_layer/nr_inline_probe.cpp -lwinpthread -o "$out/nr_inline_probe.exe"
