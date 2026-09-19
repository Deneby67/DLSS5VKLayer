// Exercise the exact application-local Vulkan shim with a readback SR stand-in.
#include "../helper/vulkan_context.h"
#include "../core/ngx_fg_params.h"
#include "../ngx_capture/params.h"
#include "../core/ngx_caller.h"
#include <cmath>
#include <vector>
#include "../ngx_capture/nr_settings.h"
#include "../ngx_capture/nr_arm.h"
#include "../ngx_capture/sr_preset.h"
using namespace dlssnr;
constexpr unsigned W=1280,H=720;
static VkCtx context;
static GpuImage color{},motion{},exposure{},output{},depth{};
static unsigned errors=0,calls=0,callbacks=0,replacements=0;
static FnVkEvaluateFeature realSr=nullptr;
static NVSDK_NGX_Handle* realHandle=nullptr;
static VkBool32 VKAPI_CALL Validation(VkDebugUtilsMessageSeverityFlagBitsEXT severity,VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data,void*) {
    if(severity&VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++errors;
    Log("[inline-validation] %s",data->pMessage);return VK_FALSE;
}
static NVSDK_NGX_Resource_VK Resource(const GpuImage& i,bool rw) {
    NVSDK_NGX_Resource_VK r{};r.Resource.ImageViewInfo={i.view,i.image,{1,0,1,0,1},i.format,i.width,i.height};
    r.Type=NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGE_VIEW;r.ReadWrite=rw;return r;
}
static void __cdecl Callback(float f) {if(f==.5f) ++callbacks;}
static uint32_t __cdecl Sr(void* command,const void* handle,const void* p,void* callback) {
    ++calls;
    if(handle!=(void*)0x1234 || callback!=(void*)&Callback) return 0xbad00005;
    void* desc=nullptr; if(ngx_capture::Get(p,"Color",&desc,8)!=1) return 0xbad00005;
    auto r=*static_cast<NVSDK_NGX_Resource_VK*>(desc);
    replacements+=r.Resource.ImageViewInfo.Image!=color.image;
    if(realSr) {
        auto result=realSr((VkCommandBuffer)command,realHandle,(const NVSDK_NGX_Parameter*)p,nullptr);
        Log("[inline-real-sr] EvaluateFeature -> %#x",unsigned(result));
        SetLastError(172);return uint32_t(result);
    }
    auto cmd=(VkCommandBuffer)command;
    GpuImage source{};source.image=r.Resource.ImageViewInfo.Image;source.layout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    TransitionImage(context,cmd,source,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_MEMORY_READ_BIT,
        VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
    TransitionImage(context,cmd,output,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageCopy region{};region.srcSubresource={1,0,0,1};region.dstSubresource={1,0,0,1};region.extent={W,H,1};
    vkCmdCopyImage(cmd,source.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,output.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region);
    TransitionImage(context,cmd,source,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    TransitionImage(context,cmd,output,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    Callback(.5f);SetLastError(172);return 1;
}
int main() {
    InlineKeyEdge edge;
    if(edge.update(true,false) || edge.update(true,true) || edge.update(false,true) ||
       !edge.update(true,true) || edge.update(true,true) || edge.update(false,true) ||
       !edge.update(true,true))return 12;
    const bool snapshotTest=getenv("DLSSNR_PROBE_SNAPSHOT")!=nullptr;
    const bool staleSnapshot=getenv("DLSSNR_PROBE_SNAPSHOT_STALE")!=nullptr;
    const bool stress=getenv("DLSSNR_PROBE_STRESS")!=nullptr;
    const bool settingTest=getenv("DLSSNR_PROBE_SETTINGS")!=nullptr;
    InlineSettings testSettings;
    if(settingTest) {
        wchar_t path[32768]{};if(!GetEnvironmentVariableW(L"DLSSNR_INLINE_SHM",path,32768))return 12;
        auto f=CreateFileW(path,GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(f==INVALID_HANDLE_VALUE)return 12;
        std::vector<char> data(kHeaderBytes);ShmInitDefaults((ShmHeader*)data.data());
        DWORD n=0;bool ok=WriteFile(f,data.data(),DWORD(data.size()),&n,nullptr);CloseHandle(f);
        if(!ok || n!=data.size() || !testSettings.read())return 12;
    }
    const bool armCycle=getenv("DLSSNR_PROBE_ARM_CYCLE")!=nullptr;
    const bool realMode=getenv("DLSSNR_PROBE_REAL_SR")!=nullptr;
    const unsigned ow=realMode?W*3/2:W,oh=realMode?H*3/2:H;
    const char* groups=getenv("DLSSNR_PROBE_GROUPS");
    if(!CreateContext(context,getenv("DLSSNR_PROBE_BDA_AUTO")==nullptr,groups?unsigned(atoi(groups)):0,getenv("DLSSNR_PROBE_EXT_BDA")!=nullptr)) return 2;
    auto create=(PFN_vkCreateDebugUtilsMessengerEXT)g_gipa(context.instance,"vkCreateDebugUtilsMessengerEXT");
    VkDebugUtilsMessengerEXT messenger{};
    VkDebugUtilsMessengerCreateInfoEXT mi{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    mi.messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
    mi.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    mi.pfnUserCallback=Validation;
    if(!create || create(context.instance,&mi,nullptr,&messenger)!=VK_SUCCESS) return 2;
    using Eval=uint32_t(__cdecl*)(void*,const void*,const void*,void*,decltype(&Sr));
    auto eval=(Eval)GetProcAddress(GetModuleHandleW(L"vulkan-1.dll"),"DlssNrEvaluate");
    if(!eval) return 3;
    if(!CreateImage2D(context,VK_FORMAT_R16G16B16A16_SFLOAT,W,H,color) ||
       !CreateImage2D(context,VK_FORMAT_R16G16B16A16_SFLOAT,ow,oh,output) ||
       !CreateImage2D(context,VK_FORMAT_R16G16_SFLOAT,W,H,motion) ||
       !CreateImage2D(context,VK_FORMAT_R32_SFLOAT,W,H,depth) ||
       !CreateImage2D(context,VK_FORMAT_R32_SFLOAT,1,1,exposure)) return 4;
    std::vector<uint16_t> pixels(size_t(W)*H*4),mv(size_t(W)*H*2,0);
    for(unsigned y=0;y<H;++y) for(unsigned x=0;x<W;++x) {
        auto i=(size_t(y)*W+x)*4;pixels[i]=x<W/2?0x4000:0x4400;
        pixels[i+1]=y<H/2?0x3800:0x3c00;pixels[i+2]=0x4800;pixels[i+3]=0x3800;
    }
    float ev=1;
    std::vector<float> z(size_t(W)*H,.5f);
    if(!UploadPixels(context,color,pixels.data(),pixels.size()*2) || !UploadPixels(context,motion,mv.data(),mv.size()*2) ||
       !UploadPixels(context,exposure,&ev,4) || !UploadPixels(context,depth,z.data(),z.size()*4)) return 5;
    auto rc=Resource(color,false),rm=Resource(motion,false),re=Resource(exposure,false),ro=Resource(output,true);
    FgParameters p;
    auto rd=Resource(depth,false);p.set("Depth",(void*)&rd);
    p.set("Color",(void*)&rc);p.set("MotionVectors",(void*)&rm);p.set("ExposureTexture",(void*)&re);p.set("Output",(void*)&ro);
    p.set("DLSS.Feature.Create.Flags",43);p.set("Reset",0);p.set("DLSS.Pre.Exposure",1.f);
    p.set("Jitter.Offset.X",.125f);p.set("Jitter.Offset.Y",.25f);p.set("MV.Scale.X",1.f);p.set("MV.Scale.Y",1.f);
    p.set("DLSS.Render.Subrect.Dimensions.Width",W);p.set("DLSS.Render.Subrect.Dimensions.Height",H);
    p.set("Width",W);p.set("Height",H);p.set("OutWidth",ow);p.set("OutHeight",oh);
    p.set("DLSS.Exposure.Scale",1.f);p.set("DLSS.Input.Depth.Subrect.Base.X",0u);p.set("DLSS.Input.Depth.Subrect.Base.Y",0u);
    p.set("DLSS.Output.Subrect.Base.X",0u);p.set("DLSS.Output.Subrect.Base.Y",0u);
    for(const char* key:{"DLSS.Input.Color.Subrect.Base.X","DLSS.Input.Color.Subrect.Base.Y",
                        "DLSS.Input.MV.Subrect.Base.X","DLSS.Input.MV.Subrect.Base.Y"})p.set(key,0u);
    HMODULE srModule{};SpoofState srSpoof{};
    FnVkReleaseFeature srRelease=nullptr;FnVkShutdown1 srShutdown=nullptr;
    if(realMode) {
        wchar_t dll[32768]{};
        if(!GetEnvironmentVariableW(L"DLSSNR_PROBE_REAL_SR",dll,32768))return 10;
        srModule=LoadLibraryExW(dll,nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if(!srModule || !InstallCallerSpoof(srModule,srSpoof,GetModuleHandleW(nullptr)))return 10;
        auto init=(FnVkInitExt)GetProcAddress(srModule,"NVSDK_NGX_VULKAN_Init_Ext");
        auto create=(FnVkCreateFeature)GetProcAddress(srModule,"NVSDK_NGX_VULKAN_CreateFeature");
        realSr=(FnVkEvaluateFeature)GetProcAddress(srModule,"NVSDK_NGX_VULKAN_EvaluateFeature");
        srRelease=(FnVkReleaseFeature)GetProcAddress(srModule,"NVSDK_NGX_VULKAN_ReleaseFeature");
        srShutdown=(FnVkShutdown1)GetProcAddress(srModule,"NVSDK_NGX_VULKAN_Shutdown1");
        if(!init || !create || !realSr || !srRelease || !srShutdown)return 10;
        auto result=init(DLSSNR_SIGNED_SNIPPET_APPLICATION_ID,L".",context.instance,context.physical,context.device,0x14,nullptr);
        Log("[inline-real-sr] init -> %#x",unsigned(result));if(result!=1)return 10;
        FgParameters creation;
        creation.set("Width",W);creation.set("Height",H);creation.set("OutWidth",ow);creation.set("OutHeight",oh);
        creation.set("PerfQualityValue",2);creation.set("DLSS.Feature.Create.Flags",43);
        creation.set("CreationNodeMask",1u);creation.set("VisibilityNodeMask",1u);
        if(!BeginCmd(context.cmdCreate))return 10;
        const char* presetEnv=getenv("DLSSNR_PROBE_SR_PRESET");
        unsigned preset=presetEnv?unsigned(atoi(presetEnv)):0;
        creation.set("DLSS.Hint.Render.Preset.Quality",0u);
        ngx_capture::SrPresetOverlay selected(creation.abi(),preset);
        result=create(context.cmdCreate,1,preset?(NVSDK_NGX_Parameter*)&selected:creation.abi(),&realHandle);
        unsigned unchanged=99;
        if(ngx_capture::Get(creation.abi(),"DLSS.Hint.Render.Preset.Quality",&unchanged,12)!=1 || unchanged)return 13;
        if(preset && !selected.reads)return 13;
        Log("[inline-real-sr] preset=%u reads=%u",preset,selected.reads);
        Log("[inline-real-sr] create -> %#x",unsigned(result));
        if(!SubmitAndWait(context,context.cmdCreate) || result!=1 || !realHandle)return 10;
        // Supply real geometric guidance for this synthetic plane, not game defaults.
        // The stand-in path needs none; direct SR needs a depth image.
    }
    InlineArm captureArm;
    if(settingTest || snapshotTest) {
        captureArm.configure();
        auto token=(ULONGLONG(*)())GetProcAddress(GetModuleHandleW(L"vulkan-1.dll"),"DlssNrArmToken");
        if(!token || !captureArm.configured)return 14;
        captureArm.token=token();
        if(!captureArm.set(true))return 14;
    }
    auto before=ngx_capture::Snapshot(&p);
    unsigned changed=0,high=0,alpha=0;
    const unsigned frameCount=stress?130:settingTest?8:armCycle?4:3;
    for(unsigned frame=0;frame<frameCount;++frame) {
        if(stress) {
            VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            allocation.commandPool=context.cmdPool;allocation.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;allocation.commandBufferCount=1;
            if(vkAllocateCommandBuffers(context.device,&allocation,&context.cmdEval)!=VK_SUCCESS)return 12;
        }
        if(settingTest) {
            auto* h=testSettings.header;
            if(frame==0 || frame>=4)h->intensityBits=FloatToBits(.65f);
            h->localToneBits=FloatToBits(.9f);h->localStructureBits=FloatToBits(.8f);
            h->skinStructureBits=FloatToBits(.25f);h->autoMask=0;
            if(frame==1)h->intensityBits=FloatToBits(1.5f);
            h->enabled=(frame==3)?0:1;
            h->sharpnessBits=FloatToBits(frame==7?.4f:0.f);
            if(frame==6)h->intensityBits=FloatToBits(NAN);
            h->controlSeq.fetch_add(1);
            if(frame==2 || frame==5)Sleep(800);
        }
        if(armCycle) {
            wchar_t path[32768]{};if(!GetEnvironmentVariableW(L"DLSSNR_ARM_FILE",path,32768))return 11;
            if(frame==0 || frame==3)DeleteFileW(path);
            else {
                auto token=(ULONGLONG(*)())GetProcAddress(GetModuleHandleW(L"vulkan-1.dll"),"DlssNrArmToken");
                if(!token)return 11;
                InlineArm writer;writer.configure();writer.token=token()+(frame==1?1:0);
                if(!writer.set(true))return 11;
            }
            Sleep(275);
        }
        if((snapshotTest && frame==0) || (settingTest && frame==7)) {
            std::wstring path=captureArm.file;
            path=path.substr(0,path.find_last_of(L"/\\")+1)+L"hdr-capture.request";
            FILE* f=_wfopen(path.c_str(),L"w");if(!f)return 14;
            fprintf(f,"%lu %llu\n",GetCurrentProcessId(),captureArm.token+(staleSnapshot?1:0));fclose(f);
            if(settingTest)Sleep(550);
        }
        if(snapshotTest)Sleep(550);
        if(snapshotTest && frame==0) {
            if(vkResetCommandBuffer(context.cmdEval,0)!=VK_SUCCESS)return 14;
            VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            if(vkBeginCommandBuffer(context.cmdEval,&bi)!=VK_SUCCESS)return 14;
        } else if(!BeginCmd(context.cmdEval)) return 6;
        for(auto* i:{&color,&motion,&exposure,&depth})TransitionImage(context,context.cmdEval,*i,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        if(realMode)TransitionImage(context,context.cmdEval,output,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,
            VK_ACCESS_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        if(eval(context.cmdEval,(void*)0x1234,&p,(void*)&Callback,&Sr)!=1 || GetLastError()!=172) return 6;
        if(realMode) output.layout=VK_IMAGE_LAYOUT_GENERAL;
        if(!SubmitAndWait(context,context.cmdEval) || !ReadbackPixels(context,output,size_t(ow)*oh*8)) return 7;
        auto result=(uint16_t*)context.readMap;changed=high=alpha=0;
        for(size_t i=0;i<size_t(ow)*oh*4;++i) {
            if((result[i]&0x7c00)==0x7c00) return 7;
            if(i<pixels.size())changed+=result[i]!=pixels[i];
            if(i%4==3) {if(i<pixels.size())alpha+=result[i]==pixels[i];}else high+=(result[i]&0x8000)==0 && result[i]>0x3c00;
        }
        if(before!=ngx_capture::Snapshot(&p)) return 8;
        if(stress && changed<=100)return 12;
        if(settingTest && ((frame==3 || frame==6)?changed!=0:changed<=100))return 12;
        if(armCycle && ((frame==2)?changed<=100:changed!=0))return 11;
        if(armCycle && frame<2 && GetModuleHandleW(L"nvngx_dlssnr.dll"))return 11;
    }
    if(realMode) {
        uint64_t digest=14695981039346656037ull;
        const auto* bytes=(const unsigned char*)context.readMap;
        for(size_t i=0;i<size_t(ow)*oh*8;++i){digest^=bytes[i];digest*=1099511628211ull;}
        Log("[inline-real-sr] output checksum=%016llx",(unsigned long long)digest);
    }
    testSettings.close();
    vkDeviceWaitIdle(context.device);
    if(realMode){srRelease(realHandle);srShutdown(context.device);RemoveCallerSpoof(srSpoof);FreeLibrary(srModule);}
    for(auto* i:{&color,&motion,&exposure,&output,&depth})DestroyImage2D(context,*i);
    if(context.cmdPool)vkDestroyCommandPool(context.device,context.cmdPool,nullptr);
    if(context.cmdPoolFlow)vkDestroyCommandPool(context.device,context.cmdPoolFlow,nullptr);
    for(auto s:{context.semPrep,context.semFlow})if(s)vkDestroySemaphore(context.device,s,nullptr);
    if(context.flowQuery)vkDestroyQueryPool(context.device,context.flowQuery,nullptr);
    for(auto shader:{context.mvShaderFixed5,context.mvShaderFloat})if(shader)vkDestroyShaderModule(context.device,shader,nullptr);
    if(context.mvPipeLayout)vkDestroyPipelineLayout(context.device,context.mvPipeLayout,nullptr);
    if(context.mvDescLayout)vkDestroyDescriptorSetLayout(context.device,context.mvDescLayout,nullptr);
    for(auto buf:{context.uploadStaging,context.readStaging,context.queryStaging})if(buf)vkDestroyBuffer(context.device,buf,nullptr);
    for(auto mem:{context.uploadMem,context.readMem,context.queryMem})if(mem){vkUnmapMemory(context.device,mem);vkFreeMemory(context.device,mem,nullptr);}
    for(auto fence:context.fences)if(fence)vkDestroyFence(context.device,fence,nullptr);
    // The shim must clean up only its own resources before forwarding destroy.
    vkDestroyDevice(context.device,nullptr);
    const bool expectNr=getenv("DLSSNR_EXPECT_BYPASS")==nullptr;
    bool pass=calls==frameCount && (realMode || callbacks==frameCount) && replacements==(settingTest?6u:armCycle?1u:(expectNr?frameCount:0u)) &&
        ((expectNr && !armCycle)?changed>100:changed==0) && high>100 && (realMode || alpha==W*H) && errors==0;
    Log("[inline-probe] %s calls=%u callbacks=%u replacements=%u changed=%u hdr=%u alpha=%u validationErrors=%u",
        pass?"PASS":"FAIL",calls,callbacks,replacements,changed,high,alpha,errors);
    return pass?0:9;
}
