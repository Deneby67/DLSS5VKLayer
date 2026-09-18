// Read only this test's own host-visible Vulkan allocations, never another process.
#include <vulkan/vulkan.h>
#include <array>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/uio.h>
#include <unistd.h>
#include "../layer_linux/src/cpu_snapshot_reader.h"
#define OK(x) do {auto r=(x);if(r!=VK_SUCCESS){fprintf(stderr,"%s: %d\n",#x,r);return 2;}}while(0)
static bool test(void* pointer, const char* label) {
    std::array<unsigned char,464> expected{},out{};
    for(size_t i=0;i<expected.size();++i)expected[i]=(i*37+11)%256;
    memcpy(pointer,expected.data(),expected.size());
    iovec local{out.data(),out.size()},remote{pointer,out.size()};
    errno=0;auto n=process_vm_readv(getpid(),&local,1,&remote,1,0);int e=errno;
    printf("%s vm_readv=%zd errno=%d match=%d ",label,n,e,out==expected);
    int fds[2];if(pipe2(fds,O_NONBLOCK|O_CLOEXEC))return false;
    errno=0;auto w=write(fds[1],pointer,out.size());e=errno;
    out.fill(0);auto r=read(fds[0],out.data(),out.size());
    close(fds[0]);close(fds[1]);
    printf("pipe_write=%zd errno=%d pipe_read=%zd match=%d\n",w,e,r,out==expected);
    bool matched=w==ssize_t(out.size()) && r==ssize_t(out.size()) && out==expected;
    dlssfg::CpuSnapshotReader reader;out.fill(0);
    auto result=reader.read(pointer,out.data(),out.size());
    printf("reader ok=%d pipe=%d vm_errno=%d error=%d match=%d\n",result.ok,result.usedPipe,result.vmError,result.error,out==expected);
    return matched && result.ok && out==expected;
}
int main() {
    std::array<unsigned char,464> host{};bool passed=test(host.data(),"host");
    VkApplicationInfo a{VK_STRUCTURE_TYPE_APPLICATION_INFO};a.apiVersion=VK_API_VERSION_1_3;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ci.pApplicationInfo=&a;
    VkInstance instance{};OK(vkCreateInstance(&ci,nullptr,&instance));
    uint32_t count=0;OK(vkEnumeratePhysicalDevices(instance,&count,nullptr));
    std::vector<VkPhysicalDevice> gpus(count);OK(vkEnumeratePhysicalDevices(instance,&count,gpus.data()));
    VkPhysicalDevice gpu{};for(auto g:gpus){VkPhysicalDeviceProperties p{};vkGetPhysicalDeviceProperties(g,&p);if(p.vendorID==0x10de)gpu=g;}
    if(!gpu)return 3;
    uint32_t family=0;vkGetPhysicalDeviceQueueFamilyProperties(gpu,&count,nullptr);
    std::vector<VkQueueFamilyProperties> families(count);vkGetPhysicalDeviceQueueFamilyProperties(gpu,&count,families.data());
    while(family<count && !(families[family].queueFlags&VK_QUEUE_GRAPHICS_BIT))++family;
    if(family==count)return 4;
    float priority=1;VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qi.queueFamilyIndex=family;qi.queueCount=1;qi.pQueuePriorities=&priority;
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};di.queueCreateInfoCount=1;di.pQueueCreateInfos=&qi;
    VkDevice device{};OK(vkCreateDevice(gpu,&di,nullptr,&device));
    VkPhysicalDeviceMemoryProperties props{};vkGetPhysicalDeviceMemoryProperties(gpu,&props);
    for(uint32_t t=0;t<props.memoryTypeCount;++t){
        auto flags=props.memoryTypes[t].propertyFlags;
        if((flags&6)!=6)continue;
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=4096;bi.usage=VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        VkBuffer buffer{};OK(vkCreateBuffer(device,&bi,nullptr,&buffer));
        VkMemoryRequirements req{};vkGetBufferMemoryRequirements(device,buffer,&req);
        if(!(req.memoryTypeBits&(1u<<t))){vkDestroyBuffer(device,buffer,nullptr);continue;}
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;ai.memoryTypeIndex=t;
        VkDeviceMemory memory{};OK(vkAllocateMemory(device,&ai,nullptr,&memory));OK(vkBindBufferMemory(device,buffer,memory,0));
        void* pointer{};OK(vkMapMemory(device,memory,0,VK_WHOLE_SIZE,0,&pointer));
        char label[80];snprintf(label,sizeof(label),"memory_type=%u flags=0x%x",t,flags);
        passed=test(pointer,label)&&passed;
        vkUnmapMemory(device,memory);vkDestroyBuffer(device,buffer,nullptr);vkFreeMemory(device,memory,nullptr);
    }
    vkDestroyDevice(device,nullptr);vkDestroyInstance(instance,nullptr);
    return passed?0:5;
}
