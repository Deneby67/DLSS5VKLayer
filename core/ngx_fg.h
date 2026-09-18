#pragma once
#include "ngx_fg_params.h"
#include <string>

namespace dlssnr {

// Camera data is supplied by the caller, never inferred or silently substituted here.
struct FgCamera {
    float viewToClip[16]{}, clipToView[16]{}, clipToPrevClip[16]{}, prevClipToClip[16]{};
    float position[3]{}, up[3]{}, right[3]{}, forward[3]{};
    float nearPlane = 0, farPlane = 0, fov = 0, aspect = 0;
    bool depthInverted = false;
};

// Separate instance, parameters and lifecycle from the Feature-18 NR backend.
class NgxFg {
public:
    bool initialize(const std::wstring& dll, const std::wstring& dataPath,
                    VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                    PFN_vkGetInstanceProcAddr gipa, PFN_vkGetDeviceProcAddr gdpa);
    bool create(VkCommandBuffer recording, unsigned width, unsigned height, VkFormat format);
    bool evaluate(VkCommandBuffer recording, NVSDK_NGX_Resource_VK& color,
                  NVSDK_NGX_Resource_VK& depth, NVSDK_NGX_Resource_VK& motion,
                  NVSDK_NGX_Resource_VK& interpolated, NVSDK_NGX_Resource_VK& real,
                  NVSDK_NGX_Resource_VK& disable, const FgCamera& camera, bool reset);
    // Caller must wait for GPU completion before shutdown, including after a failed evaluate.
    void shutdown();
    const std::string& error() const { return error_; }
    NgxFg() = default;
    NgxFg(const NgxFg&) = delete;
    NgxFg& operator=(const NgxFg&) = delete;
private:
    bool result(const char* stage, NVSDK_NGX_Result value, DWORD exception);
    HMODULE module_ = nullptr;
    VkDevice device_ = VK_NULL_HANDLE;
    NVSDK_NGX_Handle* handle_ = nullptr;
    FgParameters params_;
    FnVkCreateFeature create_ = nullptr;
    FnVkEvaluateFeature evaluate_ = nullptr;
    FnVkReleaseFeature release_ = nullptr;
    FnVkShutdown1 shutdown_ = nullptr;
    bool initialized_ = false;
    std::string error_;
};
}
