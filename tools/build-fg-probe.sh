#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
root="$PWD"
out="$root/build/fg"
sysroot="${MINGW_SYSROOT:-$out/sysroot/usr}"
llvm="${LLVM_BIN:-/opt/llvm-23.1.1-bp18/bin}"
gccdirs=("$sysroot"/lib/gcc/x86_64-w64-mingw32/*-posix)
gccdir="${gccdirs[0]}"
test -d "$gccdir" || { echo 'Extract the MinGW development/runtime packages into build/fg/sysroot first.' >&2; exit 1; }
mkdir -p "$out"
flags=(--target=x86_64-w64-mingw32 --sysroot="$sysroot/x86_64-w64-mingw32"
  -B"$gccdir/" -L"$gccdir" -fuse-ld=lld -static -femulated-tls -std=c++17 -O3
  -nostdinc++ -isystem "$gccdir/include/c++" -isystem "$gccdir/include/c++/x86_64-w64-mingw32"
  -isystem "$gccdir/include/c++/backward" -Icore -Istandalone_runner/third_party
  -Wl,--threads=24 -lshell32)
# Independent translation units are compiled concurrently, capped at the requested 24 workers.
sources=(core/guard.cpp core/ngx_fg.cpp test_layer/fg_probe.cpp core/ngx_snippet.cpp helper/main.cpp common/fg_profile.cpp)
for source in "${sources[@]}"; do
  "$llvm/clang++" "${flags[@]}" -Wno-unused-command-line-argument -c "$source" -o "$out/$(basename "$source").o" &
done
"$llvm/clang++" "${flags[@]}" -Wno-unused-command-line-argument -DDLSSFG_EMBED_PROBE -c test_layer/fg_probe.cpp -o "$out/fg_probe_service.o" &
status=0
for pid in $(jobs -p); do wait "$pid" || status=1; done
test "$status" = 0 || exit 1
"$llvm/clang++" "${flags[@]}" "$out/fg_probe.cpp.o" "$out/ngx_fg.cpp.o" "$out/guard.cpp.o" -lwinpthread -o "$out/fg_probe.exe"
"$llvm/clang++" "${flags[@]}" "$out/main.cpp.o" "$out/ngx_snippet.cpp.o" "$out/guard.cpp.o" \
  "$out/fg_probe_service.o" "$out/ngx_fg.cpp.o" "$out/fg_profile.cpp.o" -lwinpthread -o "$out/dlssnr_helper.exe"
