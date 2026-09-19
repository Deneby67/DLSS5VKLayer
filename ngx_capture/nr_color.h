#pragma once
// The caller owns device/command-buffer lifetime and guarantees shader-read
// layouts for the original inputs. All intermediate images belong to this pass.
#include "../helper/vulkan_context.h"
#include "../build/nr-inline/nr_color_spv.h"
#include <map>
#include <array>
struct NrColorBridge {
    VkCtx c{};
    GpuImage encoded{},model{},restored{};
    VkSampler sampler{};
    VkDescriptorSetLayout layout{};
    VkDescriptorPool pool{};
    VkPipelineLayout pipelineLayout{};
    VkPipeline pipeline{};
    std::map<VkCommandBuffer,std::array<VkDescriptorSet,2>> sets;
    PFN_vkCreateSampler createSampler{};
    PFN_vkDestroySampler destroySampler{};
    PFN_vkCmdPushConstants push{};
    bool initialize(VkCtx context,unsigned w,unsigned h) {
        c=context;
        createSampler=(PFN_vkCreateSampler)g_gipa(c.instance,"vkCreateSampler");
        destroySampler=(PFN_vkDestroySampler)g_gipa(c.instance,"vkDestroySampler");
        push=(PFN_vkCmdPushConstants)g_gipa(c.instance,"vkCmdPushConstants");
        if(!createSampler || !destroySampler || !push) return false;
        for(auto* image:{&encoded,&model,&restored})
            if(!CreateImage2D(c,VK_FORMAT_R16G16B16A16_SFLOAT,w,h,*image)) return false;
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter=si.minFilter=VK_FILTER_NEAREST; si.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU=si.addressModeV=si.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if(createSampler(c.device,&si,nullptr,&sampler)!=VK_SUCCESS) return false;
        VkDescriptorSetLayoutBinding bindings[5]{};
        for(unsigned i=0;i<5;++i) bindings[i]={i,i==4?VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount=5; li.pBindings=bindings;
        if(vkCreateDescriptorSetLayout(c.device,&li,nullptr,&layout)!=VK_SUCCESS) return false;
        VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,512},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,128}};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.maxSets=128; pi.poolSizeCount=2; pi.pPoolSizes=sizes;
        if(vkCreateDescriptorPool(c.device,&pi,nullptr,&pool)!=VK_SUCCESS) return false;
        VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT,0,16};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount=1; pli.pSetLayouts=&layout; pli.pushConstantRangeCount=1; pli.pPushConstantRanges=&range;
        if(vkCreatePipelineLayout(c.device,&pli,nullptr,&pipelineLayout)!=VK_SUCCESS) return false;
        VkShaderModule shader{}; VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        sm.codeSize=sizeof(nr_color_spv); sm.pCode=nr_color_spv;
        if(vkCreateShaderModule(c.device,&sm,nullptr,&shader)!=VK_SUCCESS) return false;
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,shader,"main",nullptr};
        ci.layout=pipelineLayout;
        auto result=vkCreateComputePipelines(c.device,VK_NULL_HANDLE,1,&ci,nullptr,&pipeline);
        vkDestroyShaderModule(c.device,shader,nullptr); return result==VK_SUCCESS;
    }
    bool prepare(VkCommandBuffer cmd,VkImageView color,VkImageView exposure) {
        auto found=sets.find(cmd);
        if(found==sets.end()) {
            if(sets.size()>=64) return false;
            VkDescriptorSetLayout layouts[]={layout,layout};
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool=pool; ai.descriptorSetCount=2; ai.pSetLayouts=layouts;
            std::array<VkDescriptorSet,2> allocated{};
            if(vkAllocateDescriptorSets(c.device,&ai,allocated.data())!=VK_SUCCESS) return false;
            found=sets.emplace(cmd,allocated).first;
        }
        for(unsigned mode=0;mode<2;++mode) {
            VkDescriptorImageInfo images[]={{sampler,color,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                {sampler,exposure,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                {sampler,mode?encoded.view:color,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                {sampler,mode?model.view:color,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                {VK_NULL_HANDLE,mode?restored.view:encoded.view,VK_IMAGE_LAYOUT_GENERAL}};
            VkWriteDescriptorSet writes[5]{};
            for(unsigned i=0;i<5;++i) {
                writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; writes[i].dstSet=found->second[mode];
                writes[i].dstBinding=i; writes[i].descriptorCount=1;
                writes[i].descriptorType=i==4?VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[i].pImageInfo=&images[i];
            }
            vkUpdateDescriptorSets(c.device,5,writes,0,nullptr);
        }
        return true;
    }
    void dispatch(VkCommandBuffer cmd,unsigned mode,float preExposure) {
        GpuImage& output=mode?restored:encoded;
        TransitionImage(c,cmd,output,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,
            VK_ACCESS_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        struct { uint32_t mode,width,height; float preExposure; } pc{mode,encoded.width,encoded.height,preExposure};
        vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
        auto set=sets.at(cmd)[mode];
        vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipelineLayout,0,1,&set,0,nullptr);
        push(cmd,pipelineLayout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(pc),&pc);
        vkCmdDispatch(cmd,(encoded.width+7)/8,(encoded.height+7)/8,1);
        TransitionImage(c,cmd,output,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    }
    void shutdown() {
        if(!c.device) return;
        if(pipeline) vkDestroyPipeline(c.device,pipeline,nullptr);
        if(pipelineLayout) vkDestroyPipelineLayout(c.device,pipelineLayout,nullptr);
        if(pool) vkDestroyDescriptorPool(c.device,pool,nullptr);
        if(layout) vkDestroyDescriptorSetLayout(c.device,layout,nullptr);
        if(sampler) destroySampler(c.device,sampler,nullptr);
        for(auto* i:{&encoded,&model,&restored}) DestroyImage2D(c,*i);
        c.device=nullptr;
    }
};
