// Application-local Vulkan forwarding shim. No per-draw hooks, submission or waits
// during steady-state rendering. The NGX observer calls DlssNrEvaluate explicitly.
#include "nr_color.h"
#include "params.h"
#include "color_overlay.h"
#include "../core/ngx_snippet.h"
#include "../core/guard.h"
#include <mutex>
#include <set>
namespace {
using namespace dlssnr;
using Evaluate=uint32_t(__cdecl*)(void*,const void*,const void*,void*);
HMODULE selfModule{};
bool enabled=false;
SRWLOCK stateLock=SRWLOCK_INIT,renderLock=SRWLOCK_INIT;
struct Lock { SRWLOCK* p; Lock(SRWLOCK& s):p(&s){AcquireSRWLockExclusive(p);} ~Lock(){ReleaseSRWLockExclusive(p);} };
PFN_vkGetInstanceProcAddr realGipa{};
PFN_vkGetDeviceProcAddr realGdpa{};
INIT_ONCE once=INIT_ONCE_STATIC_INIT;
BOOL CALLBACK Resolve(PINIT_ONCE,void*,void**) {
    // Wine's builtin vulkan-1 initializes user32 before using winevulkan.
    // Do the same outside our loader-lock entry point.
    auto user=LoadLibraryW(L"user32.dll");
    auto dpi=user?(UINT(WINAPI*)())GetProcAddress(user,"GetDpiForSystem"):nullptr;
    if(dpi) dpi();
    auto wine=LoadLibraryW(L"winevulkan.dll");
    if(!wine) return FALSE;
    realGipa=(PFN_vkGetInstanceProcAddr)GetProcAddress(wine,"vkGetInstanceProcAddr");
    realGdpa=(PFN_vkGetDeviceProcAddr)GetProcAddress(wine,"vkGetDeviceProcAddr");
    return realGipa && realGdpa;
}
bool resolve(){return InitOnceExecuteOnce(&once,Resolve,nullptr,nullptr);}
struct Device { VkInstance instance{}; VkPhysicalDevice physical{}; std::set<uint32_t> singleQueues; bool bda=false; };
struct Pool { VkDevice device{}; uint32_t family=~0u; };
struct Command { VkDevice device{}; VkCommandPool pool{}; uint32_t family=~0u; bool usable=false,used=false; };
std::map<VkPhysicalDevice,VkInstance> physicals;
std::map<VkDevice,Device> devices;
std::map<VkCommandPool,Pool> pools;
std::map<VkCommandBuffer,Command> commands;
NrColorBridge bridge;
NgxSnippet nr;
bool attempted=false,disabled=false;
std::set<std::string> skipped;
bool Skip(const char* why) {if(skipped.insert(why).second) Log("[nr-inline] bypass: %s",why);return false;}
unsigned accepted=0;
const void* srHandle=nullptr;
VkDevice nrDevice{};
uint32_t nrFamily=~0u;

bool Bind(VkInstance instance,VkDevice device) {
    g_gipa=realGipa;
#define LOAD(name) name=(PFN_##name)realGdpa(device,#name); if(!name) return false;
    LOAD(vkCreateImage) LOAD(vkDestroyImage) LOAD(vkCreateImageView) LOAD(vkDestroyImageView)
    LOAD(vkGetImageMemoryRequirements) LOAD(vkAllocateMemory) LOAD(vkFreeMemory) LOAD(vkBindImageMemory)
    LOAD(vkCreateShaderModule) LOAD(vkDestroyShaderModule) LOAD(vkCreatePipelineLayout) LOAD(vkDestroyPipelineLayout)
    LOAD(vkCreateComputePipelines) LOAD(vkDestroyPipeline) LOAD(vkCmdBindPipeline) LOAD(vkCmdDispatch)
    LOAD(vkCreateDescriptorSetLayout) LOAD(vkDestroyDescriptorSetLayout) LOAD(vkCreateDescriptorPool)
    LOAD(vkDestroyDescriptorPool) LOAD(vkAllocateDescriptorSets) LOAD(vkUpdateDescriptorSets) LOAD(vkCmdBindDescriptorSets)
    LOAD(vkCmdPipelineBarrier) LOAD(vkDeviceWaitIdle)
#undef LOAD
    return realGipa(instance,"vkGetPhysicalDeviceMemoryProperties")!=nullptr;
}
bool Resource(const void* params,const char* name,ngx_capture::Resource& out) {
    void* p=nullptr;
    if(ngx_capture::Get(params,name,&p,8)!=1 || !ngx_capture::Read(p,&out,sizeof(out))) return false;
    return out.type==0 && out.data.image.image && out.data.image.view && out.data.image.baseMip==0 &&
        out.data.image.baseLayer==0 && out.data.image.levels==1 && out.data.image.layers==1;
}
NVSDK_NGX_Resource_VK ToResource(const GpuImage& i,bool rw) {
    NVSDK_NGX_Resource_VK r{};
    r.Resource.ImageViewInfo={i.view,i.image,{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1},i.format,i.width,i.height};
    r.Type=NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGE_VIEW; r.ReadWrite=rw; return r;
}
bool Process(VkCommandBuffer cmd,const void* handle,const void* params,NVSDK_NGX_Resource_VK& output) {
    if(disabled) return false;
    Command command; Device device;
    {
        Lock lock(stateLock);
        auto cb=commands.find(cmd);
        if(cb==commands.end() || !cb->second.usable || cb->second.used) return Skip("untracked, simultaneous or already-used command recording");
        command=cb->second;
        auto dev=devices.find(command.device);
        if(dev==devices.end() || !dev->second.bda || !dev->second.singleQueues.count(command.family)) return Skip("device context, bufferDeviceAddress or single-queue family unavailable");
        device=dev->second;
        cb->second.used=true; // One SR replacement per recording; descriptor sets cannot be overwritten mid-recording.
    }
    ngx_capture::Resource color{},motion{},exposure{};
    int flags=0,reset=0;
    float pre=0,jx=0,jy=0,mx=0,my=0;
    unsigned w=0,h=0;
    if(!Resource(params,"Color",color) || !Resource(params,"MotionVectors",motion) || !Resource(params,"ExposureTexture",exposure) ||
       ngx_capture::Get(params,"DLSS.Feature.Create.Flags",&flags,11)!=1 || (flags&15)!=11 ||
       ngx_capture::Get(params,"Reset",&reset,11)!=1 ||
       ngx_capture::Get(params,"DLSS.Pre.Exposure",&pre,14)!=1 || !std::isfinite(pre) || pre<=0 ||
       ngx_capture::Get(params,"Jitter.Offset.X",&jx,14)!=1 || !std::isfinite(jx) ||
       ngx_capture::Get(params,"Jitter.Offset.Y",&jy,14)!=1 || !std::isfinite(jy) ||
       ngx_capture::Get(params,"MV.Scale.X",&mx,14)!=1 || mx!=1 ||
       ngx_capture::Get(params,"MV.Scale.Y",&my,14)!=1 || my!=1 ||
       ngx_capture::Get(params,"DLSS.Render.Subrect.Dimensions.Width",&w,12)!=1 ||
       ngx_capture::Get(params,"DLSS.Render.Subrect.Dimensions.Height",&h,12)!=1) return Skip("SR input contract differs from verified HDR/low-resolution-motion profile");
    for(const char* key:{"DLSS.Input.Color.Subrect.Base.X","DLSS.Input.Color.Subrect.Base.Y",
                        "DLSS.Input.MV.Subrect.Base.X","DLSS.Input.MV.Subrect.Base.Y"}) {
        unsigned value=~0u; if(ngx_capture::Get(params,key,&value,12)!=1 || value) return Skip("nonzero or unavailable input subrect origin");
    }
    const auto& ci=color.data.image; const auto& mi=motion.data.image; const auto& ei=exposure.data.image;
    if(w<64 || h<64 || w>4096 || h>2160 || ci.format!=97 || ci.width!=w || ci.height!=h || ci.aspect!=1 ||
       mi.format!=83 || mi.width!=w || mi.height!=h || mi.aspect!=1 || ei.format!=100 || ei.width!=1 || ei.height!=1 || ei.aspect!=1) return Skip("unsupported resource formats or extents");
    if(attempted && (srHandle!=handle || nrDevice!=command.device || nrFamily!=command.family ||
                     bridge.encoded.width!=w || bridge.encoded.height!=h)) {
        disabled=true; Log("[nr-inline] disabled: SR feature/device/extent changed; original SR preserved until restart"); return false;
    }
    if(!attempted) {
        attempted=true; srHandle=handle; nrDevice=command.device; nrFamily=command.family;
        if(!Bind(device.instance,command.device)) { disabled=true; return false; }
        VkCtx context{}; context.instance=device.instance; context.physical=device.physical;
        context.device=command.device; context.queueFamily=command.family;
        InstallGuard(); g_layerModule=selfModule;
        if(!bridge.initialize(context,w,h) || !NgxLoadAndInit(nr,context.instance,context.physical,context.device,w,h,cmd,{})) {
            disabled=true; Log("[nr-inline] initialization failed; original SR retained"); return false;
        }
        Log("[nr-inline] ready %ux%u: HDR proxy -> NR -> HDR residual -> SR; native motion, no synthetic depth",w,h);
    }
    if(!bridge.prepare(cmd,(VkImageView)ci.view,(VkImageView)ei.view)) {disabled=true;return false;}
    bridge.dispatch(cmd,0,pre);
    TransitionImage(bridge.c,cmd,bridge.model,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,
        VK_ACCESS_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    NVSDK_NGX_Resource_VK mv{}; static_assert(sizeof(mv)==sizeof(motion)); memcpy(&mv,&motion,sizeof(mv));
    NgxSetResources(nr,ToResource(bridge.encoded,false),ToResource(bridge.model,true),mv,{},w,h);
    NgxSetReset(nr,reset!=0 || accepted==0);
    // This NR adapter uses its own verified 16-slot parameter implementation.
    nr.params->Set("DLSSNR.Jitter.Offset.X",jx); nr.params->Set("DLSSNR.Jitter.Offset.Y",jy);
    nr.params->Set("JitterOffsetX",jx); nr.params->Set("JitterOffsetY",jy);
    if(!NgxEvaluatePass(nr,0,cmd)) {disabled=true;return false;}
    TransitionImage(bridge.c,cmd,bridge.model,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    bridge.dispatch(cmd,1,pre);
    output=ToResource(bridge.restored,false); ++accepted;
    if(accepted==1 || accepted==120) Log("[nr-inline] recorded NR-before-SR calls=%u",accepted);
    return true;
}
}

extern "C" __declspec(dllexport) uint32_t DlssNrEvaluate(void* cmd,const void* handle,const void* params,void* callback,Evaluate real) {
    if(!enabled || !TryAcquireSRWLockExclusive(&renderLock)) return real(cmd,handle,params,callback);
    NVSDK_NGX_Resource_VK replacement{};
    const DWORD last=GetLastError(); bool processed=false;
    try { processed=Process((VkCommandBuffer)cmd,handle,params,replacement); }
    catch(...) { disabled=true; Log("[nr-inline] disabled after internal exception"); }
    ngx_capture::ColorOverlay overlay(params,&replacement);
    SetLastError(last);
    // Do not catch the game's call. Original args/callback/result are forwarded,
    // with a Color-only parameter overlay after a successful NR recording.
    auto result=real(cmd,handle,processed?&overlay:params,callback);
    const DWORD after=GetLastError(); ReleaseSRWLockExclusive(&renderLock); SetLastError(after); return result;
}

extern "C" VkResult VKAPI_CALL NrEnumerate(VkInstance i,uint32_t* n,VkPhysicalDevice* p) {
    resolve(); auto fn=(PFN_vkEnumeratePhysicalDevices)GetProcAddress(GetModuleHandleW(L"winevulkan.dll"),"vkEnumeratePhysicalDevices");
    auto r=fn(i,n,p);
    if(enabled && p && (r==VK_SUCCESS || r==VK_INCOMPLETE)) {Lock lock(stateLock); for(unsigned j=0;j<*n;++j) physicals[p[j]]=i;}
    return r;
}
extern "C" VkResult VKAPI_CALL NrCreateDevice(VkPhysicalDevice p,const VkDeviceCreateInfo* info,const VkAllocationCallbacks* alloc,VkDevice* out) {
    VkInstance instance{}; {Lock lock(stateLock); auto it=physicals.find(p); if(it!=physicals.end()) instance=it->second;}
    resolve(); auto fn=(PFN_vkCreateDevice)GetProcAddress(GetModuleHandleW(L"winevulkan.dll"),"vkCreateDevice");
    // NR needs buffer device addresses. Add the supported KHR feature only when
    // there is no existing BDA/Vulkan12 declaration to conflict with. Never edit
    // the game's pNext objects or silently override an explicit false field.
    VkDeviceCreateInfo selected=*info;
    VkPhysicalDeviceBufferDeviceAddressFeatures bda{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
    bool declared=false,extBda=false,hasKhr=false;
    for(auto* n=(const VkBaseInStructure*)info->pNext;n;n=n->pNext)
        declared|=n->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES || n->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    std::vector<const char*> extensions;
    for(unsigned i=0;i<info->enabledExtensionCount;++i) {
        auto name=info->ppEnabledExtensionNames[i];extensions.push_back(name);
        extBda|=!strcmp(name,"VK_EXT_buffer_device_address");hasKhr|=!strcmp(name,"VK_KHR_buffer_device_address");
    }
    if(enabled && instance && !declared && !extBda) {
        auto features=(PFN_vkGetPhysicalDeviceFeatures2)realGipa(instance,"vkGetPhysicalDeviceFeatures2");
        auto enumerate=(PFN_vkEnumerateDeviceExtensionProperties)realGipa(instance,"vkEnumerateDeviceExtensionProperties");
        VkPhysicalDeviceFeatures2 queried{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};queried.pNext=&bda;
        uint32_t count=0;bool supported=false;
        if(features && enumerate && enumerate(p,nullptr,&count,nullptr)==VK_SUCCESS) {
            std::vector<VkExtensionProperties> available(count);
            if(enumerate(p,nullptr,&count,available.data())==VK_SUCCESS)
                for(auto& e:available)supported|=!strcmp(e.extensionName,"VK_KHR_buffer_device_address");
            features(p,&queried);
        }
        if(supported && bda.bufferDeviceAddress) {
            bda.bufferDeviceAddressCaptureReplay=VK_FALSE;bda.bufferDeviceAddressMultiDevice=VK_FALSE;
            bda.pNext=const_cast<void*>(info->pNext);selected.pNext=&bda;
            if(!hasKhr)extensions.push_back("VK_KHR_buffer_device_address");
            selected.enabledExtensionCount=unsigned(extensions.size());selected.ppEnabledExtensionNames=extensions.data();
            Log("[nr-inline] enabling supported bufferDeviceAddress for NR");
        }
    }
    auto result=fn(p,&selected,alloc,out);
    if(result!=VK_SUCCESS && selected.pNext!=info->pNext) {
        Log("[nr-inline] BDA-enabled creation failed (%d); retrying unchanged game configuration",result);
        selected=*info;result=fn(p,info,alloc,out);
    }
    if(enabled && result==VK_SUCCESS && instance) {
        Device d; d.instance=instance; d.physical=p;
        for(unsigned i=0;i<info->queueCreateInfoCount;++i) if(info->pQueueCreateInfos[i].queueCount==1)
            d.singleQueues.insert(info->pQueueCreateInfos[i].queueFamilyIndex);
        for(auto* n=(const VkBaseInStructure*)selected.pNext;n;n=n->pNext) {
            if(n->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES) d.bda=((const VkPhysicalDeviceBufferDeviceAddressFeatures*)n)->bufferDeviceAddress;
            if(n->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) d.bda=((const VkPhysicalDeviceVulkan12Features*)n)->bufferDeviceAddress;
        }
        {Lock lock(stateLock); devices[*out]=d;}
        Log("[nr-inline] Vulkan device observed; bufferDeviceAddress=%u",unsigned(d.bda));
    }
    return result;
}
extern "C" VkResult VKAPI_CALL NrCreatePool(VkDevice d,const VkCommandPoolCreateInfo* info,const VkAllocationCallbacks* a,VkCommandPool* out) {
    resolve();
    auto result=((PFN_vkCreateCommandPool)realGdpa(d,"vkCreateCommandPool"))(d,info,a,out);
    if(enabled && result==VK_SUCCESS) {Lock lock(stateLock); pools[*out]={d,info->queueFamilyIndex};}
    return result;
}
extern "C" VkResult VKAPI_CALL NrAllocate(VkDevice d,const VkCommandBufferAllocateInfo* info,VkCommandBuffer* out) {
    resolve();
    auto result=((PFN_vkAllocateCommandBuffers)realGdpa(d,"vkAllocateCommandBuffers"))(d,info,out);
    if(enabled && result==VK_SUCCESS) {Lock lock(stateLock); auto pool=pools.find(info->commandPool);
        if(pool!=pools.end()) for(unsigned i=0;i<info->commandBufferCount;++i)
            commands[out[i]]={d,info->commandPool,pool->second.family,false,false};}
    return result;
}
extern "C" VkResult VKAPI_CALL NrBegin(VkCommandBuffer cmd,const VkCommandBufferBeginInfo* info) {
    resolve(); auto wine=GetModuleHandleW(L"winevulkan.dll");
    auto result=((PFN_vkBeginCommandBuffer)GetProcAddress(wine,"vkBeginCommandBuffer"))(cmd,info);
    if(enabled && result==VK_SUCCESS) {Lock lock(stateLock); auto it=commands.find(cmd);
        if(it!=commands.end()) {it->second.usable=!(info->flags&VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT) && !info->pInheritanceInfo;it->second.used=false;}}
    return result;
}
extern "C" void VKAPI_CALL NrFree(VkDevice d,VkCommandPool pool,uint32_t n,const VkCommandBuffer* cmds) {
    resolve();
    if(enabled) {Lock lock(stateLock);for(unsigned i=0;i<n;++i) commands.erase(cmds[i]);}
    ((PFN_vkFreeCommandBuffers)realGdpa(d,"vkFreeCommandBuffers"))(d,pool,n,cmds);
}
extern "C" void VKAPI_CALL NrDestroyPool(VkDevice d,VkCommandPool p,const VkAllocationCallbacks* a) {
    resolve();
    if(enabled) {Lock lock(stateLock);pools.erase(p);for(auto it=commands.begin();it!=commands.end();)
        if(it->second.pool==p) it=commands.erase(it);else ++it;}
    ((PFN_vkDestroyCommandPool)realGdpa(d,"vkDestroyCommandPool"))(d,p,a);
}
extern "C" void VKAPI_CALL NrDestroyDevice(VkDevice d,const VkAllocationCallbacks* a) {
    resolve();
    if(enabled) {
        Lock lock(renderLock);
        if(d==nrDevice) {
            if(vkDeviceWaitIdle) vkDeviceWaitIdle(d);
            NgxTeardown(nr,d); bridge.shutdown(); disabled=true;
        }
        Lock state(stateLock); devices.erase(d);
        for(auto it=commands.begin();it!=commands.end();) if(it->second.device==d) it=commands.erase(it);else ++it;
        for(auto it=pools.begin();it!=pools.end();) if(it->second.device==d) it=pools.erase(it);else ++it;
    }
    ((PFN_vkDestroyDevice)realGdpa(d,"vkDestroyDevice"))(d,a);
}
extern "C" PFN_vkVoidFunction VKAPI_CALL NrGipa(VkInstance,const char*);
extern "C" PFN_vkVoidFunction VKAPI_CALL NrGdpa(VkDevice,const char*);
static PFN_vkVoidFunction Wrap(const char* n,PFN_vkVoidFunction original) {
    if(!enabled || !original || !n) return original;
#define HOOK(name,fn) if(!strcmp(n,name)) return (PFN_vkVoidFunction)&fn;
    HOOK("vkGetInstanceProcAddr",NrGipa) HOOK("vkGetDeviceProcAddr",NrGdpa)
    HOOK("vkEnumeratePhysicalDevices",NrEnumerate) HOOK("vkCreateDevice",NrCreateDevice)
    HOOK("vkCreateCommandPool",NrCreatePool) HOOK("vkDestroyCommandPool",NrDestroyPool)
    HOOK("vkAllocateCommandBuffers",NrAllocate) HOOK("vkFreeCommandBuffers",NrFree)
    HOOK("vkBeginCommandBuffer",NrBegin) HOOK("vkDestroyDevice",NrDestroyDevice)
#undef HOOK
    return original;
}
extern "C" PFN_vkVoidFunction VKAPI_CALL NrGipa(VkInstance i,const char* n) {return resolve()?Wrap(n,realGipa(i,n)):nullptr;}
extern "C" PFN_vkVoidFunction VKAPI_CALL NrGdpa(VkDevice d,const char* n) {return resolve()?Wrap(n,realGdpa(d,n)):nullptr;}
BOOL WINAPI DllMain(HINSTANCE self,DWORD why,void*) {
    if(why==DLL_PROCESS_ATTACH) {
        selfModule=self; DisableThreadLibraryCalls(self);
        wchar_t path[32768]{},flag[8]{}; auto count=GetModuleFileNameW(nullptr,path,32768);
        auto base=wcsrchr(path,L'\\'); base=base?base+1:path;
        enabled=count && count<32768 && !_wcsicmp(base,L"RDR2.exe") &&
            GetEnvironmentVariableW(L"DLSSNR_INLINE",flag,8)==1 && flag[0]=='1';
    }
    return TRUE;
}
