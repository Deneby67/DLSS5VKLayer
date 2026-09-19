#!/usr/bin/env bash
# Native layer only. Requires the Vulkan/X11 headers used by the existing layer.
set -euo pipefail
cd "$(dirname "$0")/.."
export CXX="${CXX:-clang++}"
export CXXFLAGS="${CXXFLAGS:--std=c++17 -O2 -fPIC -pthread} -Ilayer_linux/src -Istandalone_runner/third_party"
if [[ -d build-layer-fix/deps/usr/include ]]; then
  export CXXFLAGS="$CXXFLAGS -Ibuild-layer-fix/deps/usr/include"
fi
mkdir -p build/discovery
make -j24 -f - <<'MAKE'
SOURCES := layer discovery camera_probe draw_probe shader_vk dlssnr_pass composition capture scaler_vk hotkey
OBJECTS := $(addprefix build/discovery/,$(addsuffix .o,$(SOURCES)))
all: build/discovery/libVkLayer_NV_dlssnr.so build/discovery/discovery-probe build/discovery/camera-state-test build/discovery/mapping-read-probe build/discovery/draw-state-test
build/discovery/%.o: layer_linux/src/%.cpp
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@
build/discovery/libVkLayer_NV_dlssnr.so: $(OBJECTS) layer_linux/dlssnr.map
	$(CXX) -shared -pthread -static-libstdc++ -static-libgcc -Wl,--version-script=layer_linux/dlssnr.map -Wl,-Bsymbolic $(OBJECTS) -ldl -o $@
build/discovery/discovery-probe: test_layer/discovery_probe.cpp test_layer/camera_probe_gpu.h test_layer/camera_draw_spv.h
	$(CXX) $(CXXFLAGS) $< -l:libvulkan.so.1 -o $@
build/discovery/camera-state-test: test_layer/camera_probe_state_test.cpp layer_linux/src/camera_probe.cpp layer_linux/src/camera_probe.h layer_linux/src/cpu_snapshot_reader.h layer_linux/src/draw_probe.cpp layer_linux/src/draw_probe.h
	$(CXX) $(CXXFLAGS) test_layer/camera_probe_state_test.cpp layer_linux/src/camera_probe.cpp layer_linux/src/draw_probe.cpp -o $@
build/discovery/mapping-read-probe: test_layer/mapping_read_probe.cpp layer_linux/src/cpu_snapshot_reader.h
	$(CXX) $(CXXFLAGS) $< -static-libstdc++ -static-libgcc -l:libvulkan.so.1 -o $@
build/discovery/draw-state-test: test_layer/draw_probe_state_test.cpp layer_linux/src/draw_probe.cpp layer_linux/src/draw_probe.h
	$(CXX) $(CXXFLAGS) test_layer/draw_probe_state_test.cpp layer_linux/src/draw_probe.cpp -o $@
-include $(OBJECTS:.o=.d)
MAKE
