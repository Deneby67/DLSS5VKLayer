#pragma once
#include <vulkan/vulkan.h>
namespace dlssfg::discovery {
// Metadata only: never adds GPU work, barriers, waits, or resource usage flags.
void Register(VkDevice, PFN_vkGetDeviceProcAddr) noexcept;
void Remove(VkDevice) noexcept;
void Present(VkDevice, const VkPresentInfoKHR*) noexcept;
PFN_vkVoidFunction Lookup(const char*, PFN_vkGetDeviceProcAddr fallback) noexcept;
}
