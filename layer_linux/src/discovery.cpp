#include "discovery.h"
#include "../../third_party/nlohmann/json.hpp"
#include <array>
#include <algorithm>
#include <cerrno>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <fcntl.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>
#include <strings.h>

namespace dlssfg::discovery {
using Json=nlohmann::json;
constexpr uint64_t MaxLog=64ull<<20, MaxShaders=128ull<<20, MaxObjects=500000, MaxDescriptorLog=16ull<<20;
template<class T> static uint64_t Handle(T h) {
    if constexpr (std::is_pointer_v<T>) return reinterpret_cast<uintptr_t>(h);
    else return uint64_t(h);
}
static bool Enabled() noexcept {
    static const bool enabled=[] {
        const char* path=getenv("DLSSFG_DISCOVERY_DIR");
        if (!path || path[0]!='/') return false;
        char name[4096]{}; FILE* f=fopen("/proc/self/cmdline","rb");
        if (!f) return false;
        const auto n=fread(name,1,sizeof(name),f); fclose(f);
        if (!n || !memchr(name,0,n)) return false;
        const char* base=name;
        for (const char* p=name;*p;++p) if (*p=='/' || *p=='\\') base=p+1;
        return !strcasecmp(base,"RDR2.exe");
    }();
    return enabled;
}
struct Fd {
    int fd=-1;
    explicit Fd(int value):fd(value) {}
    ~Fd() { if (fd>=0) close(fd); }
    Fd(const Fd&)=delete;
};
static void Require(bool ok,const char* message) { if (!ok) throw std::runtime_error(message); }
// Optional runtime dependency: absence only disables discovery. Steam runtimes
// provide libcrypto.so.3; keeping the lookup dynamic preserves ordinary launch.
static std::string Sha256(const void* bytes,size_t length) {
    using Hash=unsigned char* (*)(const unsigned char*,size_t,unsigned char*);
    static Hash hash=[]() -> Hash {
        void* library=dlopen("libcrypto.so.3",RTLD_NOW|RTLD_LOCAL);
        if (!library) return nullptr;
        auto fn=reinterpret_cast<Hash>(dlsym(library,"SHA256"));
        if (!fn) dlclose(library);
        return fn; // Retain library for the process lifetime while fn is cached.
    }();
    Require(hash!=nullptr,"libcrypto.so.3 SHA256 unavailable");
    std::array<uint8_t,32> digest{};
    Require(hash(static_cast<const uint8_t*>(bytes),length,digest.data())==digest.data(),"SHA-256 failed");
    std::string out; out.reserve(64);
    for (auto b:digest) { out+="0123456789abcdef"[b>>4]; out+="0123456789abcdef"[b&15]; }
    return out;
}
static void Write(int fd,const void* data,size_t length) {
    const auto* p=static_cast<const char*>(data);
    while (length) {
        auto n=write(fd,p,length);
        if (n<0 && errno==EINTR) continue;
        Require(n>0,"diagnostic write failed"); p+=n; length-=size_t(n);
    }
}
struct Session {
    std::mutex mutex;
    std::filesystem::path directory;
    int fd=-1;
    bool stopped=false,shaderDumpFull=false,descriptorLogFull=false;
    uint64_t maxShaderBytes=MaxShaders,descriptorBytes=0;
    uint64_t bytes=0,shaderBytes=0,seq=0,nextId=0,presents=0;
    std::set<std::string> shaderFiles;
    Session() {
        // An explicitly smaller budget is useful for tests; increases stay bounded.
        if (const char* value=getenv("DLSSFG_SHADER_LIMIT_MIB")) {
            char* end=nullptr; errno=0; auto mib=strtoull(value,&end,10);
            if (*value && end && !*end && !errno && mib<=1024) maxShaderBytes=mib<<20;
        }
        const auto stamp=std::chrono::system_clock::now().time_since_epoch().count();
        directory=std::filesystem::path(getenv("DLSSFG_DISCOVERY_DIR"))/
            ("rdr2-"+std::to_string(getpid())+"-"+std::to_string(stamp));
        std::filesystem::create_directories(directory); chmod(directory.c_str(),0700);
        std::filesystem::create_directory(directory/"shaders"); chmod((directory/"shaders").c_str(),0700);
        fd=open((directory/"events.jsonl").c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);
        Require(fd>=0,"cannot create discovery log");
        try {
            Require(Sha256("abc",3)=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","SHA-256 self-test failed");
            Event({{"event","session"},{"schema",1},{"pid",getpid()},{"mode","metadata_only"},
                {"gpu_contents_captured",false},{"max_log_bytes",MaxLog},{"max_shader_bytes",maxShaderBytes},{"max_descriptor_log_bytes",MaxDescriptorLog},
                {"limitations",{"no_command_recording","no_gpu_completion_tracking","no_memory_contents", "no_descriptor_update_templates","no_renderpass2_or_dynamic_rendering","no_inline_shader_modules","no_shader_objects","partial_pipeline_failures_not_recorded"}}});
        } catch (...) { close(fd); fd=-1; throw; }
        fprintf(stderr,"[fg-discovery] metadata session: %s\n",directory.c_str());
    }
    ~Session() { if (fd>=0) close(fd); }
    void Event(Json j) {
        const auto event=j.value("event",std::string{});
        const bool descriptor=event=="descriptor_write" || event=="descriptor_copy";
        if (descriptor && descriptorLogFull) return;
        j["seq"]=seq+1; j["present_marker"]=presents;
        const std::string line=j.dump(-1,' ',false,Json::error_handler_t::replace)+"\n";
        if (descriptor && line.size()>MaxDescriptorLog-descriptorBytes) {
            descriptorLogFull=true;
            Event({{"event","descriptor_budget_exhausted"},{"bytes",descriptorBytes},
                {"effect","descriptor updates omitted; resource inventory continues"}});
            return;
        }
        Require(line.size()<=MaxLog-bytes,"metadata limit reached");
        Write(fd,line.data(),line.size()); bytes+=line.size(); ++seq;
        if (descriptor) descriptorBytes+=line.size();
    }
    void Stop(const char* reason) noexcept {
        if (stopped) return;
        stopped=true;
        fprintf(stderr,"[fg-discovery] disabled: %s\n",reason);
        // A separate short status file remains possible even after the log cap.
        try {
            Fd status(open((directory/"stopped.txt").c_str(),O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC,0600));
            if (status.fd>=0) Write(status.fd,reason,std::strlen(reason));
        } catch (...) {}
    }
};
struct Context {
    std::shared_ptr<Session> session;
    uint64_t id=0;
    std::unordered_map<std::string,PFN_vkVoidFunction> functions;
    std::map<std::pair<VkObjectType,uint64_t>,uint64_t> ids;
    std::map<uint64_t,std::string> shaders;
    std::map<uint64_t,uint64_t> setPools;
    void ForgetPool(uint64_t pool) {
        for (auto it=setPools.begin(); it!=setPools.end();) {
            if (it->second==pool) { ids.erase({VK_OBJECT_TYPE_DESCRIPTOR_SET,it->first}); it=setPools.erase(it); }
            else ++it;
        }
    }
    uint64_t Ref(VkObjectType kind,uint64_t handle) const {
        auto p=ids.find({kind,handle}); return p==ids.end()?0:p->second;
    }
    uint64_t New(VkObjectType kind,uint64_t handle) {
        Require(ids.size()<MaxObjects,"live object limit reached");
        return ids[{kind,handle}]=++session->nextId;
    }
    template<class T> T Fn(const char* name) { return reinterpret_cast<T>(functions.at(name)); }
};
static std::mutex registryMutex;
static std::unordered_map<VkDevice,std::shared_ptr<Context>> contexts;
static std::shared_ptr<Session> processSession;
static bool startupFailed=false;
// The owning layer provides dispatch independently of diagnostic allocation/I/O.
// Cached hooks must remain usable even if Register could not create a context.
static std::atomic<PFN_vkGetDeviceProcAddr> fallbackDispatch{nullptr};
template<class T> static T Next(VkDevice d,const std::shared_ptr<Context>& c,const char* name) {
    if (c) return c->Fn<T>(name);
    return reinterpret_cast<T>(fallbackDispatch.load()(d,name));
}
static std::shared_ptr<Context> Get(VkDevice device) {
    std::lock_guard<std::mutex> lock(registryMutex);
    auto found=contexts.find(device); return found==contexts.end()?nullptr:found->second;
}
template<class F> static void Record(const std::shared_ptr<Context>& c,F&& f) noexcept {
    if (!c || !c->session) return;
    auto& s=*c->session;
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.stopped) return;
    try { f(*c,s); } catch (const std::exception& e) { s.Stop(e.what()); } catch (...) { s.Stop("diagnostic exception"); }
}
static Json Ref(Context& c,VkObjectType type,uint64_t handle) {
    return {{"id",c.Ref(type,handle)},{"handle",handle}};
}
#define FUNCTIONS(X) \
 X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreateImage) X(vkDestroyImage) \
 X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateBuffer) X(vkDestroyBuffer) \
 X(vkCreateGraphicsPipelines) X(vkCreateComputePipelines) X(vkDestroyPipeline) \
 X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) \
 X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkResetDescriptorPool) \
 X(vkAllocateDescriptorSets) X(vkFreeDescriptorSets) X(vkUpdateDescriptorSets) \
 X(vkCreateRenderPass) X(vkDestroyRenderPass) X(vkCreateFramebuffer) X(vkDestroyFramebuffer)

