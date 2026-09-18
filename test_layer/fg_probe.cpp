// Offscreen conformance gate. No swapchain, shared-memory session, or game injection.
#include "../core/ngx_fg.h"
#include "../core/guard.h"
#include "../helper/vulkan_context.h"
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <vector>
#include <shellapi.h>

using namespace dlssnr;
constexpr unsigned W = 1280, H = 720;
static unsigned validationErrors=0;
static VKAPI_ATTR VkBool32 VKAPI_CALL Validation(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++validationErrors;
    Log("[fg-validation] %s", data->pMessage);
    return VK_FALSE;
}

static NVSDK_NGX_Resource_VK Resource(const GpuImage& i, bool write) {
    NVSDK_NGX_Resource_VK r{};
    r.Resource.ImageViewInfo = {i.view, i.image, {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}, i.format, i.width, i.height};
    r.Type = NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGE_VIEW; r.ReadWrite = write;
    return r;
}
static void Ppm(const char* name, const uint8_t* data) {
    FILE* f = fopen(name, "wb");
    if (!f) return;
    fprintf(f, "P6\n%u %u\n255\n", W, H);
    for (size_t i = 0; i < size_t(W)*H; ++i) fwrite(data+4*i, 1, 3, f);
    fclose(f);
}
static double Center(const uint8_t* p) {
    double sum = 0; size_t count = 0;
    for (unsigned y=H/3+12; y<2*H/3-12; ++y) for (unsigned x=0; x<W; ++x) {
        size_t i=(size_t(y)*W+x)*4;
        if (p[i]<90 && p[i+1]>180 && p[i+2]>180) { sum+=x; ++count; }
    }
    return count ? sum/count : -1;
}
int FgProbeMain(int argc, char** argv) {
    validationErrors=0;
    InstallGuard();
    if (argc != 2) { fprintf(stderr,"usage: fg_probe.exe Z:/absolute/nvngx_dlssg.dll\n"); return 2; }
    // Wine's ANSI argv can lose characters before we see it. Read the original
    // wide command line for both standalone and --fg-self-test entry points.
    int wideCount=0;
    auto wide=CommandLineToArgvW(GetCommandLineW(),&wideCount);
    if (!wide || wideCount<2) { if (wide) LocalFree(wide); return 2; }
    std::wstring dll=wide[wideCount-1];
    LocalFree(wide);
    VkCtx c{}; NgxFg fg;
    if (!CreateContext(c,true)) return 3;
    VkDebugUtilsMessengerEXT messenger=VK_NULL_HANDLE;
    auto createMessenger=reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(g_gipa(c.instance,"vkCreateDebugUtilsMessengerEXT"));
    auto destroyMessenger=reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(g_gipa(c.instance,"vkDestroyDebugUtilsMessengerEXT"));
    if (getenv("DLSSFG_VALIDATE")) {
        if (!createMessenger || !destroyMessenger) {
            Log("[fg-probe] validation requested but debug messenger is unavailable");
            return 3;
        }
        VkDebugUtilsMessengerCreateInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        info.messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        info.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        info.pfnUserCallback=Validation;
        if (createMessenger(c.instance,&info,nullptr,&messenger)!=VK_SUCCESS) return 3;
    }
    auto gdpa=reinterpret_cast<PFN_vkGetDeviceProcAddr>(g_gipa(c.instance,"vkGetDeviceProcAddr"));
    if (!fg.initialize(dll,L".",c.instance,c.physical,c.device,g_gipa,gdpa)) {
        fprintf(stderr,"FG GATE FAIL: %s\n",fg.error().c_str()); fg.shutdown(); return 4;
    }
    if (!BeginCmd(c.cmdCreate)) return 5;
    bool created=fg.create(c.cmdCreate,W,H,VK_FORMAT_R8G8B8A8_UNORM);
    if (!SubmitAndWait(c,c.cmdCreate) || !created) {
        fprintf(stderr,"FG GATE FAIL: %s\n",fg.error().c_str()); fg.shutdown(); return 5;
    }
    GpuImage color{},depth{},motion{},interp{},real{};
    if (!CreateImage2D(c,VK_FORMAT_R8G8B8A8_UNORM,W,H,color) ||
        !CreateImage2D(c,VK_FORMAT_R32_SFLOAT,W,H,depth) ||
        !CreateImage2D(c,VK_FORMAT_R16G16_SFLOAT,W,H,motion) ||
        !CreateImage2D(c,VK_FORMAT_R8G8B8A8_UNORM,W,H,interp) ||
        !CreateImage2D(c,VK_FORMAT_R8G8B8A8_UNORM,W,H,real)) return 6;
    VkBuffer disable=VK_NULL_HANDLE; VkDeviceMemory disableMemory=VK_NULL_HANDLE; void* disableMap=nullptr;
    Log("[fg-probe] images color=%p depth=%p motion=%p interp=%p real=%p",color.image,depth.image,motion.image,interp.image,real.image);
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size=4;
    bi.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    if (vkCreateBuffer(c.device,&bi,nullptr,&disable)!=VK_SUCCESS) return 6;
    VkMemoryRequirements req{}; vkGetBufferMemoryRequirements(c.device,disable,&req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize=req.size;
    VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    flags.flags=VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT; ai.pNext=&flags;
    ai.memoryTypeIndex=FindMemoryType(c,req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (ai.memoryTypeIndex==UINT32_MAX || vkAllocateMemory(c.device,&ai,nullptr,&disableMemory)!=VK_SUCCESS ||
        vkBindBufferMemory(c.device,disable,disableMemory,0)!=VK_SUCCESS ||
        vkMapMemory(c.device,disableMemory,0,VK_WHOLE_SIZE,0,&disableMap)!=VK_SUCCESS) return 6;
    NVSDK_NGX_Resource_VK rs{}; rs.Type=NVSDK_NGX_RESOURCE_VK_TYPE_VK_BUFFER;
    rs.Resource.BufferInfo={disable,4}; rs.ReadWrite=true;
    FgCamera camera{};
    camera.nearPlane=.1f; camera.farPlane=100; camera.fov=1.0471975512f; camera.aspect=float(W)/H;
    camera.right[0]=1; camera.up[1]=1; camera.forward[2]=1;
    const float sy=1/std::tan(camera.fov/2), sx=sy/camera.aspect;
    const float a=camera.farPlane/(camera.farPlane-camera.nearPlane), b=-camera.nearPlane*a;
    camera.viewToClip[0]=sx; camera.viewToClip[5]=sy;
    camera.viewToClip[10]=a; camera.viewToClip[11]=1; camera.viewToClip[14]=b;
    camera.clipToView[0]=1/sx; camera.clipToView[5]=1/sy;
    camera.clipToView[11]=1/b; camera.clipToView[14]=1; camera.clipToView[15]=-a/b;
    for (int i=0;i<4;++i) camera.clipToPrevClip[i*5]=camera.prevClipToClip[i*5]=1;
    std::vector<uint8_t> pixels(size_t(W)*H*4), previous;
    std::vector<float> depths(size_t(W)*H);
    std::vector<uint16_t> mvecs(size_t(W)*H*2);
    unsigned interpolated=0; bool failed=false;
    for (unsigned frame=0;frame<12;++frame) {
        const unsigned left=W/4+frame*8, right=left+160;
        for (unsigned y=0;y<H;++y) for (unsigned x=0;x<W;++x) {
            const size_t i=size_t(y)*W+x;
            const bool object=x>=left && x<right && y>=H/3 && y<2*H/3;
            const uint8_t bg=((x/32+y/32)%2)?60:30;
            pixels[4*i]=object?20:bg; pixels[4*i+1]=object?230:bg; pixels[4*i+2]=object?230:bg; pixels[4*i+3]=255;
            depths[i]=a+b/(object?5.0f:20.0f);
            mvecs[2*i]=object?0xc800:0; // -8 pixels, IEEE binary16, current -> previous.
            mvecs[2*i+1]=0;
        }
        if (!UploadPixels(c,color,pixels.data(),pixels.size()) ||
            !UploadPixels(c,depth,depths.data(),depths.size()*4) ||
            !UploadPixels(c,motion,mvecs.data(),mvecs.size()*2) || !BeginCmd(c.cmdEval)) { failed=true; break; }
        for (auto* i : {&color,&depth,&motion}) TransitionImage(c,c.cmdEval,*i,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        for (auto* i : {&interp,&real}) TransitionImage(c,c.cmdEval,*i,VK_IMAGE_LAYOUT_GENERAL,
            VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        auto rc=Resource(color,false),rd=Resource(depth,false),rm=Resource(motion,false);
        auto ri=Resource(interp,true),rr=Resource(real,true);
        *static_cast<uint32_t*>(disableMap)=0xffffffffu;
        bool evaluated=fg.evaluate(c.cmdEval,rc,rd,rm,ri,rr,rs,camera,frame==0);
        // This snippet finishes its backbuffer access in GENERAL. Track the
        // actual layout before the next upload rather than emitting a stale
        // SHADER_READ_ONLY -> TRANSFER_DST transition on the next frame.
        if (evaluated) color.layout=VK_IMAGE_LAYOUT_GENERAL;
        VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; host.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT; host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(c.cmdEval,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
        if (!SubmitAndWait(c,c.cmdEval) || !evaluated) { failed=true; break; }
        const bool skip=*static_cast<uint8_t*>(disableMap)!=0;
        if (!ReadbackPixels(c,interp,pixels.size())) { failed=true; break; }
        const auto* output=static_cast<const uint8_t*>(c.readMap);
        const double center=Center(output),expected=left+79.5-4;
        const bool unique=frame && memcmp(output,pixels.data(),pixels.size()) && memcmp(output,previous.data(),previous.size());
        const bool valid=frame>2 && !skip && unique && std::abs(center-expected)<=2.0;
        interpolated+=unsigned(valid);
        printf("frame=%u skip=%u unique=%u center=%.2f expected=%.2f valid=%u\n",frame,unsigned(skip),unsigned(unique),center,expected,unsigned(valid));
        Log("[fg-probe] frame=%u skip=%u unique=%u center=%.2f expected=%.2f valid=%u",frame,unsigned(skip),unsigned(unique),center,expected,unsigned(valid));
        fflush(stdout);
        if (frame==10) { Ppm("fg-interpolated.ppm",output); Ppm("fg-current.ppm",pixels.data()); Ppm("fg-previous.ppm",previous.data()); }
        previous=pixels;
    }
    vkDeviceWaitIdle(c.device);
    fg.shutdown();
    for (auto* i : {&color,&depth,&motion,&interp,&real}) DestroyImage2D(c,*i);
    vkUnmapMemory(c.device,disableMemory); vkDestroyBuffer(c.device,disable,nullptr); vkFreeMemory(c.device,disableMemory,nullptr);
    if (c.cmdPool) vkDestroyCommandPool(c.device,c.cmdPool,nullptr);
    if (c.cmdPoolFlow) vkDestroyCommandPool(c.device,c.cmdPoolFlow,nullptr);
    for (auto s : {c.semPrep,c.semFlow}) if (s) vkDestroySemaphore(c.device,s,nullptr);
    if (c.flowQuery) vkDestroyQueryPool(c.device,c.flowQuery,nullptr);
    for (auto shader : {c.mvShaderFixed5,c.mvShaderFloat}) if (shader) vkDestroyShaderModule(c.device,shader,nullptr);
    if (c.mvPipeLayout) vkDestroyPipelineLayout(c.device,c.mvPipeLayout,nullptr);
    if (c.mvDescLayout) vkDestroyDescriptorSetLayout(c.device,c.mvDescLayout,nullptr);
    for (auto buf : {c.uploadStaging,c.readStaging,c.queryStaging}) if (buf) vkDestroyBuffer(c.device,buf,nullptr);
    for (auto mem : {c.uploadMem,c.readMem,c.queryMem}) if (mem) { vkUnmapMemory(c.device,mem); vkFreeMemory(c.device,mem,nullptr); }
    for (auto f : c.fences) if (f) vkDestroyFence(c.device,f,nullptr);
    vkDestroyDevice(c.device,nullptr);
    if (messenger) destroyMessenger(c.instance,messenger,nullptr);
    vkDestroyInstance(c.instance,nullptr);
    const bool pass=!failed && interpolated>=6 && validationErrors==0;
    printf("FG GATE %s: verified intermediate frames=%u/9; %s\n",pass?"PASS":"FAIL",interpolated,fg.error().c_str());
    Log("[fg-probe] FG GATE %s: verified intermediate frames=%u/9 validationErrors=%u; %s",pass?"PASS":"FAIL",interpolated,validationErrors,fg.error().c_str());
    return pass?0:7;
}
#ifndef DLSSFG_EMBED_PROBE
int main(int argc,char** argv) { return FgProbeMain(argc,argv); }
#endif
