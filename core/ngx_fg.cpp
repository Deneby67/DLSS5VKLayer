#include "ngx_fg.h"
#include "guard.h"
#include "ngx_caller.h"
#include <cmath>

namespace dlssnr {
static SpoofState fgCaller;
static void NVSDK_CONV FgLog(const char* message, int level, int feature) {
    Log("[fg-sdk:%d:%d] %s", level, feature, message ? message : "");
}
bool NgxFg::result(const char* stage, NVSDK_NGX_Result value, DWORD exception) {
    Log("[fg] %s -> %#x seh=%#lx", stage, unsigned(value), exception);
    // Do not use NR's success macro: it also accepts its synthetic SEH sentinel.
    if (!exception && value == NVSDK_NGX_Result_Success) return true;
    char buffer[160];
    snprintf(buffer, sizeof(buffer), "%s: NGX=%#x SEH=%#lx", stage, unsigned(value), exception);
    error_ = buffer;
    return false;
}

bool NgxFg::initialize(const std::wstring& dll, const std::wstring& dataPath,
                      VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                      PFN_vkGetInstanceProcAddr gipa, PFN_vkGetDeviceProcAddr gdpa) {
    if (module_) { error_ = "FG already initialized"; return false; }
    device_ = device;
    module_ = LoadLibraryExW(dll.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module_) {
        error_ = "Cannot load nvngx_dlssg.dll, Win32=" + std::to_string(GetLastError());
        Log("[fg] %s", error_.c_str()); return false;
    }
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(module_);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(reinterpret_cast<char*>(module_) + dos->e_lfanew);
    RegisterModuleRange("nvngx_dlssg.dll", uintptr_t(module_), nt->OptionalHeader.SizeOfImage);
    create_ = reinterpret_cast<FnVkCreateFeature>(GetProcAddress(module_, "NVSDK_NGX_VULKAN_CreateFeature"));
    evaluate_ = reinterpret_cast<FnVkEvaluateFeature>(GetProcAddress(module_, "NVSDK_NGX_VULKAN_EvaluateFeature"));
    release_ = reinterpret_cast<FnVkReleaseFeature>(GetProcAddress(module_, "NVSDK_NGX_VULKAN_ReleaseFeature"));
    shutdown_ = reinterpret_cast<FnVkShutdown1>(GetProcAddress(module_, "NVSDK_NGX_VULKAN_Shutdown1"));
    using Init2 = NVSDK_NGX_Result (NVSDK_CONV*)(unsigned long long, const wchar_t*,
        VkInstance, VkPhysicalDevice, VkDevice, PFN_vkGetInstanceProcAddr,
        PFN_vkGetDeviceProcAddr, NVSDK_NGX_Version, const NVSDK_NGX_Parameter*);
    auto init = reinterpret_cast<Init2>(GetProcAddress(module_, "NVSDK_NGX_VULKAN_Init_Ext2"));
    if (!init || !create_ || !evaluate_ || !release_ || !shutdown_) {
        error_ = "Incomplete FG Vulkan exports"; return false;
    }
    if (!InstallCallerSpoof(module_, fgCaller, GetModuleHandleW(nullptr))) {
        error_ = "Cannot establish NGX caller identity"; return false;
    }
    DWORD seh = 0;
    params_.set("Minimum.Logging.Level", 2u);
    params_.set("Log.Callback", reinterpret_cast<void*>(&FgLog));
    params_.set("Disable.Other.Logging.Sinks", 0u);
    auto r = Guarded([&] { return init(0, dataPath.c_str(), instance, physical, device,
        gipa, gdpa, NVSDK_NGX_Version_API_14, params_.abi()); }, NVSDK_NGX_Result_FAIL_SEH, &seh);
    initialized_ = result("Init_Ext2", r, seh);
    return initialized_;
}

bool NgxFg::create(VkCommandBuffer recording, unsigned width, unsigned height, VkFormat format) {
    if (!initialized_ || handle_) { error_ = "FG create lifecycle error"; return false; }
    params_.set("Width", width);
    params_.set("Height", height);
    params_.set("CreationNodeMask", 1u);
    params_.set("VisibilityNodeMask", 1u);
    params_.set("DLSSG.BackbufferFormat", unsigned(format));
    DWORD seh = 0;
    auto r = Guarded([&] { return create_(recording, 11, params_.abi(), &handle_); },
        NVSDK_NGX_Result_FAIL_SEH, &seh);
    if (!result("CreateFeature(11)", r, seh)) return false;
    if (!handle_) { error_ = "FG create returned a null handle"; return false; }
    return true;
}

bool NgxFg::evaluate(VkCommandBuffer recording, NVSDK_NGX_Resource_VK& color,
                    NVSDK_NGX_Resource_VK& depth, NVSDK_NGX_Resource_VK& motion,
                    NVSDK_NGX_Resource_VK& interpolated, NVSDK_NGX_Resource_VK& real,
                    NVSDK_NGX_Resource_VK& disable, const FgCamera& c, bool reset) {
    const auto finite=[](const auto& values) {
        for (float v:values) if (!std::isfinite(v)) return false;
        return true;
    };
    if (!handle_ || !finite(c.viewToClip) || !finite(c.clipToView) ||
        !finite(c.clipToPrevClip) || !finite(c.prevClipToClip) ||
        !finite(c.position) || !finite(c.up) || !finite(c.right) || !finite(c.forward) ||
        !std::isfinite(c.farPlane) || !std::isfinite(c.aspect) ||
        !(c.nearPlane > 0 && c.farPlane > c.nearPlane &&
                     c.fov > 0 && c.fov < 3.142f && c.aspect > 0)) {
        error_ = "Missing FG handle or invalid camera"; return false;
    }
    const auto ptr = [&](const char* name, const void* p) { params_.set(name, const_cast<void*>(p)); };
    ptr("DLSSG.Backbuffer", &color); ptr("DLSSG.Depth", &depth); ptr("DLSSG.MVecs", &motion);
    ptr("DLSSG.OutputInterpolated", &interpolated); ptr("DLSSG.OutputReal", &real);
    ptr("DLSSG.OutputDisableInterpolation", &disable);
    const auto extent = [&](const char* resource, unsigned w, unsigned h) {
        const std::string prefix=std::string("DLSSG.")+resource+"Subrect";
        params_.set((prefix+"BaseX").c_str(),0u); params_.set((prefix+"BaseY").c_str(),0u);
        params_.set((prefix+"Width").c_str(),w); params_.set((prefix+"Height").c_str(),h);
    };
    extent("InputBackbuffer",color.Resource.ImageViewInfo.Width,color.Resource.ImageViewInfo.Height);
    extent("MVecs",motion.Resource.ImageViewInfo.Width,motion.Resource.ImageViewInfo.Height);
    extent("Depth",depth.Resource.ImageViewInfo.Width,depth.Resource.ImageViewInfo.Height);
    extent("OutputInterpolated",interpolated.Resource.ImageViewInfo.Width,interpolated.Resource.ImageViewInfo.Height);
    extent("OutputReal",real.Resource.ImageViewInfo.Width,real.Resource.ImageViewInfo.Height);
    for (auto name : {"DLSSG.HUDLess", "DLSSG.UI", "DLSSG.UIAlpha", "DLSSG.BidirectionalDistortionField"}) ptr(name, nullptr);
    ptr("DLSSG.CameraViewToClip", c.viewToClip); ptr("DLSSG.ClipToCameraView", c.clipToView);
    ptr("DLSSG.ClipToPrevClip", c.clipToPrevClip); ptr("DLSSG.PrevClipToClip", c.prevClipToClip);
    static const float identity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    ptr("DLSSG.ClipToLensClip", identity);
    params_.set("DLSSG.MultiFrameCount", 1u); params_.set("DLSSG.MultiFrameIndex", 1u);
    params_.set("DLSSG.Reset", unsigned(reset));
    params_.set("DLSSG.DepthInverted", unsigned(c.depthInverted));
    params_.set("DLSSG.CameraMotionIncluded", 1u);
    for (auto name : {"DLSSG.ColorBuffersHDR", "DLSSG.OrthoProjection", "DLSSG.NotRenderingGameFrames",
                     "DLSSG.AutomodeOverrideReset", "DLSSG.MvecDilated", "DLSSG.MvecJittered", "DLSSG.MenuDetectionEnabled"}) params_.set(name, 0u);
    for (auto name : {"DLSSG.JitterOffsetX", "DLSSG.JitterOffsetY", "DLSSG.CameraPinholeOffsetX", "DLSSG.CameraPinholeOffsetY"}) params_.set(name, 0.0f);
    params_.set("DLSSG.MvecScaleX", 1.0f); params_.set("DLSSG.MvecScaleY", 1.0f);
    params_.set("DLSSG.MvecInvalidValue", 65504.0f);
    params_.set("DLSSG.CameraNear", c.nearPlane); params_.set("DLSSG.CameraFar", c.farPlane);
    params_.set("DLSSG.CameraFOV", c.fov); params_.set("DLSSG.CameraAspectRatio", c.aspect);
    params_.set("DLSSG.MinRelativeLinearDepthObjectSeparation", 40.0f);
    const float* vectors[] = {c.position, c.up, c.right, c.forward};
    const char* names[] = {"CameraPos", "CameraUp", "CameraRight", "CameraFwd"};
    for (unsigned v = 0; v < 4; ++v) for (unsigned axis = 0; axis < 3; ++axis) {
        std::string key = std::string("DLSSG.") + names[v] + "XYZ"[axis];
        params_.set(key.c_str(), vectors[v][axis]);
    }
    DWORD seh = 0;
    auto r = Guarded([&] { return evaluate_(recording, handle_, params_.abi(), nullptr); },
        NVSDK_NGX_Result_FAIL_SEH, &seh);
    return result("EvaluateFeature(11)", r, seh);
}

void NgxFg::shutdown() {
    DWORD seh = 0;
    if (handle_ && release_) {
        auto r = Guarded([&] { return release_(handle_); }, NVSDK_NGX_Result_FAIL_SEH, &seh);
        result("ReleaseFeature", r, seh); handle_ = nullptr;
    }
    if (initialized_ && shutdown_) {
        auto r = Guarded([&] { return shutdown_(device_); }, NVSDK_NGX_Result_FAIL_SEH, &seh);
        result("Shutdown1", r, seh);
    }
    RemoveCallerSpoof(fgCaller);
    if (module_) FreeLibrary(module_);
    module_ = nullptr; initialized_ = false;
}
}
