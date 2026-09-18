// Real Vulkan resource lifetime/dispatch test; deliberately never submits GPU work.
#include <vulkan/vulkan.h>
#include <cstdio>
#include <array>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#define OK(expr) do { auto r=(expr); if(r!=VK_SUCCESS) { fprintf(stderr,"%s = %d at %d\n",#expr,r,__LINE__); std::exit(3); } } while(0)
#include "camera_probe_gpu.h"
int main(int argc,char** argv) {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.apiVersion=VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ici.pApplicationInfo=&app; VkInstance instance{};
    OK(vkCreateInstance(&ici,nullptr,&instance));
    // Exercise a hook obtained BEFORE device registration, including failed diagnostics.
    auto createBuffer=(PFN_vkCreateBuffer)vkGetInstanceProcAddr(instance,"vkCreateBuffer");
    uint32_t count=0; OK(vkEnumeratePhysicalDevices(instance,&count,nullptr));
    std::vector<VkPhysicalDevice> devices(count); OK(vkEnumeratePhysicalDevices(instance,&count,devices.data()));
    VkPhysicalDevice physical{};
    for(auto gpu:devices) { VkPhysicalDeviceProperties p{}; vkGetPhysicalDeviceProperties(gpu,&p); if(p.vendorID==0x10de) { physical=gpu; break; } }
    if(!physical) return 4;
    uint32_t families=0; vkGetPhysicalDeviceQueueFamilyProperties(physical,&families,nullptr);
    std::vector<VkQueueFamilyProperties> queues(families); vkGetPhysicalDeviceQueueFamilyProperties(physical,&families,queues.data());
    uint32_t family=0; while(family<families && !(queues[family].queueFlags&VK_QUEUE_COMPUTE_BIT)) ++family;
    if(family==families) return 5;
    float priority=1; VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex=family; qi.queueCount=1; qi.pQueuePriorities=&priority;
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; di.queueCreateInfoCount=1; di.pQueueCreateInfos=&qi;
    VkPhysicalDeviceSynchronization2Features sync{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES};sync.synchronization2=VK_TRUE;di.pNext=&sync;
    VkDevice device{}; OK(vkCreateDevice(physical,&di,nullptr,&device));
    auto allocate=[&](VkMemoryRequirements req) {
        VkPhysicalDeviceMemoryProperties p{}; vkGetPhysicalDeviceMemoryProperties(physical,&p);
        uint32_t type=0; while(type<p.memoryTypeCount && !(req.memoryTypeBits&(1u<<type))) ++type;
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize=req.size; ai.memoryTypeIndex=type;
        VkDeviceMemory memory{}; OK(vkAllocateMemory(device,&ai,nullptr,&memory)); return memory;
    };
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size=256; bi.usage=VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    VkBuffer buffer{}; OK(createBuffer(device,&bi,nullptr,&buffer));
    VkMemoryRequirements req{}; vkGetBufferMemoryRequirements(device,buffer,&req); auto memory=allocate(req);
    OK(vkBindBufferMemory(device,buffer,memory,0));
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ii.imageType=VK_IMAGE_TYPE_2D; ii.format=VK_FORMAT_D32_SFLOAT;
    ii.extent={64,48,1}; ii.mipLevels=1; ii.arrayLayers=1; ii.samples=VK_SAMPLE_COUNT_1_BIT;
    ii.usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
    VkImage image{}; OK(vkCreateImage(device,&ii,nullptr,&image)); vkGetImageMemoryRequirements(device,image,&req); auto imemory=allocate(req);
    OK(vkBindImageMemory(device,image,imemory,0));
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image=image; vi.viewType=VK_IMAGE_VIEW_TYPE_2D;
    vi.format=ii.format; vi.subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1}; VkImageView view{};
    OK(vkCreateImageView(device,&vi,nullptr,&view));
    VkDescriptorSetLayoutBinding binding{3,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; li.bindingCount=1; li.pBindings=&binding;
    VkDescriptorSetLayout layout{}; OK(vkCreateDescriptorSetLayout(device,&li,nullptr,&layout));
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pli.setLayoutCount=1; pli.pSetLayouts=&layout;
    VkPipelineLayout pl{}; OK(vkCreatePipelineLayout(device,&pli,nullptr,&pl));
    // SPIR-V 1.0: void main() {}, compute local size 1x1x1.
    const uint32_t shader[]={0x07230203,0x10000,0,6,0,0x20011,1,0x3000e,0,1,
        0x5000f,5,4,0x6e69616d,0,0x60010,4,17,1,1,1,0x20013,1,0x30021,2,1,
        0x50036,1,4,0,2,0x200f8,5,0x100fd,0x10038};
    VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; si.codeSize=sizeof(shader); si.pCode=shader;
    VkShaderModule module{},duplicate{}; OK(vkCreateShaderModule(device,&si,nullptr,&module)); OK(vkCreateShaderModule(device,&si,nullptr,&duplicate));
    VkComputePipelineCreateInfo pci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; pci.layout=pl;
    pci.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; pci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT; pci.stage.module=module; pci.stage.pName="main";
    VkPipeline pipeline{}; OK(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&pci,nullptr,&pipeline));
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pi.flags=VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pi.maxSets=1; pi.poolSizeCount=1; pi.pPoolSizes=&ps; VkDescriptorPool pool{}; OK(vkCreateDescriptorPool(device,&pi,nullptr,&pool));
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; ai.descriptorPool=pool; ai.descriptorSetCount=1; ai.pSetLayouts=&layout;
    for(int iteration=0;iteration<3;++iteration) {
        VkDescriptorSet set{}; OK(vkAllocateDescriptorSets(device,&ai,&set));
        VkDescriptorBufferInfo info{buffer,0,256}; VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet=set; w.dstBinding=3; w.descriptorCount=1; w.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w.pBufferInfo=&info;
        const int repeats=argc>1 && iteration==0 && (!strcmp(argv[1],"limit") || !strcmp(argv[1],"camera"))?300000:1;
        for(int j=0;j<repeats;++j) vkUpdateDescriptorSets(device,1,&w,0,nullptr);
        if(iteration==0) OK(vkResetDescriptorPool(device,pool,0));
        else if(iteration==1) OK(vkFreeDescriptorSets(device,pool,1,&set));
    }
    vkDestroyDescriptorPool(device,pool,nullptr);
    // Concurrent resource metadata, and forwarding after a deliberately full log.
    std::vector<std::thread> threads;
    for(int i=0;i<4;++i) threads.emplace_back([&] { for(int j=0;j<32;++j) { VkBuffer b{}; OK(createBuffer(device,&bi,nullptr,&b)); vkDestroyBuffer(device,b,nullptr); } });
    for(auto& t:threads) t.join();
    if(argc>1 && (!strcmp(argv[1],"camera") || !strcmp(argv[1],"camera-pass"))) CameraGpuTest(physical,device,family,!strcmp(argv[1],"camera"));
    vkDestroyPipeline(device,pipeline,nullptr); vkDestroyShaderModule(device,module,nullptr); vkDestroyShaderModule(device,duplicate,nullptr);
    vkDestroyPipelineLayout(device,pl,nullptr); vkDestroyDescriptorSetLayout(device,layout,nullptr);
    vkDestroyImageView(device,view,nullptr); vkDestroyImage(device,image,nullptr); vkFreeMemory(device,imemory,nullptr);
    vkDestroyBuffer(device,buffer,nullptr); vkFreeMemory(device,memory,nullptr);
    std::ifstream maps("/proc/self/maps"); std::string line;
    bool validation=false;
    while(std::getline(maps,line)) {
        if(line.find("librenderdoc.so")!=std::string::npos) return 6;
        if(line.find("libVkLayer_khronos_validation.so")!=std::string::npos) validation=true;
    }
    if(std::getenv("DLSSFG_TEST_VALIDATION") && !validation) return 7;
    std::printf("Validation mapped=%d\n",validation);
    vkDestroyDevice(device,nullptr); vkDestroyInstance(instance,nullptr);
    puts("PASS: real Vulkan resources/pipeline, pool reset/free, concurrent lifetimes, no RenderDoc");
}
