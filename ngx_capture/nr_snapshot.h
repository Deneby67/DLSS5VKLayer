#pragma once
#include "nr_color.h"
#include "../core/ngx_snippet.h"
#include "../build/nr-inline/nr_snapshot_spv.h"
#include <atomic>

// One opt-in snapshot per process. No image transfers, game resource mutations,
// queue waits or allocations in normal rendering. Only ONE_TIME_SUBMIT primary
// recordings qualify, so a later replay cannot race the host's readback.
// Resources remain alive until device idle/destruction, even if recording was
// discarded. Completion is proved by our own fence after the game's submission.
struct NrSnapshot {
    VkCtx c{};
    VkBuffer buffer{}; VkDeviceMemory memory{}; void* mapped{};
    VkDescriptorSetLayout layout{}; VkDescriptorPool pool{}; VkDescriptorSet set{};
    VkPipelineLayout pipelineLayout{}; VkPipeline pipeline{}; VkFence fence{};
    VkCommandBuffer command{};
    std::atomic<bool> awaitSubmit{false};
    bool attempted=false,waiting=false,finished=false,announced=false;
    ULONGLONG nextPoll=0,token=0;
    unsigned width=0,height=0,frame=0;
    VkDeviceSize stride=0,first=0,bytes=0;
    std::wstring directory;
    dlssnr::NgxTuning tuning;
#define FN(name) PFN_##name name{};
    FN(vkCreateBuffer) FN(vkDestroyBuffer) FN(vkGetBufferMemoryRequirements)
    FN(vkBindBufferMemory) FN(vkMapMemory) FN(vkUnmapMemory)
    FN(vkCreateFence) FN(vkDestroyFence) FN(vkGetFenceStatus) FN(vkQueueSubmit)
#undef FN
    bool initialize(NrColorBridge& bridge,PFN_vkGetDeviceProcAddr gdpa) {
        c=bridge.c; width=bridge.encoded.width;height=bridge.encoded.height;
#define LOAD(name) name=(PFN_##name)gdpa(c.device,#name); if(!name)return false;
        LOAD(vkCreateBuffer) LOAD(vkDestroyBuffer) LOAD(vkGetBufferMemoryRequirements)
        LOAD(vkBindBufferMemory) LOAD(vkMapMemory) LOAD(vkUnmapMemory)
        LOAD(vkCreateFence) LOAD(vkDestroyFence) LOAD(vkGetFenceStatus) LOAD(vkQueueSubmit)
#undef LOAD
        auto get=(PFN_vkGetPhysicalDeviceProperties)g_gipa(c.instance,"vkGetPhysicalDeviceProperties");
        if(!get)return false;
        VkPhysicalDeviceProperties properties{};get(c.physical,&properties);
        auto alignment=properties.limits.minStorageBufferOffsetAlignment;
        if(!alignment)alignment=1;
        auto aligned=[&](VkDeviceSize n){return (n+alignment-1)/alignment*alignment;};
        VkDeviceSize plane=VkDeviceSize(width)*height*8;
        if(properties.limits.maxPerStageDescriptorStorageBuffers<5 || properties.limits.maxDescriptorSetStorageBuffers<5 || plane>properties.limits.maxStorageBufferRange)return false;
        first=aligned(16);stride=aligned(plane);bytes=first+4*stride;
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size=bytes;bi.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        if(vkCreateBuffer(c.device,&bi,nullptr,&buffer)!=VK_SUCCESS)return false;
        VkMemoryRequirements req{};vkGetBufferMemoryRequirements(c.device,buffer,&req);
        auto type=FindHostMemoryType(c,req.memoryTypeBits,true);
        if(type==UINT32_MAX)return false;
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;ai.memoryTypeIndex=type;
        if(vkAllocateMemory(c.device,&ai,nullptr,&memory)!=VK_SUCCESS ||
           vkBindBufferMemory(c.device,buffer,memory,0)!=VK_SUCCESS ||
           vkMapMemory(c.device,memory,0,VK_WHOLE_SIZE,0,&mapped)!=VK_SUCCESS)return false;
        // Include deterministic padding; the shader fills every pixel and metadata.
        memset(mapped,0,size_t(bytes));
        VkDescriptorSetLayoutBinding bindings[10]{};
        for(unsigned i=0;i<10;++i)bindings[i]={i,i<5?VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};li.bindingCount=10;li.pBindings=bindings;
        if(vkCreateDescriptorSetLayout(c.device,&li,nullptr,&layout)!=VK_SUCCESS)return false;
        VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,5},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,5}};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pi.maxSets=1;pi.poolSizeCount=2;pi.pPoolSizes=sizes;
        if(vkCreateDescriptorPool(c.device,&pi,nullptr,&pool)!=VK_SUCCESS)return false;
        VkDescriptorSetAllocateInfo si{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};si.descriptorPool=pool;si.descriptorSetCount=1;si.pSetLayouts=&layout;
        if(vkAllocateDescriptorSets(c.device,&si,&set)!=VK_SUCCESS)return false;
        VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT,0,16};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount=1;pli.pSetLayouts=&layout;pli.pushConstantRangeCount=1;pli.pPushConstantRanges=&range;
        if(vkCreatePipelineLayout(c.device,&pli,nullptr,&pipelineLayout)!=VK_SUCCESS)return false;
        VkShaderModule shader{};VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        sm.codeSize=sizeof(nr_snapshot_spv);sm.pCode=nr_snapshot_spv;
        if(vkCreateShaderModule(c.device,&sm,nullptr,&shader)!=VK_SUCCESS)return false;
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,shader,"main",nullptr};ci.layout=pipelineLayout;
        auto result=vkCreateComputePipelines(c.device,VK_NULL_HANDLE,1,&ci,nullptr,&pipeline);
        vkDestroyShaderModule(c.device,shader,nullptr);
        if(result!=VK_SUCCESS)return false;
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        return vkCreateFence(c.device,&fi,nullptr,&fence)==VK_SUCCESS;
    }
    bool requested(const wchar_t* arm,ULONGLONG session) {
        if(attempted || !arm[0])return false;
        auto now=GetTickCount64();if(now<nextPoll)return false;nextPoll=now+500;
        std::wstring path=arm;auto slash=path.find_last_of(L"/\\");if(slash==path.npos)return false;
        directory=path.substr(0,slash+1);
        auto f=CreateFileW((directory+L"hdr-capture.request").c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                           nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(f==INVALID_HANDLE_VALUE)return false;
        char data[128]{};DWORD size=0;unsigned long pid=0;unsigned long long supplied=0;char extra=0;
        bool ok=ReadFile(f,data,sizeof(data)-1,&size,nullptr) && size<sizeof(data)-1 &&
                sscanf(data,"%lu %llu %c",&pid,&supplied,&extra)==2 && pid==GetCurrentProcessId() && supplied==session;
        CloseHandle(f);return ok;
    }
    void record(NrColorBridge& bridge,PFN_vkGetDeviceProcAddr gdpa,VkCommandBuffer cmd,bool oneTime,
                VkImageView original,VkImageView exposure,const wchar_t* arm,ULONGLONG session,unsigned call,
                float pre,float sharp,const dlssnr::NgxTuning& selected) {
        if(!requested(arm,session))return;
        if(!oneTime) {
            if(!announced)dlssnr::Log("[nr-snapshot] waiting for ONE_TIME_SUBMIT recording; reusable buffers are not captured");
            announced=true;return;
        }
        attempted=true;token=session;frame=call;tuning=selected;
        if(!initialize(bridge,gdpa)) {
            dlssnr::Log("[nr-snapshot] initialization failed; NR and SR unchanged");return;
        }
        VkImageView views[]={original,bridge.encoded.view,bridge.model.view,bridge.restored.view,exposure};
        VkDescriptorImageInfo images[5]{};VkDescriptorBufferInfo buffers[5]{};VkWriteDescriptorSet writes[10]{};
        for(unsigned i=0;i<10;++i) {
            writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[i].dstSet=set;writes[i].dstBinding=i;writes[i].descriptorCount=1;
            if(i<5) {
                images[i]={bridge.sampler,views[i],VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                writes[i].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;writes[i].pImageInfo=&images[i];
            }else{
                unsigned j=i-5;buffers[j]={buffer,j==4?0:first+j*stride,j==4?16:VkDeviceSize(width)*height*8};
                writes[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;writes[i].pBufferInfo=&buffers[j];
            }
        }
        vkUpdateDescriptorSets(c.device,10,writes,0,nullptr);
        vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
        vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipelineLayout,0,1,&set,0,nullptr);
        struct {uint32_t w,h;float pre,sharp;} pc{width,height,pre,sharp};
        bridge.push(cmd,pipelineLayout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(pc),&pc);
        vkCmdDispatch(cmd,(width+7)/8,(height+7)/8,1);
        VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.buffer=buffer;barrier.size=VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&barrier,0,nullptr);
        command=cmd;awaitSubmit.store(true);
        dlssnr::Log("[nr-snapshot] recorded call=%u %ux%u; awaiting actual submission",frame,width,height);
    }
    void submitted(VkQueue queue) {
        if(!awaitSubmit.exchange(false))return;
        // The caller still owns the application's queue external synchronization.
        // Zero additional command buffers; this fence follows the captured batch.
        auto result=vkQueueSubmit(queue,0,nullptr,fence);
        waiting=result==VK_SUCCESS;
        dlssnr::Log("[nr-snapshot] completion fence submit=%d",int(result));
    }
    bool write(const wchar_t* name,const void* data,size_t size) {
        std::wstring path=directory+name,tmp=path+L".tmp";
        auto f=CreateFileW(tmp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(f==INVALID_HANDLE_VALUE)return false;
        DWORD done=0;bool ok=WriteFile(f,data,DWORD(size),&done,nullptr) && done==size;
        if(ok)ok=FlushFileBuffers(f)!=0;CloseHandle(f);
        if(ok)ok=MoveFileExW(tmp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
        if(!ok)DeleteFileW(tmp.c_str());return ok;
    }
    void poll() {
        if(!waiting || finished)return;
        auto result=vkGetFenceStatus(c.device,fence);
        if(result==VK_NOT_READY)return;
        finished=true;
        if(result!=VK_SUCCESS){dlssnr::Log("[nr-snapshot] completion failed=%d; no dump written",int(result));return;}
        // Coherent memory plus shader-to-host barrier and the completed fence.
        // One disk write per process, only after an explicit capture request.
        auto meta=(const float*)mapped;
        char json[2048];
        int n=snprintf(json,sizeof(json),
          "{\"version\":1,\"width\":%u,\"height\":%u,\"frame\":%u,\"pid\":%lu,\"token\":%llu,"
          "\"first\":%llu,\"stride\":%llu,\"bytes\":%llu,\"format\":\"rgba16f-le\","
          "\"planes\":[\"original_hdr\",\"nr_input_srgb\",\"nr_output_srgb\",\"restored_hdr\"],"
          "\"exposure_valid\":%s,\"sharpness\":%.9g,\"intensity\":%.9g,\"tone\":%.9g,\"structure\":%.9g,"
          "\"skin\":%.9g,\"style\":%u,\"preset\":%u,\"automask\":%u}\n",
          width,height,frame,GetCurrentProcessId(),token,(unsigned long long)first,(unsigned long long)stride,(unsigned long long)bytes,
          std::isfinite(meta[0]) && meta[0]>0 && std::isfinite(meta[1]) && meta[1]>0?"true":"false",
          meta[2],tuning.intensity,tuning.localTone,tuning.localStructure,tuning.skinStructure,tuning.style,tuning.preset,tuning.autoMask);
        bool ok=n>0 && n<int(sizeof(json)) && write(L"hdr-snapshot.bin",mapped,size_t(bytes)) && write(L"hdr-snapshot.json",json,size_t(n));
        dlssnr::Log("[nr-snapshot] %s call=%u exposure=%.9g preExposure=%.9g sharpness=%.4f",
                    ok?"saved":"write failed",frame,meta[0],meta[1],meta[2]);
    }
    void shutdown() {
        if(!c.device)return;
        awaitSubmit=false;
        if(fence)vkDestroyFence(c.device,fence,nullptr);
        if(pipeline)vkDestroyPipeline(c.device,pipeline,nullptr);
        if(pipelineLayout)vkDestroyPipelineLayout(c.device,pipelineLayout,nullptr);
        if(pool)vkDestroyDescriptorPool(c.device,pool,nullptr);
        if(layout)vkDestroyDescriptorSetLayout(c.device,layout,nullptr);
        if(mapped)vkUnmapMemory(c.device,memory);
        if(buffer)vkDestroyBuffer(c.device,buffer,nullptr);
        if(memory)vkFreeMemory(c.device,memory,nullptr);
        c.device=nullptr;
    }
};
