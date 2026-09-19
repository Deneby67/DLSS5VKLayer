#pragma once
#include <vulkan/vulkan.h>
namespace dlssfg::discovery {
// Inventory and opt-in CPU snapshots: never adds GPU work, barriers, waits or usage flags.
void Register(VkDevice, PFN_vkGetDeviceProcAddr, const VkPhysicalDeviceMemoryProperties&,const VkDeviceCreateInfo&) noexcept;
void Remove(VkDevice) noexcept;
void Present(VkDevice, const VkPresentInfoKHR*) noexcept;
uint64_t BeforeSubmit(VkDevice,VkQueue,uint32_t,const VkSubmitInfo*) noexcept;
uint64_t BeforeSubmit2(VkDevice,VkQueue,uint32_t,const VkSubmitInfo2*) noexcept;
void AfterSubmit(VkDevice,uint64_t,VkResult) noexcept;
PFN_vkVoidFunction Lookup(const char*, PFN_vkGetDeviceProcAddr fallback) noexcept;
}
