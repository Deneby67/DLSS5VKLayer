// Native Vulkan loader gate. Do not load RenderDoc (or run its constructors)
// outside RDR2: in particular the Rockstar launcher uses DXVK too.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <dlfcn.h>
#include <unistd.h>
#include <mutex>
#include <unordered_map>

// Some loader versions propagate rejected negotiation as an instance failure.
// Use a transparent forwarding layer for excluded processes instead.
struct InstanceNext { VkInstance instance; PFN_vkGetInstanceProcAddr gipa; PFN_GetPhysicalDeviceProcAddr gpdpa; };
static std::mutex dispatchMutex;
static std::unordered_map<void*,InstanceNext> instances;
static std::unordered_map<void*,PFN_vkGetDeviceProcAddr> devices;
template<class T> static void* Key(T handle) { return handle?*reinterpret_cast<void**>(handle):nullptr; }
static PFN_vkVoidFunction VKAPI_CALL BypassGipa(VkInstance,const char*);
static PFN_vkVoidFunction VKAPI_CALL BypassGdpa(VkDevice,const char*);
static InstanceNext FindInstance(VkInstance instance) {
    std::lock_guard<std::mutex> lock(dispatchMutex);
    auto found=instances.find(Key(instance));
    return found==instances.end()?InstanceNext{}:found->second;
}
static VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo* ci,const VkAllocationCallbacks* ac,VkInstance* out) {
    auto link=reinterpret_cast<const VkLayerInstanceCreateInfo*>(ci->pNext);
    while (link && (link->sType!=VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO || link->function!=VK_LAYER_LINK_INFO))
        link=reinterpret_cast<const VkLayerInstanceCreateInfo*>(link->pNext);
    if (!link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    const auto next=*link->u.pLayerInfo;
    auto create=reinterpret_cast<PFN_vkCreateInstance>(next.pfnNextGetInstanceProcAddr(nullptr,"vkCreateInstance"));
    const_cast<VkLayerInstanceCreateInfo*>(link)->u.pLayerInfo=next.pNext;
    const VkResult r=create(ci,ac,out);
    if (r==VK_SUCCESS) {
        std::lock_guard<std::mutex> lock(dispatchMutex);
        instances[Key(*out)]={*out,next.pfnNextGetInstanceProcAddr,next.pfnNextGetPhysicalDeviceProcAddr};
    }
    return r;
}
static void VKAPI_CALL DestroyInstance(VkInstance instance,const VkAllocationCallbacks* ac) {
    const auto next=FindInstance(instance);
    if (!next.gipa) return;
    auto destroy=reinterpret_cast<PFN_vkDestroyInstance>(next.gipa(instance,"vkDestroyInstance"));
    { std::lock_guard<std::mutex> lock(dispatchMutex); instances.erase(Key(instance)); }
    destroy(instance,ac);
}
static VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice physical,const VkDeviceCreateInfo* ci,const VkAllocationCallbacks* ac,VkDevice* out) {
    auto link=reinterpret_cast<const VkLayerDeviceCreateInfo*>(ci->pNext);
    while (link && (link->sType!=VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO || link->function!=VK_LAYER_LINK_INFO))
        link=reinterpret_cast<const VkLayerDeviceCreateInfo*>(link->pNext);
    if (!link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    const auto next=*link->u.pLayerInfo;
    auto create=reinterpret_cast<PFN_vkCreateDevice>(next.pfnNextGetInstanceProcAddr(nullptr,"vkCreateDevice"));
    const_cast<VkLayerDeviceCreateInfo*>(link)->u.pLayerInfo=next.pNext;
    const VkResult r=create(physical,ci,ac,out);
    if (r==VK_SUCCESS) {
        std::lock_guard<std::mutex> lock(dispatchMutex);
        devices[Key(*out)]=next.pfnNextGetDeviceProcAddr;
    }
    return r;
}
static PFN_vkGetDeviceProcAddr FindDevice(VkDevice device) {
    std::lock_guard<std::mutex> lock(dispatchMutex);
    auto found=devices.find(Key(device)); return found==devices.end()?nullptr:found->second;
}
static void VKAPI_CALL DestroyDevice(VkDevice device,const VkAllocationCallbacks* ac) {
    auto next=FindDevice(device);
    if (!next) return;
    auto destroy=reinterpret_cast<PFN_vkDestroyDevice>(next(device,"vkDestroyDevice"));
    { std::lock_guard<std::mutex> lock(dispatchMutex); devices.erase(Key(device)); }
    destroy(device,ac);
}
static PFN_vkVoidFunction VKAPI_CALL BypassGipa(VkInstance instance,const char* name) {
    if (!name) return nullptr;
    if (!std::strcmp(name,"vkGetInstanceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(BypassGipa);
    if (!std::strcmp(name,"vkGetDeviceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(BypassGdpa);
    if (!std::strcmp(name,"vkCreateInstance")) return reinterpret_cast<PFN_vkVoidFunction>(CreateInstance);
    if (!std::strcmp(name,"vkDestroyInstance")) return reinterpret_cast<PFN_vkVoidFunction>(DestroyInstance);
    if (!std::strcmp(name,"vkCreateDevice")) return reinterpret_cast<PFN_vkVoidFunction>(CreateDevice);
    if (!std::strcmp(name,"vkDestroyDevice")) return reinterpret_cast<PFN_vkVoidFunction>(DestroyDevice);
    auto next=FindInstance(instance); return next.gipa?next.gipa(instance,name):nullptr;
}
static PFN_vkVoidFunction VKAPI_CALL BypassGdpa(VkDevice device,const char* name) {
    if (!name) return nullptr;
    if (!std::strcmp(name,"vkGetDeviceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(BypassGdpa);
    if (!std::strcmp(name,"vkDestroyDevice")) return reinterpret_cast<PFN_vkVoidFunction>(DestroyDevice);
    auto next=FindDevice(device); return next?next(device,name):nullptr;
}
static PFN_vkVoidFunction VKAPI_CALL BypassGpdpa(VkInstance instance,const char* name) {
    auto next=FindInstance(instance); return next.gpdpa?next.gpdpa(instance,name):nullptr;
}

static bool IsRdr2() {
    char path[4096]{};
    FILE* f=std::fopen("/proc/self/cmdline","rb");
    if (!f) return false;
    const size_t size=std::fread(path,1,sizeof(path),f);
    std::fclose(f);
    // Reject unreadable/truncated argv[0]; Wine exposes the Windows image here.
    if (!size || !std::memchr(path,0,size)) return false;
    const char* base=path;
    for (const char* c=path;*c;++c) if (*c=='/' || *c=='\\') base=c+1;
    return !strcasecmp(base,"RDR2.exe");
}
static void* Backend() {
    if (!IsRdr2()) return nullptr;
    static void* library=[] {
        const char* path=std::getenv("DLSSFG_RENDERDOC_LIBRARY");
        if (!path || path[0]!='/') {
            std::fprintf(stderr,"[rdr2-capture] pid=%d missing absolute backend path\n",getpid());
            return static_cast<void*>(nullptr);
        }
        void* handle=dlopen(path,RTLD_NOW|RTLD_LOCAL);
        std::fprintf(stderr,"[rdr2-capture] pid=%d RDR2 backend %s\n",getpid(),handle?"loaded":dlerror());
        return handle; // Kept loaded for the lifetime of all Vulkan objects.
    }();
    return library;
}
template<class T> static T Symbol(const char* name) {
    auto backend=Backend();
    return backend?reinterpret_cast<T>(dlsym(backend,name)):nullptr;
}
#define EXPORT extern "C" __attribute__((visibility("default")))
static VkResult BypassNegotiation(VkNegotiateLayerInterface* info) {
    if (!info || info->loaderLayerInterfaceVersion<2) return VK_ERROR_INITIALIZATION_FAILED;
    info->loaderLayerInterfaceVersion=2;
    info->pfnGetInstanceProcAddr=BypassGipa;
    info->pfnGetDeviceProcAddr=BypassGdpa;
    info->pfnGetPhysicalDeviceProcAddr=BypassGpdpa;
    return VK_SUCCESS;
}
EXPORT VKAPI_ATTR VkResult VKAPI_CALL DLSSFG_Negotiate(VkNegotiateLayerInterface* info) {
    if (!IsRdr2()) {
        static std::atomic_flag logged=ATOMIC_FLAG_INIT;
        if (!logged.test_and_set()) std::fprintf(stderr,"[rdr2-capture] pid=%d skipped non-RDR2 process\n",getpid());
        return BypassNegotiation(info);
    }
    auto fn=Symbol<PFN_vkNegotiateLoaderLayerInterfaceVersion>(
        "VK_LAYER_RENDERDOC_CaptureNegotiateLoaderLayerInterfaceVersion");
    return fn?fn(info):BypassNegotiation(info);
}
EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL DLSSFG_GetInstanceProcAddr(VkInstance instance,const char* name) {
    auto fn=Symbol<PFN_vkGetInstanceProcAddr>("VK_LAYER_RENDERDOC_CaptureGetInstanceProcAddr");
    return fn?fn(instance,name):BypassGipa(instance,name);
}
EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL DLSSFG_GetDeviceProcAddr(VkDevice device,const char* name) {
    auto fn=Symbol<PFN_vkGetDeviceProcAddr>("VK_LAYER_RENDERDOC_CaptureGetDeviceProcAddr");
    return fn?fn(device,name):BypassGdpa(device,name);
}
using PreEnumerate=VkResult(VKAPI_PTR*)(const VkEnumerateInstanceExtensionPropertiesChain*,const char*,uint32_t*,VkExtensionProperties*);
EXPORT VKAPI_ATTR VkResult VKAPI_CALL DLSSFG_EnumerateInstanceExtensionProperties(
    const VkEnumerateInstanceExtensionPropertiesChain* chain,const char* layer,uint32_t* count,VkExtensionProperties* properties) {
    // The loader may enumerate extensions before negotiating a layer interface.
    // Gate that entry point too, otherwise RenderDoc reaches Launcher.exe early.
    if (IsRdr2()) {
        auto fn=Symbol<PreEnumerate>("VK_LAYER_RENDERDOC_CaptureEnumerateInstanceExtensionProperties");
        if (fn) return fn(chain,layer,count,properties);
    }
    return chain->CallDown(layer,count,properties);
}
