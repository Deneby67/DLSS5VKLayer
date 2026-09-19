#include "../core/ngx_fg_params.h"
#include "../ngx_capture/sr_preset.h"
#include <cstdlib>
using dlssnr::FgParameters;
static FgParameters params;
static int token;
static unsigned calls, callbacks;
extern "C" __declspec(dllexport) void* TestParameters() { return &params; }
extern "C" __declspec(dllexport) unsigned TestCalls() { return calls; }
extern "C" __declspec(dllexport) unsigned TestCallbacks() { return callbacks; }
extern "C" __declspec(dllexport) unsigned TestOrdinal() { return 123; }
extern "C" __declspec(dllexport) unsigned NVSDK_NGX_VULKAN_CreateFeature(void* cmd, int feature, void* p, void** out) {
    if (cmd != reinterpret_cast<void*>(0x1234) || !out || feature != 1) return 0xbad00005u;
    const auto requested=getenv("DLSSNR_TEST_SR_PRESET");
    if(requested) {
        unsigned preset=0,width=0;
        for(const char* key:{"DLSS.Hint.Render.Preset.Quality","DLSS.Hint.Render.Preset.Balanced", "DLSS.Hint.Render.Preset.Performance","DLSS.Hint.Render.Preset.UltraPerformance","DLSS.Hint.Render.Preset.UltraQuality","DLSS.Hint.Render.Preset.DLAA"})
            if(ngx_capture::Get(p,key,&preset,12)!=1 || preset!=unsigned(atoi(requested)))return 0xbad00005u;
        if(ngx_capture::Get(p,"Width",&width,12)!=1 || width!=640)return 0xbad00005u;
    } else if(p!=&params)return 0xbad00005u;
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
