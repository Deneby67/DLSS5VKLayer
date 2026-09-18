#pragma once
#include <filesystem>
#include <chrono>
#include <unistd.h>
// Included after the test's OK macro. No rendering is needed to check byte-accurate
// host snapshots; the diagnostic correctly labels a bound set as unproven use.
static void CameraGpuTest(VkPhysicalDevice gpu,VkDevice d,uint32_t family,bool capture=true) {
    VkPhysicalDeviceMemoryProperties props{};vkGetPhysicalDeviceMemoryProperties(gpu,&props);
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=4096;bi.usage=VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    VkBuffer buffer{};OK(vkCreateBuffer(d,&bi,nullptr,&buffer));VkMemoryRequirements req{};vkGetBufferMemoryRequirements(d,buffer,&req);
    uint32_t type=0;
    for(;type<props.memoryTypeCount;++type) if((req.memoryTypeBits&(1u<<type)) && (props.memoryTypes[type].propertyFlags&(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))==(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))break;
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
        VkCommandBufferInheritanceInfo inherit{VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO};
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.pInheritanceInfo=&inherit;
        if(useSecondary){OK(vkBeginCommandBuffer(secondary,&begin));if(withSet)vkCmdBindDescriptorSets(secondary,VK_PIPELINE_BIND_POINT_GRAPHICS,pl,0,1,&set,0,nullptr);OK(vkEndCommandBuffer(secondary));}
        OK(vkBeginCommandBuffer(primary,&begin));
        if(useSecondary)vkCmdExecuteCommands(primary,1,&secondary);
        else if(withSet)vkCmdBindDescriptorSets(primary,VK_PIPELINE_BIND_POINT_GRAPHICS,pl,0,1,&set,0,nullptr);
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
    {std::ofstream request(session/"camera/request.json");request<<"{\"id\":1,\"device\":1,\"duration_ms\":1200,\"max_samples\":32}";}
    }
    fill(1);submit();
    fill(2);record(true,true);useSubmit2=true;submit();useSubmit2=false; // Secondary command traversal.
    vkUnmapMemory(d,memory);submit(); // Inaccessible mapping is skipped.
    OK(vkMapMemory(d,memory,mapOffset,VK_WHOLE_SIZE,0,&pointer));fill(3);
    record(false,false);submit(); // Pool reset must discard the old set references.
    record(true,false);submit();
    std::this_thread::sleep_for(std::chrono::milliseconds(850));submit(); // Close timed request.
    vkDestroyCommandPool(d,commandPool,nullptr);vkDestroyDescriptorPool(d,pool,nullptr);
    vkDestroyPipelineLayout(d,pl,nullptr);vkDestroyDescriptorSetLayout(d,layout,nullptr);
    vkUnmapMemory(d,memory);vkDestroyBuffer(d,buffer,nullptr);vkFreeMemory(d,memory,nullptr);
    puts("PASS: requested camera snapshots, binding/map/descriptor offsets, secondary command, unmap and pool reset");
}
