#pragma once
#include <filesystem>
#include <chrono>
#include <unistd.h>
#include "camera_draw_spv.h"
// Included after the test's OK macro. No rendering is needed to check byte-accurate
// host snapshots; the diagnostic correctly labels a bound set as unproven use.
static void CameraGpuTest(VkPhysicalDevice gpu,VkDevice d,uint32_t family,bool capture=true) {
    VkPhysicalDeviceMemoryProperties props{};vkGetPhysicalDeviceMemoryProperties(gpu,&props);
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=4096;bi.usage=VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    VkBuffer buffer{};OK(vkCreateBuffer(d,&bi,nullptr,&buffer));VkMemoryRequirements req{};vkGetBufferMemoryRequirements(d,buffer,&req);
    uint32_t type=0;
    auto required=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if(std::getenv("DLSSFG_TEST_DEVICE_LOCAL_CAMERA"))required|=VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    for(;type<props.memoryTypeCount;++type) if((req.memoryTypeBits&(1u<<type)) && (props.memoryTypes[type].propertyFlags&required)==required)break;
    if(type==props.memoryTypeCount)std::exit(10);
    const VkDeviceSize bindOffset=std::max<VkDeviceSize>(req.alignment,4096),mapOffset=bindOffset/2,descriptorOffset=256;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size+bindOffset;ai.memoryTypeIndex=type;
    VkDeviceMemory memory{};OK(vkAllocateMemory(d,&ai,nullptr,&memory));
    VkBindBufferMemoryInfo bind{VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO};bind.buffer=buffer;bind.memory=memory;bind.memoryOffset=bindOffset;
    OK(vkBindBufferMemory2(d,1,&bind));
    void* pointer{};OK(vkMapMemory(d,memory,mapOffset,VK_WHOLE_SIZE,0,&pointer));
    VkDescriptorSetLayoutBinding lb{29,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT,nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};li.bindingCount=1;li.pBindings=&lb;
    VkDescriptorSetLayout layout{};OK(vkCreateDescriptorSetLayout(d,&li,nullptr,&layout));
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pli.setLayoutCount=1;pli.pSetLayouts=&layout;
    VkPipelineLayout pl{};OK(vkCreatePipelineLayout(d,&pli,nullptr,&pl));
    const bool drawAssociations=std::getenv("DLSSFG_TEST_DRAW_ASSOCIATIONS")!=nullptr;
    const bool renderPass2=std::getenv("DLSSFG_TEST_RENDERPASS2")!=nullptr;
    VkPipeline pipeline{};VkRenderPass renderPass{};VkFramebuffer framebuffer{};
    std::array<VkImage,2> targetImages{};std::array<VkImageView,2> targetViews{};std::array<VkDeviceMemory,2> targetMemory{};
    if(drawAssociations){
        VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};sm.codeSize=sizeof(cameraDrawSpirv);sm.pCode=cameraDrawSpirv;
        VkShaderModule shader{};OK(vkCreateShaderModule(d,&sm,nullptr,&shader));
        std::array<VkAttachmentDescription,2> attachments{};
        for(unsigned i=0;i<2;++i){
            auto format=i?VK_FORMAT_D32_SFLOAT:VK_FORMAT_R16G16_SFLOAT;
            VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};image.imageType=VK_IMAGE_TYPE_2D;image.format=format;
            image.extent={32,24,1};image.mipLevels=image.arrayLayers=1;image.samples=VK_SAMPLE_COUNT_1_BIT;
            image.usage=(i?VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT:VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            OK(vkCreateImage(d,&image,nullptr,&targetImages[i]));VkMemoryRequirements mr{};vkGetImageMemoryRequirements(d,targetImages[i],&mr);
            uint32_t mt=0;while(mt<props.memoryTypeCount && !(mr.memoryTypeBits&(1u<<mt)))++mt;
            VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};allocation.allocationSize=mr.size;allocation.memoryTypeIndex=mt;
            OK(vkAllocateMemory(d,&allocation,nullptr,&targetMemory[i]));OK(vkBindImageMemory(d,targetImages[i],targetMemory[i],0));
            VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};view.image=targetImages[i];view.viewType=VK_IMAGE_VIEW_TYPE_2D;view.format=format;
            view.subresourceRange={i?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};OK(vkCreateImageView(d,&view,nullptr,&targetViews[i]));
            auto& a=attachments[i];a.format=format;a.samples=VK_SAMPLE_COUNT_1_BIT;a.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;a.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
            a.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;a.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
            a.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;a.finalLayout=i?VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
        VkAttachmentReference color{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},depth{1,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sub.colorAttachmentCount=1;sub.pColorAttachments=&color;sub.pDepthStencilAttachment=&depth;
        VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};rp.subpassCount=1;rp.pSubpasses=&sub;rp.attachmentCount=2;rp.pAttachments=attachments.data();
        if(renderPass2){
            std::array<VkAttachmentDescription2,2> as{};for(unsigned i=0;i<2;++i){auto& a=as[i];const auto& b=attachments[i];a.sType=VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
                a.format=b.format;a.samples=b.samples;a.loadOp=b.loadOp;a.storeOp=b.storeOp;a.stencilLoadOp=b.stencilLoadOp;a.stencilStoreOp=b.stencilStoreOp;a.initialLayout=b.initialLayout;a.finalLayout=b.finalLayout;}
            VkAttachmentReference2 c{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2},z{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2};c.attachment=0;c.layout=color.layout;z.attachment=1;z.layout=depth.layout;
            VkSubpassDescription2 s{VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2};s.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;s.colorAttachmentCount=1;s.pColorAttachments=&c;s.pDepthStencilAttachment=&z;
            VkRenderPassCreateInfo2 rp2Info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2};rp2Info.attachmentCount=2;rp2Info.pAttachments=as.data();rp2Info.subpassCount=1;rp2Info.pSubpasses=&s;
            OK(vkCreateRenderPass2(d,&rp2Info,nullptr,&renderPass));
        }else{OK(vkCreateRenderPass(d,&rp,nullptr,&renderPass));}
        VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};fb.renderPass=renderPass;fb.width=fb.height=fb.layers=1;
        fb.attachmentCount=2;fb.pAttachments=targetViews.data();
        OK(vkCreateFramebuffer(d,&fb,nullptr,&framebuffer));
        VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};stage.stage=VK_SHADER_STAGE_VERTEX_BIT;stage.module=shader;stage.pName="main";
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};rs.rasterizerDiscardEnable=VK_TRUE;rs.lineWidth=1;
        VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};pi.stageCount=1;pi.pStages=&stage;pi.layout=pl;pi.renderPass=renderPass;
        pi.pVertexInputState=&vi;pi.pInputAssemblyState=&ia;pi.pRasterizationState=&rs;
        OK(vkCreateGraphicsPipelines(d,VK_NULL_HANDLE,1,&pi,nullptr,&pipeline));
        vkDestroyShaderModule(d,shader,nullptr); // Pipeline metadata must outlive its shader module.
    }
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1};VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pi.maxSets=1;pi.poolSizeCount=1;pi.pPoolSizes=&ps;
    VkDescriptorPool pool{};OK(vkCreateDescriptorPool(d,&pi,nullptr,&pool));
    VkDescriptorSetAllocateInfo si{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};si.descriptorPool=pool;si.descriptorSetCount=1;si.pSetLayouts=&layout;
    VkDescriptorSet set{};OK(vkAllocateDescriptorSets(d,&si,&set));
    VkDescriptorBufferInfo info{buffer,descriptorOffset,464};VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=set;write.dstBinding=29;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;write.pBufferInfo=&info;
    vkUpdateDescriptorSets(d,1,&write,0,nullptr);
    VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};cp.queueFamilyIndex=family;cp.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool commandPool{};OK(vkCreateCommandPool(d,&cp,nullptr,&commandPool));
    VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=commandPool;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;
    VkCommandBuffer primary{},secondary{};OK(vkAllocateCommandBuffers(d,&ca,&primary));ca.level=VK_COMMAND_BUFFER_LEVEL_SECONDARY;OK(vkAllocateCommandBuffers(d,&ca,&secondary));
    VkQueue queue{};vkGetDeviceQueue(d,family,0,&queue);
    auto record=[&](bool withSet,bool useSecondary) {
        OK(vkResetCommandPool(d,commandPool,0));
        VkCommandBufferInheritanceInfo inherit{VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO};inherit.renderPass=renderPass;inherit.framebuffer=framebuffer;
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.pInheritanceInfo=&inherit;
        auto commands=[&](VkCommandBuffer cb){
            if(withSet)vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,pl,0,1,&set,0,nullptr);
            if(drawAssociations && withSet){vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);vkCmdDraw(cb,3,1,0,0);}
        };
        if(useSecondary){if(drawAssociations)begin.flags=VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
            OK(vkBeginCommandBuffer(secondary,&begin));commands(secondary);OK(vkEndCommandBuffer(secondary));}
        begin.flags=0;
        OK(vkBeginCommandBuffer(primary,&begin));
        VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};rp.renderPass=renderPass;rp.framebuffer=framebuffer;rp.renderArea.extent={1,1};
        std::array<VkClearValue,2> clears{};clears[1].depthStencil.depth=1;rp.clearValueCount=2;rp.pClearValues=clears.data();
        VkSubpassBeginInfo start{VK_STRUCTURE_TYPE_SUBPASS_BEGIN_INFO};start.contents=useSecondary?VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS:VK_SUBPASS_CONTENTS_INLINE;
        if(drawAssociations){if(renderPass2)vkCmdBeginRenderPass2(primary,&rp,&start);else vkCmdBeginRenderPass(primary,&rp,start.contents);}
        if(useSecondary)vkCmdExecuteCommands(primary,1,&secondary);
        else commands(primary);
        if(drawAssociations){if(renderPass2){VkSubpassEndInfo end{VK_STRUCTURE_TYPE_SUBPASS_END_INFO};vkCmdEndRenderPass2(primary,&end);}else vkCmdEndRenderPass(primary);}
        OK(vkEndCommandBuffer(primary));
    };
    bool useSubmit2=false;
    auto submit=[&] {
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};si.commandBufferCount=1;si.pCommandBuffers=&primary;
        if(useSubmit2) {
            VkCommandBufferSubmitInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};cb.commandBuffer=primary;
            VkSubmitInfo2 s2{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};s2.commandBufferInfoCount=1;s2.pCommandBufferInfos=&cb;
            OK(vkQueueSubmit2(queue,1,&s2,VK_NULL_HANDLE));
        } else {OK(vkQueueSubmit(queue,1,&si,VK_NULL_HANDLE));}
        OK(vkQueueWaitIdle(queue));
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
    };
    auto fill=[&](float base) {std::array<float,116> values{};for(size_t i=0;i<values.size();++i)values[i]=base+float(i)*.25f;
        std::memcpy(static_cast<char*>(pointer)+bindOffset+descriptorOffset-mapOffset,values.data(),sizeof(values));};
    record(true,false);fill(1);submit(); // No request yet: no sample file may exist.
    if(capture) {
    const char* root=std::getenv("DLSSFG_DISCOVERY_DIR");if(!root)std::exit(11);
    std::filesystem::path session;
    for(auto& dir:std::filesystem::directory_iterator(root))if(dir.path().filename().string().find("rdr2-"+std::to_string(getpid())+"-")==0)session=dir.path();
    if(session.empty())std::exit(12);
    for(auto& f:std::filesystem::directory_iterator(session/"camera"))if(f.path().extension()==".jsonl")std::exit(13);
    {std::ofstream request(session/"camera/request.json");request<<"{\"id\":1,\"device\":1,\"duration_ms\":1200,\"max_samples\":32,\"require_draw\":"<<(drawAssociations?"true":"false")<<"}";}
    }
    fill(1);submit();
    fill(2);record(true,true);useSubmit2=true;submit();useSubmit2=false; // Secondary command traversal.
    vkUnmapMemory(d,memory);submit(); // Inaccessible mapping is skipped.
    OK(vkMapMemory(d,memory,mapOffset,VK_WHOLE_SIZE,0,&pointer));fill(3);
    record(false,false);submit(); // Pool reset must discard the old set references.
    record(true,false);submit();
    std::this_thread::sleep_for(std::chrono::milliseconds(850));submit(); // Close timed request.
    vkDestroyCommandPool(d,commandPool,nullptr);vkDestroyDescriptorPool(d,pool,nullptr);
    if(pipeline)vkDestroyPipeline(d,pipeline,nullptr);
    if(framebuffer)vkDestroyFramebuffer(d,framebuffer,nullptr);
    if(renderPass)vkDestroyRenderPass(d,renderPass,nullptr);
    for(unsigned i=0;i<2;++i)if(targetImages[i]){vkDestroyImageView(d,targetViews[i],nullptr);vkDestroyImage(d,targetImages[i],nullptr);vkFreeMemory(d,targetMemory[i],nullptr);}
    vkDestroyPipelineLayout(d,pl,nullptr);vkDestroyDescriptorSetLayout(d,layout,nullptr);
    vkUnmapMemory(d,memory);vkDestroyBuffer(d,buffer,nullptr);vkFreeMemory(d,memory,nullptr);
    puts("PASS: requested camera snapshots, binding/map/descriptor offsets, secondary command, unmap and pool reset");
}
