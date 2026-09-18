#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/capture
cxx="${CXX:-/opt/llvm-23.1.1-bp18/bin/clang++}"
"$cxx" -std=c++17 -O2 -Wall -Wextra -Werror -fPIC -fvisibility=hidden -shared \
  -Istandalone_runner/third_party tools/rdr2_capture_gate.cpp -ldl -pthread -static-libstdc++ -static-libgcc -Wl,-Bsymbolic -Wl,--exclude-libs,ALL \
  -o build/capture/libdlssfg_capture_gate.so
"$cxx" -std=c++17 -O2 -Istandalone_runner/third_party \
  test_layer/capture_gate_probe.cpp -lvulkan -o build/capture/capture-gate-probe
