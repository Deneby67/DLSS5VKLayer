#include <vulkan/vulkan.h>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

int main(int argc,char** argv) {
    if (argc!=2) return 2;
    const bool expect=std::string(argv[1])=="captured";
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; VkInstance instance{};
    VkResult result=vkCreateInstance(&ci,nullptr,&instance);
    if (result) { std::printf("instance=%d\n",result); return 3; }
    uint32_t count=0; vkEnumeratePhysicalDevices(instance,&count,nullptr);
    std::vector<VkPhysicalDevice> devices(count); vkEnumeratePhysicalDevices(instance,&count,devices.data());
    bool created=false;
    for (auto gpu:devices) {
        VkPhysicalDeviceProperties props{}; vkGetPhysicalDeviceProperties(gpu,&props);
        if (props.vendorID!=0x10de) continue;
        uint32_t queues=0; vkGetPhysicalDeviceQueueFamilyProperties(gpu,&queues,nullptr);
        std::vector<VkQueueFamilyProperties> families(queues); vkGetPhysicalDeviceQueueFamilyProperties(gpu,&queues,families.data());
        uint32_t q=0; while (q<queues && !(families[q].queueFlags&VK_QUEUE_GRAPHICS_BIT)) ++q;
        if (q==queues) continue;
        float priority=1; VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue.queueFamilyIndex=q; queue.queueCount=1; queue.pQueuePriorities=&priority;
        VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dc.queueCreateInfoCount=1; dc.pQueueCreateInfos=&queue;
        VkDevice device{}; result=vkCreateDevice(gpu,&dc,nullptr,&device);
        std::printf("device=%d GPU=%s\n",result,props.deviceName);
        if (result==VK_SUCCESS) { created=true; vkDestroyDevice(device,nullptr); }
        break;
    }
    std::ifstream file("/proc/self/maps"); std::string line; bool renderdoc=false;
    while (std::getline(file,line)) if (line.find("librenderdoc.so")!=std::string::npos) renderdoc=true;
    vkDestroyInstance(instance,nullptr);
    std::printf("RenderDoc mapped=%d expected=%d\n",renderdoc,expect);
    return created && renderdoc==expect?0:4;
}
