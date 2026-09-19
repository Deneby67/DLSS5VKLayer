// Isolated NR HDR gate: real GPU output, no game or presentation modification.
#include "../core/ngx_snippet.h"
#include "../core/guard.h"
#include "../helper/vulkan_context.h"
#include "../ngx_capture/nr_color.h"
#include <algorithm>
#include <cmath>
#include <vector>
using namespace dlssnr;
constexpr unsigned W=1280,H=720;
static unsigned errors=0;
static VKAPI_ATTR VkBool32 VKAPI_CALL Validation(VkDebugUtilsMessageSeverityFlagBitsEXT s,
    VkDebugUtilsMessageTypeFlagsEXT,const VkDebugUtilsMessengerCallbackDataEXT* d,void*) {
    if(s&VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++errors;
    Log("[nr-validation] %s",d->pMessage); return VK_FALSE;
}
static NVSDK_NGX_Resource_VK Resource(const GpuImage& i,bool rw) {
    NVSDK_NGX_Resource_VK r{};
    r.Resource.ImageViewInfo={i.view,i.image,{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1},i.format,i.width,i.height};
    r.Type=NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGE_VIEW; r.ReadWrite=rw; return r;
}
static float Half(uint16_t h) {
    const unsigned e=(h>>10)&31,m=h&1023;
    float f=e==31?(m?NAN:INFINITY):(e?std::ldexp(1.f+float(m)/1024,int(e)-15):std::ldexp(float(m),-24));
    return h&0x8000?-f:f;
}
int main() {
    InstallGuard(); g_layerModule=GetModuleHandleW(nullptr);
    const bool bridgeMode=getenv("DLSSNR_PROBE_BRIDGE")!=nullptr;
    VkCtx c{}; NgxSnippet nr; NrColorBridge bridge;
    if(!CreateContext(c,true)) return 2; // NR also needs bufferDeviceAddress.
    VkDebugUtilsMessengerEXT messenger{};
    auto create=(PFN_vkCreateDebugUtilsMessengerEXT)g_gipa(c.instance,"vkCreateDebugUtilsMessengerEXT");
    if(getenv("DLSSFG_VALIDATE")) {
        VkDebugUtilsMessengerCreateInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        info.messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
        info.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        info.pfnUserCallback=Validation;
        if(!create || create(c.instance,&info,nullptr,&messenger)!=VK_SUCCESS) return 2;
    }
    nr.hdrActive=!bridgeMode;
    if(!BeginCmd(c.cmdCreate)) return 3;
    const bool initialized=NgxLoadAndInit(nr,c.instance,c.physical,c.device,W,H,c.cmdCreate,{});
    if(!SubmitAndWait(c,c.cmdCreate) || !initialized || (!bridgeMode && !nr.hdrActive)) {
        Log("[nr-hdr] FAIL: HDR create unavailable"); return 3;
    }
    GpuImage color{},out{},mv{},depth{},exposure{};
    if(!CreateImage2D(c,VK_FORMAT_R16G16B16A16_SFLOAT,W,H,color) ||
       !CreateImage2D(c,VK_FORMAT_R16G16B16A16_SFLOAT,W,H,out) ||
       !CreateImage2D(c,VK_FORMAT_R16G16_SFLOAT,W,H,mv) ||
       !CreateImage2D(c,VK_FORMAT_R32_SFLOAT,W,H,depth) ||
       !CreateImage2D(c,VK_FORMAT_R32_SFLOAT,1,1,exposure)) return 4;
    if(bridgeMode && !bridge.initialize(c,W,H)) return 4;
    float exposureValue=1;
    if(!UploadPixels(c,exposure,&exposureValue,sizeof(exposureValue))) return 4;
    std::vector<uint16_t> pixels(size_t(W)*H*4),motion(size_t(W)*H*2,0);
    std::vector<float> depths(size_t(W)*H,.5f);
    // A stationary synthetic scene includes values beyond SDR white (1,2,4,8).
    const uint16_t values[]={0x3800,0x3c00,0x4000,0x4400,0x4800};
    for(unsigned y=0;y<H;++y) for(unsigned x=0;x<W;++x) {
        auto i=(size_t(y)*W+x)*4;
        for(unsigned k=0;k<3;++k) pixels[i+k]=values[((x/96+y/72+k)%5)];
        pixels[i+3]=0x3c00;
    }
    unsigned valid=0;
    for(unsigned frame=0;frame<4;++frame) {
        if(!UploadPixels(c,color,pixels.data(),pixels.size()*2) ||
           !UploadPixels(c,mv,motion.data(),motion.size()*2) ||
           !UploadPixels(c,depth,depths.data(),depths.size()*4) || !BeginCmd(c.cmdEval)) return 5;
        for(auto* i:{&color,&mv,&depth,&exposure}) TransitionImage(c,c.cmdEval,*i,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        TransitionImage(c,c.cmdEval,out,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,
            VK_ACCESS_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        GpuImage* input=&color; GpuImage* output=&out;
        if(bridgeMode) {
            if(!bridge.prepare(c.cmdEval,color.view,exposure.view)) return 5;
            bridge.dispatch(c.cmdEval,0,1);
            input=&bridge.encoded; output=&bridge.model;
            TransitionImage(c,c.cmdEval,*output,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,
                VK_ACCESS_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        }
        NgxSetResources(nr,Resource(*input,false),Resource(*output,true),Resource(mv,false),Resource(depth,false),W,H);
        NgxSetReset(nr,frame==0);
        const bool evaluated=NgxEvaluatePass(nr,0,c.cmdEval);
        if(bridgeMode) {
            TransitionImage(c,c.cmdEval,*output,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_SHADER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
            bridge.dispatch(c.cmdEval,1,1); output=&bridge.restored;
        }
        if(!SubmitAndWait(c,c.cmdEval) || !evaluated || !ReadbackPixels(c,*output,pixels.size()*2)) return 6;
        auto* result=static_cast<const uint16_t*>(c.readMap);
        size_t finite=0,overWhite=0,changed=0; float maximum=0;
        for(size_t i=0;i<pixels.size();++i) {
            float f=Half(result[i]); finite+=std::isfinite(f); changed+=result[i]!=pixels[i];
            if(i%4!=3 && std::isfinite(f)) { overWhite+=f>1.001f; maximum=std::max(maximum,f); }
        }
        const bool good=finite==pixels.size() && overWhite>100 && changed>100;
        valid+=good;
        Log("[nr-hdr] frame=%u valid=%u finite=%llu overWhite=%llu changed=%llu max=%g",frame,unsigned(good),
            (unsigned long long)finite,(unsigned long long)overWhite,(unsigned long long)changed,maximum);
    }
    bool identity=!bridgeMode;
    if(bridgeMode) {
        if(!BeginCmd(c.cmdEval) || !bridge.prepare(c.cmdEval,color.view,exposure.view)) return 8;
        bridge.dispatch(c.cmdEval,0,1);
        TransitionImage(c,c.cmdEval,bridge.encoded,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        TransitionImage(c,c.cmdEval,bridge.model,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_ACCESS_SHADER_READ_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageCopy copy{};copy.srcSubresource={1,0,0,1};copy.dstSubresource={1,0,0,1};copy.extent={W,H,1};
        vkCmdCopyImage(c.cmdEval,bridge.encoded.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,bridge.model.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
        for(auto* i:{&bridge.encoded,&bridge.model}) TransitionImage(c,c.cmdEval,*i,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        bridge.dispatch(c.cmdEval,1,1);
        if(!SubmitAndWait(c,c.cmdEval) || !ReadbackPixels(c,bridge.restored,pixels.size()*2)) return 8;
        identity=!memcmp(c.readMap,pixels.data(),pixels.size()*2);
        Log("[nr-hdr] unchanged-model HDR round trip exact=%u",unsigned(identity));
    }
    vkDeviceWaitIdle(c.device); NgxTeardown(nr,c.device);
    bridge.shutdown();
    for(auto* i:{&color,&out,&mv,&depth,&exposure}) DestroyImage2D(c,*i);
    const bool passed=valid==4 && identity && errors==0;
    Log("[nr-hdr] %s frames=%u validationErrors=%u",passed?"PASS":"FAIL",valid,errors);
    return passed?0:7;
}
