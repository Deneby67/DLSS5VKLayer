#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out="$PWD/build/ngx-capture"
sysroot="${MINGW_SYSROOT:-$PWD/build/fg/sysroot/usr}"
llvm="${LLVM_BIN:-/opt/llvm-23.1.1-bp18/bin}"
gccdirs=("$sysroot"/lib/gcc/x86_64-w64-mingw32/*-posix)
gccdir="${gccdirs[0]}"
test -d "$gccdir" || { echo 'MinGW dependencies from build/fg are required.' >&2; exit 1; }
mkdir -p "$out"
flags=(--target=x86_64-w64-mingw32 --sysroot="$sysroot/x86_64-w64-mingw32"
  -B"$gccdir/" -L"$gccdir" -fuse-ld=lld -static -femulated-tls -std=c++17 -O2 -fms-extensions
  -nostdinc++ -isystem "$gccdir/include/c++" -isystem "$gccdir/include/c++/x86_64-w64-mingw32"
  -isystem "$gccdir/include/c++/backward" -Istandalone_runner/third_party -Wl,--threads=6)
# Independent targets; never exceed the user's 24-job limit.
"$llvm/clang++" "${flags[@]}" -shared ngx_capture/proxy.cpp ngx_capture/version.def -lwinpthread -o "$out/version.dll" &
"$llvm/clang++" "${flags[@]}" -shared test_layer/ngx_capture_fake.cpp -lwinpthread -o "$out/_nvngx.dll" &
"$llvm/clang++" "${flags[@]}" -shared test_layer/ngx_capture_stub.cpp test_layer/ngx_capture_stub.S test_layer/ngx_capture_stub.def -lwinpthread -o "$out/nvngx.dll" &
"$llvm/clang++" "${flags[@]}" test_layer/ngx_capture_probe.cpp -lversion -lwinpthread -o "$out/RDR2.exe" &
status=0
for pid in $(jobs -p); do wait "$pid" || status=1; done
exit "$status"
