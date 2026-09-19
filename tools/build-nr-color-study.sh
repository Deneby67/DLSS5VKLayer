#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out="$PWD/build/nr-color-study"
sysroot="${MINGW_SYSROOT:-$PWD/build/fg/sysroot/usr}"
llvm="${LLVM_BIN:-/opt/llvm-23.1.1-bp18/bin}"
gccdirs=("$sysroot"/lib/gcc/x86_64-w64-mingw32/*-posix)
gccdir="${gccdirs[0]}"
mkdir -p "$out"
"$llvm/clang++" --target=x86_64-w64-mingw32 --sysroot="$sysroot/x86_64-w64-mingw32" \
 -B"$gccdir/" -L"$gccdir" -fuse-ld=lld -static -femulated-tls -std=c++17 -O2 -fms-extensions \
 -nostdinc++ -isystem "$gccdir/include/c++" -isystem "$gccdir/include/c++/x86_64-w64-mingw32" \
 -isystem "$gccdir/include/c++/backward" -Icore -Istandalone_runner/third_party -Wl,--threads=24 \
 test_layer/nr_color_study.cpp core/ngx_snippet.cpp core/guard.cpp -lwinpthread -o "$out/nr_color_study.exe"
