#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
#include <filesystem>
#include <memory>
namespace dlssfg {
// CPU-side observations only. Never maps new memory or changes GPU synchronization.
// All methods are called under the owning discovery session mutex.
class CameraProbe {
public:
    CameraProbe(const std::filesystem::path&,uint64_t,const VkPhysicalDeviceMemoryProperties&);
    ~CameraProbe();
    void buffer(VkBuffer,VkDeviceSize,VkBufferCreateFlags);
    void destroyBuffer(VkBuffer);
    void allocate(VkDeviceMemory,VkDeviceSize,uint32_t);
    void freeMemory(VkDeviceMemory);
    void map(VkDeviceMemory,VkDeviceSize,VkDeviceSize,void*);
    void unmap(VkDeviceMemory);
    void bind(VkBuffer,VkDeviceMemory,VkDeviceSize);
    void set(VkDescriptorSet,VkDescriptorPool);
    void freeSet(VkDescriptorSet);
    void pool(VkDescriptorPool);
    void updates(uint32_t,const VkWriteDescriptorSet*,uint32_t,const VkCopyDescriptorSet*);
    void invalidate(VkDescriptorSet);
    void command(VkCommandBuffer,VkCommandPool);
    void freeCommand(VkCommandBuffer);
    void begin(VkCommandBuffer);
    void commandPool(VkCommandPool,bool);
    void bindSets(VkCommandBuffer,VkPipelineBindPoint,uint32_t,uint32_t,const VkDescriptorSet*);
    void execute(VkCommandBuffer,uint32_t,const VkCommandBuffer*);
    void present();
    bool wantsSubmit();
    uint64_t submit(VkQueue,uint32_t,const VkCommandBuffer*);
    void result(uint64_t,VkResult);
private:
    struct Impl;
    std::unique_ptr<Impl> p;
};
}
