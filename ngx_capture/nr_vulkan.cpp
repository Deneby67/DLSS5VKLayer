// Application-local Vulkan forwarding shim. No per-draw hooks, submission or waits
// during steady-state rendering. The NGX observer calls DlssNrEvaluate explicitly.
#include "nr_color.h"
#include "params.h"
#include "color_overlay.h"
#include "../core/ngx_snippet.h"
#include "../core/guard.h"
#include <mutex>
#include <set>
#include "bootstrap_trace.h"
#include "nr_arm.h"
#include "nr_settings.h"
#include <vulkan/vulkan_win32.h>
namespace {
using namespace dlssnr;
using Evaluate=uint32_t(__cdecl*)(void*,const void*,const void*,void*);
HMODULE selfModule{},nativeLoader{};
bool enabled=false;
bool bootstrap=false,augmentBda=true;
SRWLOCK stateLock=SRWLOCK_INIT,renderLock=SRWLOCK_INIT;
struct Lock { SRWLOCK* p; Lock(SRWLOCK& s):p(&s){AcquireSRWLockExclusive(p);} ~Lock(){ReleaseSRWLockExclusive(p);} };
PFN_vkGetInstanceProcAddr realGipa{};
PFN_vkGetDeviceProcAddr realGdpa{};
INIT_ONCE once=INIT_ONCE_STATIC_INIT;
InlineArm arm;
InlineSettings settings;
InlineKeyEdge f2;
std::vector<NgxTuning> tuningCache;
NgxTuning pendingTuning;
ULONGLONG tuningChanged=0;
unsigned activeTuning=0;
bool resetHistory=true;
int renderingState=-1;
thread_local bool inNrAllocationScope=false;
struct NrAllocationScope {
    bool previous=inNrAllocationScope;
    NrAllocationScope(){inNrAllocationScope=true;}
    ~NrAllocationScope(){inNrAllocationScope=previous;}
};
BOOL CALLBACK Resolve(PINIT_ONCE,void*,void**) {
    // Preserve the native loader/ICD chain used by RDR2. The renamed loader
    // is a hash-pinned private copy of the game's prefix DLL, beside this shim.
    BootstrapTrace trace("resolve",bootstrap);
    wchar_t path[32768]{};
    auto count=GetModuleFileNameW(selfModule,path,32768);
    if(!count || count>=32768) return FALSE;
    auto slash=wcsrchr(path,L'\\');
    if(!slash || size_t(slash-path)+32>=32768) return FALSE;
    wcscpy(slash+1,L"dlssnr_system_vulkan.dll");
    nativeLoader=LoadLibraryExW(path,nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if(!nativeLoader || nativeLoader==selfModule) return FALSE;
    realGipa=(PFN_vkGetInstanceProcAddr)GetProcAddress(nativeLoader,"vkGetInstanceProcAddr");
    realGdpa=(PFN_vkGetDeviceProcAddr)GetProcAddress(nativeLoader,"vkGetDeviceProcAddr");
    return realGipa && realGdpa;
}
bool resolve(){return InitOnceExecuteOnce(&once,Resolve,nullptr,nullptr);}
struct Device { VkInstance instance{}; VkPhysicalDevice physical{}; std::set<uint32_t> singleQueues; bool bda=false,extAddress=false; };
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
        if(dev==devices.end()) return Skip("command buffer belongs to an unobserved device");
        if(!dev->second.bda) return Skip("command device has no enabled bufferDeviceAddress");
        if(!dev->second.singleQueues.count(command.family)) return Skip("command family does not have exactly one enabled queue");
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
        if(!Bind(device.instance,command.device)) { disabled=true; Log("[nr-inline] disabled: Vulkan dispatch unavailable"); return false; }
        VkCtx context{}; context.instance=device.instance; context.physical=device.physical;
        context.device=command.device; context.queueFamily=command.family;
        InstallGuard(); g_layerModule=selfModule;
        if(!bridge.initialize(context,w,h) || !NgxLoadAndInit(nr,context.instance,context.physical,context.device,w,h,cmd,settings.tuning)) {
            disabled=true; Log("[nr-inline] initialization failed; original SR retained"); return false;
        }
        tuningCache.push_back(settings.tuning);pendingTuning=settings.tuning;
        Log("[nr-inline] ready %ux%u: HDR proxy -> NR -> HDR residual -> SR; native motion, no synthetic depth",w,h);
    }
    if(settings.tuning!=pendingTuning) {pendingTuning=settings.tuning;tuningChanged=GetTickCount64();}
    if(settings.tuning!=tuningCache[activeTuning] && GetTickCount64()-tuningChanged>=750) {
        unsigned selected=0;
        while(selected<tuningCache.size() && tuningCache[selected]!=settings.tuning)++selected;
        if(selected==tuningCache.size()) {
            // Retain old features until device idle/destruction: already recorded game
            // command buffers can still reference their histories. Bound VRAM growth.
            if(selected>=8) {disabled=true;Log("[nr-inline] disabled: eight Rendering configurations cached; restart game to release histories");return false;}
            NgxSetCreateTuning(nr,settings.tuning);
            if(!NgxCreatePass(nr,selected,w,h,cmd)) {disabled=true;Log("[nr-inline] disabled: Rendering configuration creation failed");return false;}
            tuningCache.push_back(settings.tuning);
        }
        NgxSetCreateTuning(nr,settings.tuning);
        activeTuning=selected;resetHistory=true;
        Log("[nr-inline] Rendering configuration applied slot=%u intensity=%.2f tone=%.2f structure=%.2f skin=%.2f style=%u preset=%u automask=%u",
            selected,settings.tuning.intensity,settings.tuning.localTone,settings.tuning.localStructure,
            settings.tuning.skinStructure,settings.tuning.style,settings.tuning.preset,settings.tuning.autoMask);
    }
    if(!bridge.prepare(cmd,(VkImageView)ci.view,(VkImageView)ei.view)) {
        disabled=true;Log("[nr-inline] disabled: color descriptor allocation failed (%zu command buffers)",bridge.sets.size());return false;
    }
    NgxSetSharpness(nr,settings.sharpness);
    bridge.dispatch(cmd,0,pre);
    TransitionImage(bridge.c,cmd,bridge.model,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,
        VK_ACCESS_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    NVSDK_NGX_Resource_VK mv{}; static_assert(sizeof(mv)==sizeof(motion)); memcpy(&mv,&motion,sizeof(mv));
    NgxSetResources(nr,ToResource(bridge.encoded,false),ToResource(bridge.model,true),mv,{},w,h);
    NgxSetReset(nr,reset!=0 || resetHistory || accepted==0);
    // This NR adapter uses its own verified 16-slot parameter implementation.
    nr.params->Set("DLSSNR.Jitter.Offset.X",jx); nr.params->Set("DLSSNR.Jitter.Offset.Y",jy);
    nr.params->Set("JitterOffsetX",jx); nr.params->Set("JitterOffsetY",jy);
    if(!NgxEvaluatePass(nr,activeTuning,cmd)) {disabled=true;Log("[nr-inline] disabled: NGX evaluation failed");return false;}
    resetHistory=false;
    TransitionImage(bridge.c,cmd,bridge.model,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    bridge.dispatch(cmd,1,pre);
    output=ToResource(bridge.restored,false); ++accepted;
    if(accepted==1 || accepted%600==0) Log("[nr-inline] recorded NR-before-SR calls=%u",accepted);
    return true;
}
}

extern "C" __declspec(dllexport) ULONGLONG DlssNrArmToken() {return arm.token;}

extern "C" __declspec(dllexport) uint32_t DlssNrEvaluate(void* cmd,const void* handle,const void* params,void* callback,Evaluate real) {
    if(bootstrap || !enabled || !TryAcquireSRWLockExclusive(&renderLock)) return real(cmd,handle,params,callback);
    NVSDK_NGX_Resource_VK replacement{};
    const DWORD last=GetLastError(); bool processed=false;
    try {
        const bool validSettings=settings.read();
        bool requested=arm.allow();
        if(InlineF2(f2) && arm.configured) {
            const bool on=!(requested && settings.enabled);
            if(arm.set(on)) {
                settings.setEnabled(on);requested=arm.allow();
                Log("[nr-inline] F2 %s",on?"enabled":"disabled");
            } else Log("[nr-inline] F2 control write failed error=%lu",GetLastError());
        }
        const int gate=validSettings?(settings.enabled?1:0):2;
        if(gate!=renderingState) {
            renderingState=gate;
            Log("[nr-inline] Rendering state %s",gate==1?"enabled":gate==0?"disabled":"unavailable");
        }
        if(requested && validSettings && settings.enabled) {
            NrAllocationScope scope;processed=Process((VkCommandBuffer)cmd,handle,params,replacement);
        } else {
            resetHistory=true;
        }
    }
    catch(...) { disabled=true; Log("[nr-inline] disabled after internal exception"); }
    ngx_capture::ColorOverlay overlay(params,&replacement);
    SetLastError(last);
    // Do not catch the game's call. Original args/callback/result are forwarded,
    // with a Color-only parameter overlay after a successful NR recording.
    auto result=real(cmd,handle,processed?&overlay:params,callback);
    const DWORD after=GetLastError(); ReleaseSRWLockExclusive(&renderLock); SetLastError(after); return result;
}

extern "C" VkResult VKAPI_CALL NrEnumerate(VkInstance i,uint32_t* n,VkPhysicalDevice* p) {
    BootstrapTrace trace("vkEnumeratePhysicalDevices",bootstrap);
    resolve(); auto fn=(PFN_vkEnumeratePhysicalDevices)GetProcAddress(nativeLoader,"vkEnumeratePhysicalDevices");
    auto r=fn(i,n,p);
    if(enabled && p && (r==VK_SUCCESS || r==VK_INCOMPLETE)) {Lock lock(stateLock); for(unsigned j=0;j<*n;++j) physicals[p[j]]=i;}
    trace.done(r);return r;
}
static VkResult EnumerateGroups(VkInstance i,uint32_t* count,VkPhysicalDeviceGroupProperties* groups,const char* name) {
    if(!resolve()) return VK_ERROR_INITIALIZATION_FAILED;
    auto fn=(PFN_vkEnumeratePhysicalDeviceGroups)realGipa(i,name);
    if(!fn) return VK_ERROR_EXTENSION_NOT_PRESENT;
    auto result=fn(i,count,groups);
    if(enabled && groups && (result==VK_SUCCESS || result==VK_INCOMPLETE)) {
        Lock lock(stateLock);
        for(uint32_t j=0;j<*count;++j)
            for(uint32_t k=0;k<groups[j].physicalDeviceCount && k<VK_MAX_DEVICE_GROUP_SIZE;++k)
                physicals[groups[j].physicalDevices[k]]=i;
        Log("[nr-inline] %s observed %u physical-device groups",name,*count);
    }
    return result;
}
extern "C" VkResult VKAPI_CALL NrGroups(VkInstance i,uint32_t* n,VkPhysicalDeviceGroupProperties* groups) {
    return EnumerateGroups(i,n,groups,"vkEnumeratePhysicalDeviceGroups");
}
extern "C" VkResult VKAPI_CALL NrGroupsKHR(VkInstance i,uint32_t* n,VkPhysicalDeviceGroupProperties* groups) {
    return EnumerateGroups(i,n,groups,"vkEnumeratePhysicalDeviceGroupsKHR");
}
extern "C" void VKAPI_CALL NrDestroyInstance(VkInstance i,const VkAllocationCallbacks* alloc) {
    if(!resolve())return;
    auto fn=(PFN_vkDestroyInstance)realGipa(i,"vkDestroyInstance");
    if(enabled) {Lock lock(stateLock);for(auto it=physicals.begin();it!=physicals.end();)
        if(it->second==i)it=physicals.erase(it);else ++it;}
    fn(i,alloc);
}
extern "C" VkResult VKAPI_CALL NrCreateDevice(VkPhysicalDevice p,const VkDeviceCreateInfo* info,const VkAllocationCallbacks* alloc,VkDevice* out) {
    BootstrapTrace trace("vkCreateDevice",bootstrap);
    resolve(); auto fn=(PFN_vkCreateDevice)GetProcAddress(nativeLoader,"vkCreateDevice");
    if(!enabled) {auto result=fn(p,info,alloc,out);trace.done(result);return result;}
    VkInstance instance{}; {Lock lock(stateLock); auto it=physicals.find(p); if(it!=physicals.end()) instance=it->second;}
    Log("[nr-inline] create-device physical=%p tracked-instance=%p",p,instance);
    for(unsigned j=0;j<info->queueCreateInfoCount;++j)
        Log("[nr-inline] requested queue family=%u count=%u flags=%#x",info->pQueueCreateInfos[j].queueFamilyIndex,info->pQueueCreateInfos[j].queueCount,info->pQueueCreateInfos[j].flags);
    // NR needs buffer device addresses. Add the supported KHR feature only when
    // there is no existing BDA/Vulkan12 declaration to conflict with. Never edit
    // the game's pNext objects or silently override an explicit false field.
    VkDeviceCreateInfo selected=*info;
    VkPhysicalDeviceBufferDeviceAddressFeatures bda{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
    VkPhysicalDeviceBufferDeviceAddressFeaturesEXT extFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES_EXT};
    bool declared=false,extBda=false,hasKhr=false;
    for(auto* n=(const VkBaseInStructure*)info->pNext;n;n=n->pNext)
        declared|=n->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES_EXT || n->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES || n->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    std::vector<const char*> extensions;
    for(unsigned i=0;i<info->enabledExtensionCount;++i) {
        auto name=info->ppEnabledExtensionNames[i];extensions.push_back(name);
        extBda|=!strcmp(name,"VK_EXT_buffer_device_address");hasKhr|=!strcmp(name,"VK_KHR_buffer_device_address");
    }
    Log("[nr-inline] BDA contract: declared=%u EXT=%u KHR=%u",unsigned(declared),unsigned(extBda),unsigned(hasKhr));
    if(enabled && augmentBda && instance && !declared) {
        auto features=(PFN_vkGetPhysicalDeviceFeatures2)realGipa(instance,"vkGetPhysicalDeviceFeatures2");
        if(!features)features=(PFN_vkGetPhysicalDeviceFeatures2)realGipa(instance,"vkGetPhysicalDeviceFeatures2KHR");
        auto enumerate=(PFN_vkEnumerateDeviceExtensionProperties)realGipa(instance,"vkEnumerateDeviceExtensionProperties");
        VkPhysicalDeviceFeatures2 queried{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};queried.pNext=extBda?static_cast<void*>(&extFeatures):static_cast<void*>(&bda);
        uint32_t count=0;bool supported=false;
        if(features && enumerate && enumerate(p,nullptr,&count,nullptr)==VK_SUCCESS) {
            std::vector<VkExtensionProperties> available(count);
            if(enumerate(p,nullptr,&count,available.data())==VK_SUCCESS)
                for(auto& e:available)supported|=!strcmp(e.extensionName,extBda?"VK_EXT_buffer_device_address":"VK_KHR_buffer_device_address");
            features(p,&queried);
        }
        if(supported && extBda && extFeatures.bufferDeviceAddress) {
            extFeatures.bufferDeviceAddressCaptureReplay=VK_FALSE;extFeatures.bufferDeviceAddressMultiDevice=VK_FALSE;
            extFeatures.pNext=const_cast<void*>(info->pNext);selected.pNext=&extFeatures;
            Log("[nr-inline] enabling supported EXT bufferDeviceAddress for NR (no KHR extension added)");
        } else if(supported && !extBda && bda.bufferDeviceAddress) {
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
    Log("[nr-inline] create-device result=%d device=%p",result,result==VK_SUCCESS?*out:nullptr);
    if(enabled && result==VK_SUCCESS && !instance)Log("[nr-inline] device left untracked: physical device was not observed in enumeration");
    if(enabled && result==VK_SUCCESS && instance) {
        Device d; d.instance=instance; d.physical=p; d.extAddress=extBda;
        for(unsigned i=0;i<info->queueCreateInfoCount;++i) if(info->pQueueCreateInfos[i].queueCount==1)
            d.singleQueues.insert(info->pQueueCreateInfos[i].queueFamilyIndex);
        for(auto* n=(const VkBaseInStructure*)selected.pNext;n;n=n->pNext) {
            if(n->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES_EXT) d.bda=((const VkPhysicalDeviceBufferDeviceAddressFeaturesEXT*)n)->bufferDeviceAddress;
            if(n->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES) d.bda=((const VkPhysicalDeviceBufferDeviceAddressFeatures*)n)->bufferDeviceAddress;
            if(n->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) d.bda=((const VkPhysicalDeviceVulkan12Features*)n)->bufferDeviceAddress;
        }
        {Lock lock(stateLock); devices[*out]=d;}
        Log("[nr-inline] Vulkan device observed; bufferDeviceAddress=%u",unsigned(d.bda));
    }
    trace.done(result);return result;
}
extern "C" VkResult VKAPI_CALL NrAllocateMemory(VkDevice d,const VkMemoryAllocateInfo* info,const VkAllocationCallbacks* alloc,VkDeviceMemory* out) {
    if(!resolve())return VK_ERROR_INITIALIZATION_FAILED;
    auto fn=(PFN_vkAllocateMemory)realGdpa(d,"vkAllocateMemory");
    bool ext=false;
    if(enabled) {Lock lock(stateLock);auto it=devices.find(d);
        ext=it!=devices.end() && it->second.bda && it->second.extAddress;}
    if(ext) for(auto* node=(const VkBaseInStructure*)info->pNext;node;node=node->pNext) {
        if(node->sType!=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO)continue;
        auto flags=*reinterpret_cast<const VkMemoryAllocateFlagsInfo*>(node);
        if(!(flags.flags&(VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT|VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT)))break;
        if(!inNrAllocationScope) {
            // The game's SR DLL has the same invalid KHR flag on its EXT path.
            // Limit that correction to calls from the known NGX modules.
            HMODULE caller{};wchar_t path[32768]{};
            if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  (LPCWSTR)__builtin_return_address(0),&caller))break;
            auto length=GetModuleFileNameW(caller,path,32768);
            auto basename=wcsrchr(path,L'\\');basename=basename?basename+1:path;
            if(!length || length>=32768 || (_wcsicmp(basename,L"nvngx_dlss.dll") && _wcsicmp(basename,L"nvngx.dll")))break;
        }
        // EXT addresses do not require the KHR allocation flag. Some NGX builds
        // choose the EXT address command but still supply that KHR-only flag.
        // Adapt only NGX allocations, without mutating caller-owned chains.
        if(info->pNext!=node || flags.flags&VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT) {
            if(!inNrAllocationScope)break; // Preserve unknown SR allocation contracts.
            Log("[nr-inline] unsupported NR EXT address allocation chain; allocation refused");
            *out=VK_NULL_HANDLE;return VK_ERROR_FEATURE_NOT_PRESENT;
        }
        flags.flags&=~VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        VkMemoryAllocateInfo selected=*info;selected.pNext=&flags;
        Log("[nr-inline] adapting NGX allocation to EXT buffer address semantics");
        return fn(d,&selected,alloc,out);
    }
    return fn(d,info,alloc,out);
}
extern "C" VkResult VKAPI_CALL NrCreatePool(VkDevice d,const VkCommandPoolCreateInfo* info,const VkAllocationCallbacks* a,VkCommandPool* out) {
    BootstrapTrace trace("vkCreateCommandPool",bootstrap);
    resolve();
    auto result=((PFN_vkCreateCommandPool)realGdpa(d,"vkCreateCommandPool"))(d,info,a,out);
    if(enabled && result==VK_SUCCESS) {Lock lock(stateLock); pools[*out]={d,info->queueFamilyIndex};}
    trace.done(result);return result;
}
extern "C" VkResult VKAPI_CALL NrAllocate(VkDevice d,const VkCommandBufferAllocateInfo* info,VkCommandBuffer* out) {
    BootstrapTrace trace("vkAllocateCommandBuffers",bootstrap);
    resolve();
    auto result=((PFN_vkAllocateCommandBuffers)realGdpa(d,"vkAllocateCommandBuffers"))(d,info,out);
    if(enabled && result==VK_SUCCESS) {Lock lock(stateLock); auto pool=pools.find(info->commandPool);
        if(pool!=pools.end()) for(unsigned i=0;i<info->commandBufferCount;++i)
            commands[out[i]]={d,info->commandPool,pool->second.family,false,false};}
    trace.done(result);return result;
}
extern "C" VkResult VKAPI_CALL NrBegin(VkCommandBuffer cmd,const VkCommandBufferBeginInfo* info) {
    BootstrapTrace trace("vkBeginCommandBuffer",bootstrap);
    resolve();
    auto result=((PFN_vkBeginCommandBuffer)GetProcAddress(nativeLoader,"vkBeginCommandBuffer"))(cmd,info);
    if(enabled && result==VK_SUCCESS) {Lock lock(stateLock); auto it=commands.find(cmd);
        if(it!=commands.end()) {it->second.usable=!(info->flags&VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT) && !info->pInheritanceInfo;it->second.used=false;}}
    trace.done(result);return result;
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
// Bootstrap-only breadcrumbs; all application arguments are forwarded unchanged.
extern "C" VkResult VKAPI_CALL NrCreateInstance(const VkInstanceCreateInfo* i,const VkAllocationCallbacks* a,VkInstance* o) {
    BootstrapTrace trace("vkCreateInstance",bootstrap);
    if(!resolve()) return VK_ERROR_INITIALIZATION_FAILED;
    auto fn=(PFN_vkCreateInstance)GetProcAddress(nativeLoader,"vkCreateInstance");
    auto result=fn(i,a,o);trace.done(result);return result;
}
extern "C" VkResult VKAPI_CALL NrCreateSurface(VkInstance i,const VkWin32SurfaceCreateInfoKHR* c,const VkAllocationCallbacks* a,VkSurfaceKHR* o) {
    BootstrapTrace trace("vkCreateWin32SurfaceKHR",bootstrap);
    if(!resolve()) return VK_ERROR_INITIALIZATION_FAILED;
    auto fn=(PFN_vkCreateWin32SurfaceKHR)GetProcAddress(nativeLoader,"vkCreateWin32SurfaceKHR");
    auto result=fn(i,c,a,o);trace.done(result);return result;
}
extern "C" VkResult VKAPI_CALL NrCreateSwapchain(VkDevice d,const VkSwapchainCreateInfoKHR* c,const VkAllocationCallbacks* a,VkSwapchainKHR* o) {
    BootstrapTrace trace("vkCreateSwapchainKHR",bootstrap);
    if(!resolve()) return VK_ERROR_INITIALIZATION_FAILED;
    auto fn=(PFN_vkCreateSwapchainKHR)GetProcAddress(nativeLoader,"vkCreateSwapchainKHR");
    auto result=fn(d,c,a,o);trace.done(result);return result;
}
extern "C" VkResult VKAPI_CALL NrSubmit(VkQueue q,uint32_t n,const VkSubmitInfo* s,VkFence f) {
    BootstrapTrace trace("vkQueueSubmit",bootstrap);
    if(!resolve()) return VK_ERROR_INITIALIZATION_FAILED;
    auto fn=(PFN_vkQueueSubmit)GetProcAddress(nativeLoader,"vkQueueSubmit");
    auto result=fn(q,n,s,f);trace.done(result);return result;
}
extern "C" VkResult VKAPI_CALL NrWait(VkDevice d,uint32_t n,const VkFence* f,VkBool32 all,uint64_t timeout) {
    BootstrapTrace trace("vkWaitForFences",bootstrap);
    if(!resolve()) return VK_ERROR_INITIALIZATION_FAILED;
    auto fn=(PFN_vkWaitForFences)GetProcAddress(nativeLoader,"vkWaitForFences");
    auto result=fn(d,n,f,all,timeout);trace.done(result);return result;
}
extern "C" VkResult VKAPI_CALL NrAcquire(VkDevice d,VkSwapchainKHR s,uint64_t t,VkSemaphore sem,VkFence f,uint32_t* i) {
    BootstrapTrace trace("vkAcquireNextImageKHR",bootstrap);
    if(!resolve()) return VK_ERROR_INITIALIZATION_FAILED;
    auto fn=(PFN_vkAcquireNextImageKHR)GetProcAddress(nativeLoader,"vkAcquireNextImageKHR");
    auto result=fn(d,s,t,sem,f,i);trace.done(result);return result;
}
extern "C" VkResult VKAPI_CALL NrPresent(VkQueue q,const VkPresentInfoKHR* p) {
    BootstrapTrace trace("vkQueuePresentKHR",bootstrap);
    if(!resolve()) return VK_ERROR_INITIALIZATION_FAILED;
    auto fn=(PFN_vkQueuePresentKHR)GetProcAddress(nativeLoader,"vkQueuePresentKHR");
    auto result=fn(q,p);trace.done(result);return result;
}
static PFN_vkVoidFunction Wrap(const char* n,PFN_vkVoidFunction original) {
    if((!enabled && !bootstrap) || !original || !n) return original;
#define HOOK(name,fn) if(!strcmp(n,name)) return (PFN_vkVoidFunction)&fn;
    if(bootstrap) {
        HOOK("vkCreateInstance",NrCreateInstance)
        HOOK("vkCreateWin32SurfaceKHR",NrCreateSurface)
        HOOK("vkCreateSwapchainKHR",NrCreateSwapchain)
        HOOK("vkQueueSubmit",NrSubmit)
        HOOK("vkWaitForFences",NrWait)
        HOOK("vkAcquireNextImageKHR",NrAcquire)
        HOOK("vkQueuePresentKHR",NrPresent)
    }
    HOOK("vkGetInstanceProcAddr",NrGipa) HOOK("vkGetDeviceProcAddr",NrGdpa)
    if(!enabled) {
        HOOK("vkCreateDevice",NrCreateDevice)
        return original;
    }
    HOOK("vkEnumeratePhysicalDevices",NrEnumerate) HOOK("vkCreateDevice",NrCreateDevice)
    HOOK("vkEnumeratePhysicalDeviceGroups",NrGroups) HOOK("vkEnumeratePhysicalDeviceGroupsKHR",NrGroupsKHR)
    HOOK("vkDestroyInstance",NrDestroyInstance)
    HOOK("vkAllocateMemory",NrAllocateMemory)
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
        arm.configure();
        wchar_t path[32768]{},flag[8]{}; auto count=GetModuleFileNameW(nullptr,path,32768);
        auto base=wcsrchr(path,L'\\'); base=base?base+1:path;
        enabled=count && count<32768 && !_wcsicmp(base,L"RDR2.exe") &&
            GetEnvironmentVariableW(L"DLSSNR_INLINE",flag,8)==1 && flag[0]=='1';
        wchar_t mode[16]{};
        if(count && count<32768 && !_wcsicmp(base,L"RDR2.exe") &&
           GetEnvironmentVariableW(L"DLSSNR_BOOTSTRAP",mode,16)>0) {
            bootstrap=!wcscmp(mode,L"forward") || !wcscmp(mode,L"track") || !wcscmp(mode,L"bda");
            if(bootstrap) {
                enabled=wcscmp(mode,L"forward")!=0;
                augmentBda=!wcscmp(mode,L"bda");
            }
        }
    }
    return TRUE;
}