void Register(VkDevice device,PFN_vkGetDeviceProcAddr next) noexcept {
    if (!Enabled()) return;
    std::lock_guard<std::mutex> lock(registryMutex);
    if (startupFailed) return;
    try {
        if (!processSession) processSession=std::make_shared<Session>();
        auto c=std::make_shared<Context>(); c->session=processSession;
#define LOAD(name) c->functions[#name]=next(device,#name); Require(c->functions[#name]!=nullptr,"missing core Vulkan dispatch");
        FUNCTIONS(LOAD)
#undef LOAD
        Record(c,[&](Context& ctx,Session& s) { ctx.id=++s.nextId; s.Event({{"event","device"},{"device",ctx.id}}); });
        contexts[device]=c;
    } catch (const std::exception& e) { startupFailed=true; fprintf(stderr,"[fg-discovery] unavailable: %s\n",e.what()); }
    catch (...) { startupFailed=true; }
}
void Remove(VkDevice device) noexcept {
    if (!Enabled()) return;
    try {
        auto c=Get(device);
        Record(c,[](Context& ctx,Session& s) { s.Event({{"event","device_destroy"},{"device",ctx.id}}); });
        std::lock_guard<std::mutex> lock(registryMutex); contexts.erase(device);
    } catch (...) {}
}
void Present(VkDevice device,const VkPresentInfoKHR* info) noexcept {
    if (!Enabled()) return;
    auto c=Get(device);
    Record(c,[&](Context& ctx,Session& s) {
        ++s.presents;
        // A CPU marker only, never evidence that resource writes completed.
        if (s.presents<=4 || s.presents%120==0) s.Event({{"event","present_marker"},{"device",ctx.id},{"swapchains",info->swapchainCount}});
    });
}
static VkResult VKAPI_CALL CreateShaderModule(VkDevice d,const VkShaderModuleCreateInfo* ci,const VkAllocationCallbacks* a,VkShaderModule* out) {
    auto c=Get(d);
    auto result=Next<PFN_vkCreateShaderModule>(d,c,"vkCreateShaderModule")(d,ci,a,out);
    if (result==VK_SUCCESS) Record(c,[&](Context& ctx,Session& s) {
        Require(ci->codeSize<=16*1024*1024 && ci->codeSize%4==0,"unsupported shader size");
        auto hash=Sha256(ci->pCode,ci->codeSize);
        bool saved=s.shaderFiles.count(hash)!=0;
        if (!saved && !s.shaderDumpFull) {
            if (ci->codeSize>s.maxShaderBytes-s.shaderBytes) {
                s.shaderDumpFull=true;
                s.Event({{"event","shader_budget_exhausted"},{"bytes",s.shaderBytes},
                    {"effect","new shader binaries omitted; hashes and resource inventory continue"}});
            } else {
                Fd f(open((s.directory/"shaders"/(hash+".spv")).c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600));
                Require(f.fd>=0,"cannot write shader"); Write(f.fd,ci->pCode,ci->codeSize);
                s.shaderBytes+=ci->codeSize; s.shaderFiles.insert(hash); saved=true;
            }
        }
        auto id=ctx.New(VK_OBJECT_TYPE_SHADER_MODULE,Handle(*out)); ctx.shaders[Handle(*out)]=hash;
        s.Event({{"event","shader"},{"device",ctx.id},{"id",id},{"sha256",hash},{"bytes",ci->codeSize},{"binary_saved",saved}});
    });
    return result;
}
#define DESTROY(Name,Type,Kind) \
static void VKAPI_CALL Destroy##Name(VkDevice d,Type object,const VkAllocationCallbacks* a) { \
 auto c=Get(d); \
 Record(c,[&](Context& ctx,Session& s) { s.Event({{"event","destroy"},{"device",ctx.id},{"kind",Kind},{"object",Ref(ctx,Kind,Handle(object))}}); ctx.ids.erase({Kind,Handle(object)}); if (Kind==VK_OBJECT_TYPE_SHADER_MODULE) ctx.shaders.erase(Handle(object)); }); \
 Next<PFN_vkDestroy##Name>(d,c,"vkDestroy" #Name)(d,object,a); }
