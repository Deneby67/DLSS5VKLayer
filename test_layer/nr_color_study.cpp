// Offline same-image NR study. Static repeated input, zero motion/jitter and no
// depth: this is explicitly not a replay of the game's temporal history.
#include "../core/ngx_snippet.h"
#include "../core/guard.h"
#include "../helper/vulkan_context.h"
#include <vector>
#include <cmath>
using namespace dlssnr;
static unsigned errors=0;
static VKAPI_ATTR VkBool32 VKAPI_CALL Validation(VkDebugUtilsMessageSeverityFlagBitsEXT s,
    VkDebugUtilsMessageTypeFlagsEXT,const VkDebugUtilsMessengerCallbackDataEXT* d,void*) {
    if(s&VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)++errors;
    Log("[study-validation] %s",d->pMessage);return VK_FALSE;
}
static NVSDK_NGX_Resource_VK Resource(const GpuImage& i,bool rw) {
    NVSDK_NGX_Resource_VK r{};r.Resource.ImageViewInfo={i.view,i.image,{1,0,1,0,1},i.format,i.width,i.height};
    r.Type=NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGE_VIEW;r.ReadWrite=rw;return r;
}
int main() {
    unsigned w=0,h=0,hdr=0,frames=0;float sharp=0;NgxTuning t;
    FILE* file=fopen("settings.txt","r");if(!file)return 2;
    auto count=fscanf(file,"%u %u %u %u %f %f %f %f %f %u %u %u",&w,&h,&hdr,&frames,&sharp,
                      &t.intensity,&t.localTone,&t.localStructure,&t.skinStructure,&t.style,&t.preset,&t.autoMask);
    fclose(file);
    if(count!=12 || w<64 || h<64 || w>4096 || h>2160 || hdr>1 || frames<2 || frames>32 ||
       !std::isfinite(sharp) || sharp<0 || sharp>1 || t.style>2 || t.preset>15 || t.autoMask>1)return 2;
    std::vector<uint16_t> input(size_t(w)*h*4),motion(size_t(w)*h*2,0);
    file=fopen("input.rgba16f","rb");if(!file)return 2;
    count=fread(input.data(),2,input.size(),file);bool eof=fgetc(file)==EOF;fclose(file);
    if(count!=input.size() || !eof)return 2;
    for(auto half:input)if((half&0x7c00)==0x7c00)return 2;
    InstallGuard();g_layerModule=GetModuleHandleW(nullptr);
    VkCtx c{};NgxSnippet nr;nr.hdrActive=hdr!=0;
    if(!CreateContext(c,true))return 3;
    auto create=(PFN_vkCreateDebugUtilsMessengerEXT)g_gipa(c.instance,"vkCreateDebugUtilsMessengerEXT");
    VkDebugUtilsMessengerEXT messenger{};
    VkDebugUtilsMessengerCreateInfoEXT di{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    di.messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
    di.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    di.pfnUserCallback=Validation;
    if(!create || create(c.instance,&di,nullptr,&messenger)!=VK_SUCCESS)return 3;
    if(!BeginCmd(c.cmdCreate))return 4;
    bool initialized=NgxLoadAndInit(nr,c.instance,c.physical,c.device,w,h,c.cmdCreate,t);
    if(!SubmitAndWait(c,c.cmdCreate) || !initialized || nr.hdrActive!=bool(hdr))return 4;
    // Public NVIDIA ABI: four arguments; the output reports platform support,
    // not HDR/color-mode capability. Keep this diagnostic separate from the
    // historical three-argument query in the production adapter under study.
    struct Project {const char* id;unsigned engine;const char* version;};
    struct Identity {unsigned type;union {Project project;unsigned long long app;} value;};
    struct Discovery {unsigned sdk,feature;Identity id;const wchar_t* data;const void* common;};
    struct Requirements {unsigned supported,architecture;char os[255];};
    static_assert(sizeof(Discovery)==56 && sizeof(Requirements)==264);
    auto query=(NVSDK_NGX_Result(NVSDK_CONV*)(VkInstance,VkPhysicalDevice,const Discovery*,Requirements*))
        GetProcAddress(nr.snippet,"NVSDK_NGX_VULKAN_GetFeatureRequirements");
    Discovery discovery{};discovery.sdk=0x14;discovery.feature=18;
    discovery.id.value.app=DLSSNR_SIGNED_SNIPPET_APPLICATION_ID;discovery.data=L".";
    Requirements requirements{};DWORD seh=0;
    auto queryResult=query?Guarded([&]{return query(c.instance,c.physical,&discovery,&requirements);},NVSDK_NGX_Result_FAIL_SEH,&seh):NVSDK_NGX_Result_FAIL_SEH;
    Log("[color-study] requirements correct ABI result=%#x seh=%#x supported=%#x architecture=%#x (no HDR capability field)",
        unsigned(queryResult),seh,requirements.supported,requirements.architecture);
    if(queryResult!=1 || seh || !nr.requirementsKnown || nr.featureSupport!=requirements.supported ||
       nr.minimumArchitecture!=requirements.architecture)return 10;
    if(hdr)Log("[color-study] HDR is an explicit unverified contract experiment, NOT capability-confirmed");
    // Isolated flag-numbering control; never consumed by the game adapter.
    if(const char* requestedFlags=getenv("NR_COLOR_STUDY_FLAGS")) {
        unsigned flags=unsigned(strtoul(requestedFlags,nullptr,0));
        if(flags>0xff)return 9;
        NgxReleasePass(nr,0,c.device);
        nr.params->Set("Feature_Flags",flags|(hdr?1u:0u));
        nr.params->Set("NVSDK_NGX_Parameter_Feature_Flags",flags|(hdr?1u:0u));
        if(!BeginCmd(c.cmdCreate))return 9;
        DWORD fault=0;
        auto result=Guarded([&]{return nr.createFeature(c.cmdCreate,18,nr.params,&nr.features[0]);},NVSDK_NGX_Result_FAIL_SEH,&fault);
        if(!SubmitAndWait(c,c.cmdCreate) || result!=1 || !nr.features[0])return 9;
        Log("[color-study] explicit create flags=%#x result=%#x seh=%#x",flags|(hdr?1u:0u),unsigned(result),fault);
    }
    GpuImage color{},out{},mv{};
    if(!CreateImage2D(c,VK_FORMAT_R16G16B16A16_SFLOAT,w,h,color) ||
       !CreateImage2D(c,VK_FORMAT_R16G16B16A16_SFLOAT,w,h,out) ||
       !CreateImage2D(c,VK_FORMAT_R16G16_SFLOAT,w,h,mv))return 5;
    if(!UploadPixels(c,color,input.data(),input.size()*2) || !UploadPixels(c,mv,motion.data(),motion.size()*2))return 5;
    for(unsigned frame=0;frame<frames;++frame) {
        if(!BeginCmd(c.cmdEval))return 6;
        for(auto* i:{&color,&mv})TransitionImage(c,c.cmdEval,*i,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_MEMORY_READ_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        TransitionImage(c,c.cmdEval,out,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_MEMORY_READ_BIT,
            VK_ACCESS_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        NgxSetResources(nr,Resource(color,false),Resource(out,true),Resource(mv,false),{},w,h);
        NgxSetReset(nr,frame==0);NgxSetSharpness(nr,sharp);
        if(!NgxEvaluatePass(nr,0,c.cmdEval) || !SubmitAndWait(c,c.cmdEval) || !ReadbackPixels(c,out,input.size()*2))return 6;
        auto* data=static_cast<const uint16_t*>(c.readMap);
        unsigned nonfinite=0;for(size_t i=0;i<input.size();++i)nonfinite+=(data[i]&0x7c00)==0x7c00;
        if(nonfinite)return 7;
        if(frame==0 || frame==frames/2 || frame==frames-1) {
            char name[64];snprintf(name,sizeof(name),"output-%02u.rgba16f",frame);
            file=fopen(name,"wb");if(!file)return 7;
            bool ok=fwrite(data,2,input.size(),file)==input.size();ok=fclose(file)==0 && ok;
            if(!ok)return 7;
        }
    }
    vkDeviceWaitIdle(c.device);NgxTeardown(nr,c.device);
    for(auto* i:{&color,&out,&mv})DestroyImage2D(c,*i);
    Log("[color-study] %s frames=%u hdr=%u sharpness=%.4f validationErrors=%u",errors?"FAIL":"PASS",frames,hdr,sharp,errors);
    return errors?8:0;
}
