#pragma once
#include <vulkan/vulkan.h>
#include "../../third_party/nlohmann/json.hpp"
#include <filesystem>
#include <memory>
#include <string>
#include <map>
namespace dlssfg {
// CPU command-recording evidence, not shader execution or resource completion.
// Serialized by the CameraProbe owner's mutex.
class DrawProbe {
public:
    DrawProbe();
    ~DrawProbe();
    void disable(const std::string&);
    std::string status() const;
    nlohmann::json diagnostics() const;
    void shader(VkShaderModule,const std::string& hash);
    void layout(VkPipelineLayout);
    void pipeline(VkPipeline,const VkGraphicsPipelineCreateInfo&);
    void image(VkImage,const VkImageCreateInfo&);
    void view(VkImageView,const VkImageViewCreateInfo&);
    void descriptor(const VkWriteDescriptorSet&);
    void renderPass(VkRenderPass,const VkRenderPassCreateInfo&);
    void renderPass2(VkRenderPass,const VkRenderPassCreateInfo2&);
    void framebuffer(VkFramebuffer,const VkFramebufferCreateInfo&);
    void inheritance(VkCommandBuffer,const VkCommandBufferBeginInfo&);
    void beginPass(VkCommandBuffer,const VkRenderPassBeginInfo&);
    void nextSubpass(VkCommandBuffer);
    void endPass(VkCommandBuffer);
    void beginRendering(VkCommandBuffer,const VkRenderingInfo&);
    void destroy(VkObjectType,uint64_t);
    void set(VkDescriptorSet,VkDescriptorPool);
    void write(VkDescriptorSet);
    void freeSet(VkDescriptorSet);
    void pool(VkDescriptorPool);
    void command(VkCommandBuffer,VkCommandPool);
    void reset(VkCommandBuffer);
    void freeCommand(VkCommandBuffer);
    void commandPool(VkCommandPool,bool destroy);
    void bindSets(VkCommandBuffer,VkPipelineBindPoint,VkPipelineLayout,uint32_t,uint32_t,const VkDescriptorSet*,uint32_t dynamicCount);
    void bindPipeline(VkCommandBuffer,VkPipelineBindPoint,VkPipeline);
    void invalidateSets(VkCommandBuffer);
    void execute(VkCommandBuffer,uint32_t,const VkCommandBuffer*);
    void draw(VkCommandBuffer,const char* kind,bool potentiallyNonempty,bool indirect);
    std::map<uint64_t,nlohmann::json> links(uint32_t,const VkCommandBuffer*);
private:
    struct Impl;
    std::unique_ptr<Impl> p;
};
}