DESTROY(ShaderModule,VkShaderModule,VK_OBJECT_TYPE_SHADER_MODULE)
DESTROY(Image,VkImage,VK_OBJECT_TYPE_IMAGE)
DESTROY(ImageView,VkImageView,VK_OBJECT_TYPE_IMAGE_VIEW)
DESTROY(Buffer,VkBuffer,VK_OBJECT_TYPE_BUFFER)
DESTROY(Pipeline,VkPipeline,VK_OBJECT_TYPE_PIPELINE)
DESTROY(DescriptorSetLayout,VkDescriptorSetLayout,VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT)
DESTROY(PipelineLayout,VkPipelineLayout,VK_OBJECT_TYPE_PIPELINE_LAYOUT)
DESTROY(RenderPass,VkRenderPass,VK_OBJECT_TYPE_RENDER_PASS)
DESTROY(Framebuffer,VkFramebuffer,VK_OBJECT_TYPE_FRAMEBUFFER)
#undef DESTROY
#define CREATE_START(Name,Info,Type) \
static VkResult VKAPI_CALL Create##Name(VkDevice d,const Info* ci,const VkAllocationCallbacks* a,Type* out) { \
 auto c=Get(d); \
 auto result=Next<PFN_vkCreate##Name>(d,c,"vkCreate" #Name)(d,ci,a,out); \
 if (result==VK_SUCCESS) Record(c,[&](Context& ctx,Session& s) {
#define CREATE_END }); return result; }
CREATE_START(Image,VkImageCreateInfo,VkImage)
    auto id=ctx.New(VK_OBJECT_TYPE_IMAGE,Handle(*out));
    s.Event({{"event","image"},{"device",ctx.id},{"id",id},{"format",ci->format},{"type",ci->imageType},
        {"extent",{ci->extent.width,ci->extent.height,ci->extent.depth}},{"mips",ci->mipLevels},{"layers",ci->arrayLayers},
        {"samples",ci->samples},{"usage",ci->usage},{"flags",ci->flags},{"tiling",ci->tiling}});
CREATE_END
CREATE_START(ImageView,VkImageViewCreateInfo,VkImageView)
    auto id=ctx.New(VK_OBJECT_TYPE_IMAGE_VIEW,Handle(*out)); const auto& r=ci->subresourceRange;
    s.Event({{"event","image_view"},{"device",ctx.id},{"id",id},{"image",Ref(ctx,VK_OBJECT_TYPE_IMAGE,Handle(ci->image))},
        {"format",ci->format},{"view_type",ci->viewType},{"aspect",r.aspectMask},{"base_mip",r.baseMipLevel},
        {"mips",r.levelCount},{"base_layer",r.baseArrayLayer},{"layers",r.layerCount}});
CREATE_END
CREATE_START(Buffer,VkBufferCreateInfo,VkBuffer)
    s.Event({{"event","buffer"},{"device",ctx.id},{"id",ctx.New(VK_OBJECT_TYPE_BUFFER,Handle(*out))},{"bytes",ci->size},{"usage",ci->usage},{"flags",ci->flags}});
CREATE_END
CREATE_START(DescriptorSetLayout,VkDescriptorSetLayoutCreateInfo,VkDescriptorSetLayout)
    Json bindings=Json::array();
    for (uint32_t i=0;i<ci->bindingCount;++i) { const auto& b=ci->pBindings[i]; bindings.push_back({{"binding",b.binding},{"type",b.descriptorType},{"count",b.descriptorCount},{"stages",b.stageFlags}}); }
    s.Event({{"event","set_layout"},{"device",ctx.id},{"id",ctx.New(VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT,Handle(*out))},{"flags",ci->flags},{"bindings",bindings}});
CREATE_END
CREATE_START(PipelineLayout,VkPipelineLayoutCreateInfo,VkPipelineLayout)
    Json sets=Json::array(),push=Json::array();
    for (uint32_t i=0;i<ci->setLayoutCount;++i) sets.push_back(Ref(ctx,VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT,Handle(ci->pSetLayouts[i])));
    for (uint32_t i=0;i<ci->pushConstantRangeCount;++i) { const auto& p=ci->pPushConstantRanges[i]; push.push_back({{"offset",p.offset},{"bytes",p.size},{"stages",p.stageFlags}}); }
    s.Event({{"event","pipeline_layout"},{"device",ctx.id},{"id",ctx.New(VK_OBJECT_TYPE_PIPELINE_LAYOUT,Handle(*out))},{"sets",sets},{"push_constants",push}});
CREATE_END
CREATE_START(RenderPass,VkRenderPassCreateInfo,VkRenderPass)
    Json attachments=Json::array(),subpasses=Json::array();
    for (uint32_t i=0;i<ci->attachmentCount;++i) { const auto& x=ci->pAttachments[i]; attachments.push_back({{"format",x.format},{"samples",x.samples},{"load",x.loadOp},{"store",x.storeOp},{"initial_layout",x.initialLayout},{"final_layout",x.finalLayout}}); }
    for (uint32_t i=0;i<ci->subpassCount;++i) { const auto& p=ci->pSubpasses[i]; Json colors=Json::array(); for(uint32_t j=0;j<p.colorAttachmentCount;++j) colors.push_back(p.pColorAttachments[j].attachment); subpasses.push_back({{"colors",colors},{"depth",p.pDepthStencilAttachment?p.pDepthStencilAttachment->attachment:VK_ATTACHMENT_UNUSED}}); }
    s.Event({{"event","render_pass"},{"device",ctx.id},{"id",ctx.New(VK_OBJECT_TYPE_RENDER_PASS,Handle(*out))},{"attachments",attachments},{"subpasses",subpasses}});
CREATE_END
CREATE_START(Framebuffer,VkFramebufferCreateInfo,VkFramebuffer)
    Json views=Json::array();
    if (!(ci->flags&VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT)) for(uint32_t i=0;i<ci->attachmentCount;++i) views.push_back(Ref(ctx,VK_OBJECT_TYPE_IMAGE_VIEW,Handle(ci->pAttachments[i])));
    s.Event({{"event","framebuffer"},{"device",ctx.id},{"id",ctx.New(VK_OBJECT_TYPE_FRAMEBUFFER,Handle(*out))},
        {"render_pass",Ref(ctx,VK_OBJECT_TYPE_RENDER_PASS,Handle(ci->renderPass))},{"views",views},{"flags",ci->flags},{"width",ci->width},{"height",ci->height},{"layers",ci->layers}});
CREATE_END
#undef CREATE_START
#undef CREATE_END
static Json Stage(Context& ctx,const VkPipelineShaderStageCreateInfo& x) {
    auto hash=ctx.shaders.find(Handle(x.module));
    return {{"stage",x.stage},{"shader",Ref(ctx,VK_OBJECT_TYPE_SHADER_MODULE,Handle(x.module))},
        {"sha256",hash==ctx.shaders.end()?"":hash->second},{"entry",x.pName?x.pName:""},
        {"specialized",x.pSpecializationInfo!=nullptr}};
}
static VkResult VKAPI_CALL CreateGraphicsPipelines(VkDevice d,VkPipelineCache cache,uint32_t count,const VkGraphicsPipelineCreateInfo* ci,const VkAllocationCallbacks* a,VkPipeline* out) {
    auto c=Get(d);
    auto r=Next<PFN_vkCreateGraphicsPipelines>(d,c,"vkCreateGraphicsPipelines")(d,cache,count,ci,a,out);
    if (r==VK_SUCCESS) Record(c,[&](Context& ctx,Session& s) { Require(count<=4096,"pipeline batch too large"); for(uint32_t i=0;i<count;++i) if(out[i]) {
        Json stages=Json::array(); for(uint32_t j=0;j<ci[i].stageCount;++j) stages.push_back(Stage(ctx,ci[i].pStages[j]));
        s.Event({{"event","graphics_pipeline"},{"device",ctx.id},{"id",ctx.New(VK_OBJECT_TYPE_PIPELINE,Handle(out[i]))},{"stages",stages},
            {"layout",Ref(ctx,VK_OBJECT_TYPE_PIPELINE_LAYOUT,Handle(ci[i].layout))},{"render_pass",Ref(ctx,VK_OBJECT_TYPE_RENDER_PASS,Handle(ci[i].renderPass))},{"subpass",ci[i].subpass}});
    }});
    return r;
}
static VkResult VKAPI_CALL CreateComputePipelines(VkDevice d,VkPipelineCache cache,uint32_t count,const VkComputePipelineCreateInfo* ci,const VkAllocationCallbacks* a,VkPipeline* out) {
    auto c=Get(d);
    auto r=Next<PFN_vkCreateComputePipelines>(d,c,"vkCreateComputePipelines")(d,cache,count,ci,a,out);
    if (r==VK_SUCCESS) Record(c,[&](Context& ctx,Session& s) { Require(count<=4096,"pipeline batch too large"); for(uint32_t i=0;i<count;++i) if(out[i]) s.Event({{"event","compute_pipeline"},{"device",ctx.id},
        {"id",ctx.New(VK_OBJECT_TYPE_PIPELINE,Handle(out[i]))},{"stage",Stage(ctx,ci[i].stage)},{"layout",Ref(ctx,VK_OBJECT_TYPE_PIPELINE_LAYOUT,Handle(ci[i].layout))}}); });
    return r;
}
static VkResult VKAPI_CALL CreateDescriptorPool(VkDevice d,const VkDescriptorPoolCreateInfo* ci,const VkAllocationCallbacks* a,VkDescriptorPool* out) {
    auto c=Get(d);
    auto r=Next<PFN_vkCreateDescriptorPool>(d,c,"vkCreateDescriptorPool")(d,ci,a,out);
    if (r==VK_SUCCESS) Record(c,[&](Context& ctx,Session& s) { s.Event({{"event","descriptor_pool"},{"device",ctx.id},{"id",ctx.New(VK_OBJECT_TYPE_DESCRIPTOR_POOL,Handle(*out))}}); });
    return r;
}
static VkResult VKAPI_CALL AllocateDescriptorSets(VkDevice d,const VkDescriptorSetAllocateInfo* ci,VkDescriptorSet* out) {
    auto c=Get(d);
    auto r=Next<PFN_vkAllocateDescriptorSets>(d,c,"vkAllocateDescriptorSets")(d,ci,out);
    if (r==VK_SUCCESS) Record(c,[&](Context& ctx,Session& s) { Require(ci->descriptorSetCount<=65536,"descriptor allocation too large"); for(uint32_t i=0;i<ci->descriptorSetCount;++i) { ctx.setPools[Handle(out[i])]=Handle(ci->descriptorPool); s.Event({{"event","descriptor_set"},{"device",ctx.id},
        {"id",ctx.New(VK_OBJECT_TYPE_DESCRIPTOR_SET,Handle(out[i]))},{"pool",Ref(ctx,VK_OBJECT_TYPE_DESCRIPTOR_POOL,Handle(ci->descriptorPool))},
        {"layout",Ref(ctx,VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT,Handle(ci->pSetLayouts[i]))}}); } });
    return r;
}
static VkResult VKAPI_CALL FreeDescriptorSets(VkDevice d,VkDescriptorPool pool,uint32_t count,const VkDescriptorSet* sets) {
    auto c=Get(d);
    auto r=Next<PFN_vkFreeDescriptorSets>(d,c,"vkFreeDescriptorSets")(d,pool,count,sets);
    if (r==VK_SUCCESS) Record(c,[&](Context& ctx,Session& s) { for(uint32_t i=0;i<count;++i) { s.Event({{"event","destroy"},{"device",ctx.id},{"kind",VK_OBJECT_TYPE_DESCRIPTOR_SET},{"object",Ref(ctx,VK_OBJECT_TYPE_DESCRIPTOR_SET,Handle(sets[i]))}}); ctx.ids.erase({VK_OBJECT_TYPE_DESCRIPTOR_SET,Handle(sets[i])}); ctx.setPools.erase(Handle(sets[i])); } });
    return r;
}
static VkResult VKAPI_CALL ResetDescriptorPool(VkDevice d,VkDescriptorPool pool,VkDescriptorPoolResetFlags flags) {
    auto c=Get(d);
    auto r=Next<PFN_vkResetDescriptorPool>(d,c,"vkResetDescriptorPool")(d,pool,flags);
    if (r==VK_SUCCESS) Record(c,[&](Context& ctx,Session& s) { ctx.ForgetPool(Handle(pool)); s.Event({{"event","descriptor_pool_reset"},{"device",ctx.id},{"pool",Ref(ctx,VK_OBJECT_TYPE_DESCRIPTOR_POOL,Handle(pool))}}); });
    return r;
}
static void VKAPI_CALL DestroyDescriptorPool(VkDevice d,VkDescriptorPool pool,const VkAllocationCallbacks* a) {
    auto c=Get(d);
    Record(c,[&](Context& ctx,Session& s) { ctx.ForgetPool(Handle(pool)); s.Event({{"event","descriptor_pool_destroy"},{"device",ctx.id},{"pool",Ref(ctx,VK_OBJECT_TYPE_DESCRIPTOR_POOL,Handle(pool))}}); ctx.ids.erase({VK_OBJECT_TYPE_DESCRIPTOR_POOL,Handle(pool)}); });
    Next<PFN_vkDestroyDescriptorPool>(d,c,"vkDestroyDescriptorPool")(d,pool,a);
}
static void VKAPI_CALL UpdateDescriptorSets(VkDevice d,uint32_t count,const VkWriteDescriptorSet* writes,uint32_t copyCount,const VkCopyDescriptorSet* copies) {
    auto c=Get(d);
    Next<PFN_vkUpdateDescriptorSets>(d,c,"vkUpdateDescriptorSets")(d,count,writes,copyCount,copies);
    Record(c,[&](Context& ctx,Session& s) {
        if (s.descriptorLogFull) return;
        Require(count<=4096 && copyCount<=4096,"descriptor batch too large");
        uint64_t elements=0;
        for(uint32_t i=0;i<count;++i) {
            elements+=writes[i].descriptorCount; Require(elements<=65536,"descriptor element limit reached");
            const auto& w=writes[i]; Json resources=Json::array();
            for(uint32_t j=0;j<w.descriptorCount;++j) {
                switch(w.descriptorType) {
                case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE: case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
                    resources.push_back({{"view",Ref(ctx,VK_OBJECT_TYPE_IMAGE_VIEW,Handle(w.pImageInfo[j].imageView))},{"layout",w.pImageInfo[j].imageLayout}}); break;
                case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC: case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
                    resources.push_back({{"buffer",Ref(ctx,VK_OBJECT_TYPE_BUFFER,Handle(w.pBufferInfo[j].buffer))},{"offset",w.pBufferInfo[j].offset},{"range",w.pBufferInfo[j].range}}); break;
                default: break; // Never interpret inactive union members.
                }
            }
            s.Event({{"event","descriptor_write"},{"device",ctx.id},{"set",Ref(ctx,VK_OBJECT_TYPE_DESCRIPTOR_SET,Handle(w.dstSet))},{"binding",w.dstBinding},{"element",w.dstArrayElement},{"count",w.descriptorCount},{"type",w.descriptorType},{"resources",resources}});
        }
        for(uint32_t i=0;i<copyCount;++i) { const auto& c=copies[i]; s.Event({{"event","descriptor_copy"},{"device",ctx.id},{"source",Ref(ctx,VK_OBJECT_TYPE_DESCRIPTOR_SET,Handle(c.srcSet))},
            {"src_binding",c.srcBinding},{"src_element",c.srcArrayElement},{"target",Ref(ctx,VK_OBJECT_TYPE_DESCRIPTOR_SET,Handle(c.dstSet))},{"dst_binding",c.dstBinding},{"dst_element",c.dstArrayElement},{"count",c.descriptorCount}}); }
    });
}
PFN_vkVoidFunction Lookup(const char* name,PFN_vkGetDeviceProcAddr fallback) noexcept {
    if (!Enabled() || !name) return nullptr;
    fallbackDispatch.store(fallback);
    // Names of the C++ wrappers omit the Vulkan prefix.
#define HOOK(name) if (!std::strcmp(nameStr,"vk" #name)) return reinterpret_cast<PFN_vkVoidFunction>(name);
    const char* nameStr=name;
    HOOK(CreateShaderModule) HOOK(DestroyShaderModule) HOOK(CreateImage) HOOK(DestroyImage)
    HOOK(CreateImageView) HOOK(DestroyImageView) HOOK(CreateBuffer) HOOK(DestroyBuffer)
    HOOK(CreateGraphicsPipelines) HOOK(CreateComputePipelines) HOOK(DestroyPipeline)
    HOOK(CreateDescriptorSetLayout) HOOK(DestroyDescriptorSetLayout) HOOK(CreatePipelineLayout) HOOK(DestroyPipelineLayout)
    HOOK(CreateDescriptorPool) HOOK(DestroyDescriptorPool) HOOK(ResetDescriptorPool)
    HOOK(AllocateDescriptorSets) HOOK(FreeDescriptorSets) HOOK(UpdateDescriptorSets)
    HOOK(CreateRenderPass) HOOK(DestroyRenderPass) HOOK(CreateFramebuffer) HOOK(DestroyFramebuffer)
#undef HOOK
    return nullptr;
}
}
