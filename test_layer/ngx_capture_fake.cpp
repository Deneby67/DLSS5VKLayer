#include "../core/ngx_fg_params.h"
using dlssnr::FgParameters;
static FgParameters params;
static int token;
static unsigned calls, callbacks;
extern "C" __declspec(dllexport) void* TestParameters() { return &params; }
extern "C" __declspec(dllexport) unsigned TestCalls() { return calls; }
extern "C" __declspec(dllexport) unsigned TestCallbacks() { return callbacks; }
extern "C" __declspec(dllexport) unsigned TestOrdinal() { return 123; }
extern "C" __declspec(dllexport) unsigned NVSDK_NGX_VULKAN_CreateFeature(void* cmd, int feature, void* p, void** out) {
    if (cmd != reinterpret_cast<void*>(0x1234) || p != &params || !out || feature != 1) return 0xbad00005u;
    *out = &token; SetLastError(171); return 1;
}
extern "C" __declspec(dllexport) unsigned NVSDK_NGX_VULKAN_CreateFeature1(void* device, void* cmd, int feature, void* p, void** out) {
    if (device != reinterpret_cast<void*>(0x5678)) return 0xbad00005u;
    return NVSDK_NGX_VULKAN_CreateFeature(cmd, feature, p, out);
}
extern "C" __declspec(dllexport) unsigned NVSDK_NGX_VULKAN_EvaluateFeature(void* cmd, const void* handle, const void* p, void* callback) {
    ++calls;
    if (cmd != reinterpret_cast<void*>(0x1234) || handle != &token || p != &params) return 0xbad00005u;
    if (callback) { reinterpret_cast<void(*)(float)>(callback)(0.5f); ++callbacks; }
    SetLastError(172); return 1;
}
extern "C" __declspec(dllexport) unsigned NVSDK_NGX_VULKAN_ReleaseFeature(void* handle) {
    SetLastError(173); return handle == &token ? 1 : 0xbad00005u;
}
