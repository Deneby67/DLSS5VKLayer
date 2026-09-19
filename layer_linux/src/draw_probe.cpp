#include "draw_probe.h"
#include <set>
#include <vector>
#include <tuple>
#include <stdexcept>
#include <type_traits>
#include <array>
namespace dlssfg {
using Json=nlohmann::json;
template<class T> static uint64_t h(T v) {if constexpr(std::is_pointer_v<T>)return reinterpret_cast<uintptr_t>(v);else return uint64_t(v);}
struct DrawProbe::Impl {
    struct Ref {uint64_t handle=0,gen=0;};
    struct ImageBinding {Ref view;VkImageLayout layout;VkDescriptorType type;};
    struct Set {uint64_t gen,pool,revision=0;std::map<uint32_t,ImageBinding> images;};
    struct Image {uint64_t gen;VkFormat format;VkExtent3D extent;VkImageUsageFlags usage;};
    struct View {uint64_t gen;Ref image;VkFormat format;VkImageSubresourceRange range;};
    struct Subpass {std::vector<VkAttachmentReference> colors;VkAttachmentReference depth{VK_ATTACHMENT_UNUSED,VK_IMAGE_LAYOUT_UNDEFINED};};
    struct RenderPass {uint64_t gen;std::vector<VkAttachmentDescription> attachments;std::vector<Subpass> subpasses;};
    struct Framebuffer {uint64_t gen;bool imageless;std::vector<Ref> views;};
    struct Target {Ref view;std::string role;uint32_t index;VkImageLayout layout;VkAttachmentLoadOp load;VkAttachmentStoreOp store;};
    struct Pass {
        uint64_t id=0;Ref renderPass,framebuffer;uint32_t subpass=0;VkRect2D area{};
        std::vector<Ref> views;std::vector<Target> targets;
        std::string status="unresolved";bool dynamic=false;
    };
    struct Pipeline {uint64_t gen,layout;Json stages;};
    struct Draw {Ref pipeline,set;uint64_t revision,first,last,count;std::string kind;bool indirect;std::shared_ptr<Pass> targets;};
    using Key=std::tuple<uint64_t,uint64_t,uint64_t,std::string,uint64_t>;
    struct Child {uint64_t gen;std::shared_ptr<Pass> pass;};
    struct Command {
        uint64_t gen=0,pool=0,ordinal=0,layout=0;
        Ref pipeline,set;
        Ref inheritedRenderPass,inheritedFramebuffer;uint32_t inheritedSubpass=0;
        std::shared_ptr<Pass> pass;
        bool truncated=false;
        std::map<Key,Draw> draws;
        std::map<uint64_t,Child> children;
    };
    std::map<uint64_t,std::string> shaders;
    std::map<uint64_t,uint64_t> layouts;
    std::map<uint64_t,Pipeline> pipelines;
    std::map<uint64_t,Set> sets;
    std::map<uint64_t,Image> images;
    std::map<uint64_t,View> views;
    std::map<uint64_t,RenderPass> renderPasses;
    std::map<uint64_t,Framebuffer> framebuffers;
    std::map<uint64_t,Command> commands;
    uint64_t next=0,totalDraws=0;
    std::array<uint64_t,7> observations{};
    std::string disabled;
    uint64_t gen() {
        if(shaders.size()+layouts.size()+pipelines.size()+sets.size()+commands.size()+images.size()+views.size()+renderPasses.size()+framebuffers.size()>500000)
            throw std::runtime_error("draw association object limit");
        return ++next;
    }
    void clear(Command& c) {totalDraws-=c.draws.size();auto pool=c.pool;c=Command{};c.pool=pool;c.gen=gen();}
    Ref viewRef(VkImageView handle){auto v=views.find(h(handle));return v==views.end()?Ref{}:Ref{h(handle),v->second.gen};}
    void fill(Pass& pass) {
        pass.targets.clear();auto rp=renderPasses.find(pass.renderPass.handle);
        if(rp==renderPasses.end() || rp->second.gen!=pass.renderPass.gen){pass.status="untracked render pass";return;}
        if(pass.subpass>=rp->second.subpasses.size()){pass.status="untracked subpass";return;}
        auto add=[&](VkAttachmentReference ref,const char* role,uint32_t index){
            if(ref.attachment==VK_ATTACHMENT_UNUSED)return;
            if(ref.attachment>=pass.views.size() || ref.attachment>=rp->second.attachments.size()){pass.status="unresolved attachment";return;}
            const auto& a=rp->second.attachments[ref.attachment];
            pass.targets.push_back({pass.views[ref.attachment],role,index,ref.layout,a.loadOp,a.storeOp});
        };
        pass.status="tracked attachment references";
        const auto& sub=rp->second.subpasses[pass.subpass];
        for(uint32_t i=0;i<sub.colors.size();++i)add(sub.colors[i],"color",i);
        add(sub.depth,"depth_stencil",0);
    }
    Json targets(const std::shared_ptr<Pass>& pass) {
        if(!pass)return {{"status","no resolved render scope"}};
        Json result={{"status",pass->status},{"dynamic_rendering",pass->dynamic},
            {"render_pass_generation",pass->renderPass.gen},{"framebuffer_generation",pass->framebuffer.gen},
            {"subpass",pass->subpass},{"render_area",{pass->area.offset.x,pass->area.offset.y,pass->area.extent.width,pass->area.extent.height}},
            {"gpu_contents_captured",false},{"attachments",Json::array()}};
        if(!pass->dynamic){
            auto rp=renderPasses.find(pass->renderPass.handle);auto fb=framebuffers.find(pass->framebuffer.handle);
            if(rp==renderPasses.end() || rp->second.gen!=pass->renderPass.gen || fb==framebuffers.end() || fb->second.gen!=pass->framebuffer.gen){result["status"]="stale render scope";return result;}
        }
        for(const auto& t:pass->targets){
            Json entry={{"role",t.role},{"attachment_slot",t.index},{"declared_layout",t.layout},{"load",t.load},{"store",t.store},{"valid",false}};
            auto v=views.find(t.view.handle);
            if(v!=views.end() && v->second.gen==t.view.gen){
                auto im=images.find(v->second.image.handle);
                if(im!=images.end() && im->second.gen==v->second.image.gen){
                    entry.update({{"valid",true},{"image_generation",im->second.gen},{"view_generation",v->second.gen},
                        {"view_format",v->second.format},{"image_format",im->second.format},
                        {"extent",{im->second.extent.width,im->second.extent.height,im->second.extent.depth}},
                        {"usage",im->second.usage},{"aspect",v->second.range.aspectMask},
                        {"base_mip",v->second.range.baseMipLevel},{"mips",v->second.range.levelCount},
                        {"base_layer",v->second.range.baseArrayLayer},{"layers",v->second.range.layerCount}});
                }
            }
            result["attachments"].push_back(std::move(entry));
        }
        return result;
    }
    void collect(uint64_t cb,uint64_t generation,std::set<uint64_t>& visited,std::map<uint64_t,Json>& result,const std::shared_ptr<Pass>& inherited={}) {
        if(!visited.insert(cb).second)return;
        if(visited.size()>256)throw std::runtime_error("draw secondary traversal limit");
        auto it=commands.find(cb);if(it==commands.end() || (generation && it->second.gen!=generation))return;
        const auto& c=it->second;
        for(const auto& item:c.draws) {
            const auto& d=item.second;auto s=sets.find(d.set.handle);auto pipe=pipelines.find(d.pipeline.handle);
            if(s==sets.end() || s->second.gen!=d.set.gen || s->second.revision!=d.revision ||
               pipe==pipelines.end() || pipe->second.gen!=d.pipeline.gen)continue;
            auto& links=result[d.set.handle];if(links.is_null())links=Json::array();
            // Bounded examples, never an exhaustive draw count or ordered GPU trace.
            if(links.size()>=4)continue;
            auto target=d.targets;
            if(!target && inherited && !inherited->dynamic && c.inheritedRenderPass.handle==inherited->renderPass.handle &&
               c.inheritedRenderPass.gen==inherited->renderPass.gen && c.inheritedSubpass==inherited->subpass &&
               (!c.inheritedFramebuffer.handle || (c.inheritedFramebuffer.handle==inherited->framebuffer.handle && c.inheritedFramebuffer.gen==inherited->framebuffer.gen)))target=inherited;
            Json imageBindings=Json::array();
            for(const auto& binding:s->second.images){
                const auto& ref=binding.second;auto view=views.find(ref.view.handle);
                if(view==views.end() || view->second.gen!=ref.view.gen)continue;
                auto image=images.find(view->second.image.handle);
                if(image==images.end() || image->second.gen!=view->second.image.gen)continue;
                const auto& v=view->second;const auto& im=image->second;
                imageBindings.push_back({{"binding",binding.first},{"element",0},{"type",ref.type},
                    {"descriptor_layout",ref.layout},{"image_generation",im.gen},{"view_generation",v.gen},
                    {"image_format",im.format},{"view_format",v.format},{"extent",{im.extent.width,im.extent.height,im.extent.depth}},
                    {"usage",im.usage},{"aspect",v.range.aspectMask},{"base_mip",v.range.baseMipLevel},
                    {"mips",v.range.levelCount},{"base_layer",v.range.baseArrayLayer},{"layers",v.range.layerCount}});
            }
            links.push_back({{"command_buffer",cb},{"command_generation",c.gen},
                {"pipeline_generation",pipe->second.gen},{"pipeline_layout_generation",pipe->second.layout},
                {"stages",pipe->second.stages},{"kind",d.kind},{"indirect_count_unknown",d.indirect},
                {"first_draw_ordinal",d.first},{"last_draw_ordinal",d.last},{"recorded_calls",d.count},
                {"set_generation",d.set.gen},{"descriptor_revision",d.revision},{"set_index",0},
                {"set0_image_bindings",imageBindings},{"image_binding_scope","at most 16 single-element writes; no resource contents"},
                {"render_targets",targets(target)},
                {"command_examples_truncated",c.truncated},{"gpu_execution_verified",false},
                {"descriptor_binding_used_by_shader_verified",false}});
            if(result.size()>8192)throw std::runtime_error("draw submitted set limit");
        }
        for(auto child:c.children)collect(child.first,child.second.gen,visited,result,child.second.pass);
    }
};
DrawProbe::DrawProbe():p(new Impl){}
DrawProbe::~DrawProbe()=default;
void DrawProbe::disable(const std::string& reason){p->disabled=reason;}
std::string DrawProbe::status()const{return p->disabled.empty()?"legacy graphics draw associations":p->disabled;}
Json DrawProbe::diagnostics()const{return {{"scope","CPU draw calls since device registration"},
    {"observed",p->observations[0]},{"empty_direct_or_zero_max_count",p->observations[1]},
    {"unknown_pipeline",p->observations[2]},{"unknown_set",p->observations[3]},
    {"layout_identity_mismatch",p->observations[4]},{"associated_at_recording",p->observations[5]},
    {"new_example_limit",p->observations[6]},{"status",status()}};}
void DrawProbe::shader(VkShaderModule s,const std::string& hash){if(!p->disabled.empty())return;p->gen();p->shaders[h(s)]=hash;}
void DrawProbe::layout(VkPipelineLayout l){if(!p->disabled.empty())return;p->layouts[h(l)]=p->gen();}
void DrawProbe::pipeline(VkPipeline pipeline,const VkGraphicsPipelineCreateInfo& info) {
    if(!p->disabled.empty())return;
    auto layout=p->layouts.find(h(info.layout));p->pipelines.erase(h(pipeline));
    if(layout==p->layouts.end() || !info.stageCount || info.stageCount>16)return;
    Json stages=Json::array();bool vertex=false;
    for(uint32_t i=0;i<info.stageCount;++i){const auto& s=info.pStages[i];auto hash=p->shaders.find(h(s.module));
        if(hash==p->shaders.end())return;
        stages.push_back({{"stage",s.stage},{"sha256",hash->second},{"entry",s.pName?s.pName:""},
                          {"specialized",s.pSpecializationInfo!=nullptr}});
        vertex|=s.stage==VK_SHADER_STAGE_VERTEX_BIT;
    }
    if(vertex)p->pipelines[h(pipeline)]={p->gen(),layout->second,std::move(stages)};
}
void DrawProbe::destroy(VkObjectType type,uint64_t object) {
    if(type==VK_OBJECT_TYPE_SHADER_MODULE)p->shaders.erase(object);
    if(type==VK_OBJECT_TYPE_PIPELINE_LAYOUT)p->layouts.erase(object);
    if(type==VK_OBJECT_TYPE_PIPELINE)p->pipelines.erase(object);
    if(type==VK_OBJECT_TYPE_IMAGE)p->images.erase(object);
    if(type==VK_OBJECT_TYPE_IMAGE_VIEW)p->views.erase(object);
    if(type==VK_OBJECT_TYPE_RENDER_PASS)p->renderPasses.erase(object);
    if(type==VK_OBJECT_TYPE_FRAMEBUFFER)p->framebuffers.erase(object);
}
void DrawProbe::image(VkImage image,const VkImageCreateInfo& info){if(p->disabled.empty())p->images[h(image)]={p->gen(),info.format,info.extent,info.usage};}
void DrawProbe::view(VkImageView view,const VkImageViewCreateInfo& info){
    if(!p->disabled.empty())return;auto im=p->images.find(h(info.image));p->views.erase(h(view));if(im==p->images.end())return;
    p->views[h(view)]={p->gen(),{h(info.image),im->second.gen},info.format,info.subresourceRange};
}
void DrawProbe::descriptor(const VkWriteDescriptorSet& w){
    auto it=p->sets.find(h(w.dstSet));if(it==p->sets.end())return;auto& s=it->second;++s.revision;
    if(w.descriptorCount!=1){s.images.clear();return;} // Do not interpret array or spill writes.
    s.images.erase(w.dstBinding);
    if(w.dstArrayElement || (w.descriptorType!=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE && w.descriptorType!=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE &&
       w.descriptorType!=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER && w.descriptorType!=VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT))return;
    auto view=p->views.find(h(w.pImageInfo[0].imageView));if(view==p->views.end())return;
    if(s.images.size()>=16)return;
    s.images[w.dstBinding]={{view->first,view->second.gen},w.pImageInfo[0].imageLayout,w.descriptorType};
}
void DrawProbe::renderPass(VkRenderPass handle,const VkRenderPassCreateInfo& info){
    if(!p->disabled.empty())return;
    if(info.attachmentCount>32 || info.subpassCount>32)throw std::runtime_error("render pass tracking limit");
    Impl::RenderPass rp{};rp.gen=p->gen();
    for(uint32_t i=0;i<info.attachmentCount;++i)rp.attachments.push_back(info.pAttachments[i]);
    for(uint32_t i=0;i<info.subpassCount;++i){const auto& s=info.pSubpasses[i];Impl::Subpass sub;
        if(s.colorAttachmentCount>16)throw std::runtime_error("color attachment tracking limit");
        for(uint32_t j=0;j<s.colorAttachmentCount;++j)sub.colors.push_back(s.pColorAttachments[j]);
        if(s.pDepthStencilAttachment)sub.depth=*s.pDepthStencilAttachment;
        rp.subpasses.push_back(std::move(sub));
    }
    p->renderPasses[h(handle)]=std::move(rp);
}
void DrawProbe::renderPass2(VkRenderPass handle,const VkRenderPassCreateInfo2& info){
    if(!p->disabled.empty())return;
    if(info.attachmentCount>32 || info.subpassCount>32)throw std::runtime_error("render pass2 tracking limit");
    Impl::RenderPass rp{};rp.gen=p->gen();
    for(uint32_t i=0;i<info.attachmentCount;++i){const auto& a=info.pAttachments[i];rp.attachments.push_back({a.flags,a.format,a.samples,a.loadOp,a.storeOp,a.stencilLoadOp,a.stencilStoreOp,a.initialLayout,a.finalLayout});}
    for(uint32_t i=0;i<info.subpassCount;++i){const auto& s=info.pSubpasses[i];Impl::Subpass sub;
        if(s.colorAttachmentCount>16)throw std::runtime_error("color attachment tracking limit");
        for(uint32_t j=0;j<s.colorAttachmentCount;++j)sub.colors.push_back({s.pColorAttachments[j].attachment,s.pColorAttachments[j].layout});
        if(s.pDepthStencilAttachment)sub.depth={s.pDepthStencilAttachment->attachment,s.pDepthStencilAttachment->layout};
        rp.subpasses.push_back(std::move(sub));
    }
    p->renderPasses[h(handle)]=std::move(rp);
}
void DrawProbe::framebuffer(VkFramebuffer handle,const VkFramebufferCreateInfo& info){
    if(!p->disabled.empty())return;
    if(info.attachmentCount>32)throw std::runtime_error("framebuffer tracking limit");
    Impl::Framebuffer fb{p->gen(),bool(info.flags&VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT),{}};
    if(!fb.imageless)for(uint32_t i=0;i<info.attachmentCount;++i)fb.views.push_back(p->viewRef(info.pAttachments[i]));
    p->framebuffers[h(handle)]=std::move(fb);
}
void DrawProbe::inheritance(VkCommandBuffer cb,const VkCommandBufferBeginInfo& info){
    auto it=p->commands.find(h(cb));if(it==p->commands.end() || !(info.flags&VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT) || !info.pInheritanceInfo)return;
    const auto& i=*info.pInheritanceInfo;auto& c=it->second;
    auto rp=p->renderPasses.find(h(i.renderPass));auto fb=p->framebuffers.find(h(i.framebuffer));
    c.inheritedRenderPass={h(i.renderPass),rp==p->renderPasses.end()?0:rp->second.gen};
    c.inheritedFramebuffer={h(i.framebuffer),fb==p->framebuffers.end()?0:fb->second.gen};c.inheritedSubpass=i.subpass;
}
void DrawProbe::beginPass(VkCommandBuffer cb,const VkRenderPassBeginInfo& info){
    auto it=p->commands.find(h(cb));if(it==p->commands.end())return;
    auto pass=std::make_shared<Impl::Pass>();pass->id=p->gen();pass->area=info.renderArea;
    auto rp=p->renderPasses.find(h(info.renderPass));auto fb=p->framebuffers.find(h(info.framebuffer));
    pass->renderPass={h(info.renderPass),rp==p->renderPasses.end()?0:rp->second.gen};
    pass->framebuffer={h(info.framebuffer),fb==p->framebuffers.end()?0:fb->second.gen};
    if(fb!=p->framebuffers.end()){
        pass->views=fb->second.views;
        if(fb->second.imageless){
            unsigned count=0;
            for(auto node=static_cast<const VkBaseInStructure*>(info.pNext);node && count++<32;node=node->pNext)
                if(node->sType==VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO){
                    auto attachments=reinterpret_cast<const VkRenderPassAttachmentBeginInfo*>(node);
                    if(attachments->attachmentCount>32)throw std::runtime_error("imageless attachment limit");
                    for(uint32_t i=0;i<attachments->attachmentCount;++i)pass->views.push_back(p->viewRef(attachments->pAttachments[i]));
                    break;
                }
        }
    }
    p->fill(*pass);it->second.pass=std::move(pass);
}
void DrawProbe::nextSubpass(VkCommandBuffer cb){
    auto it=p->commands.find(h(cb));if(it==p->commands.end() || !it->second.pass)return;
    auto pass=std::make_shared<Impl::Pass>(*it->second.pass);pass->id=p->gen();++pass->subpass;
    if(pass->dynamic){it->second.pass.reset();return;}p->fill(*pass);it->second.pass=std::move(pass);
}
void DrawProbe::endPass(VkCommandBuffer cb){auto it=p->commands.find(h(cb));if(it!=p->commands.end())it->second.pass.reset();}
void DrawProbe::beginRendering(VkCommandBuffer cb,const VkRenderingInfo& info){
    auto it=p->commands.find(h(cb));if(it==p->commands.end())return;
    if(info.colorAttachmentCount>16)throw std::runtime_error("dynamic attachment limit");
    auto pass=std::make_shared<Impl::Pass>();pass->id=p->gen();pass->area=info.renderArea;pass->dynamic=true;pass->status="tracked dynamic attachment references";
    auto add=[&](const VkRenderingAttachmentInfo* a,const char* role,uint32_t index){if(a && a->imageView)pass->targets.push_back({p->viewRef(a->imageView),role,index,a->imageLayout,a->loadOp,a->storeOp});};
    for(uint32_t i=0;i<info.colorAttachmentCount;++i)add(&info.pColorAttachments[i],"color",i);
    add(info.pDepthAttachment,"depth",0);add(info.pStencilAttachment,"stencil",0);
    it->second.pass=std::move(pass);
}
void DrawProbe::set(VkDescriptorSet s,VkDescriptorPool pool){if(p->disabled.empty())p->sets[h(s)]={p->gen(),h(pool),0,{}};}
void DrawProbe::write(VkDescriptorSet s){auto it=p->sets.find(h(s));if(it!=p->sets.end()){++it->second.revision;it->second.images.clear();}}
void DrawProbe::freeSet(VkDescriptorSet s){p->sets.erase(h(s));}
void DrawProbe::pool(VkDescriptorPool pool){for(auto it=p->sets.begin();it!=p->sets.end();)if(it->second.pool==h(pool))it=p->sets.erase(it);else ++it;}
void DrawProbe::command(VkCommandBuffer cb,VkCommandPool pool){if(!p->disabled.empty())return;freeCommand(cb);auto& c=p->commands[h(cb)];c.gen=p->gen();c.pool=h(pool);}
void DrawProbe::reset(VkCommandBuffer cb){auto it=p->commands.find(h(cb));if(it!=p->commands.end())p->clear(it->second);}
void DrawProbe::freeCommand(VkCommandBuffer cb){auto it=p->commands.find(h(cb));if(it!=p->commands.end()){p->totalDraws-=it->second.draws.size();p->commands.erase(it);}}
void DrawProbe::commandPool(VkCommandPool pool,bool destroy){for(auto it=p->commands.begin();it!=p->commands.end();) {
    if(it->second.pool!=h(pool)){++it;continue;}p->clear(it->second);if(destroy)it=p->commands.erase(it);else ++it;
}}
void DrawProbe::bindSets(VkCommandBuffer cb,VkPipelineBindPoint point,VkPipelineLayout layout,uint32_t first,uint32_t count,const VkDescriptorSet* sets,uint32_t dynamicCount) {
    if(point!=VK_PIPELINE_BIND_POINT_GRAPHICS)return;
    auto it=p->commands.find(h(cb));if(it==p->commands.end())return;auto& c=it->second;
    auto l=p->layouts.find(h(layout));
    // Exact layout lifetime identity only. Compatible-but-distinct layouts and
    // dynamic-offset sets are conservatively unresolved, never guessed.
    if(l==p->layouts.end() || dynamicCount){c.set={};c.layout=0;return;}
    if(c.layout!=l->second)c.set={};
    c.layout=l->second;
    if(first==0 && count){auto s=p->sets.find(h(sets[0]));c.set=s==p->sets.end()?Impl::Ref{}:Impl::Ref{h(sets[0]),s->second.gen};}
}
void DrawProbe::bindPipeline(VkCommandBuffer cb,VkPipelineBindPoint point,VkPipeline pipeline) {
    if(point!=VK_PIPELINE_BIND_POINT_GRAPHICS)return;auto c=p->commands.find(h(cb));if(c==p->commands.end())return;
    auto it=p->pipelines.find(h(pipeline));c->second.pipeline=it==p->pipelines.end()?Impl::Ref{}:Impl::Ref{h(pipeline),it->second.gen};
}
void DrawProbe::invalidateSets(VkCommandBuffer cb){auto it=p->commands.find(h(cb));if(it!=p->commands.end()){it->second.set={};it->second.layout=0;}}
void DrawProbe::execute(VkCommandBuffer cb,uint32_t count,const VkCommandBuffer* children){
    auto it=p->commands.find(h(cb));if(it==p->commands.end())return;auto& c=it->second;
    for(uint32_t i=0;i<count;++i){auto child=p->commands.find(h(children[i]));if(child!=p->commands.end())c.children[h(children[i])]={child->second.gen,c.pass};}
    if(c.children.size()>256)throw std::runtime_error("draw secondary reference limit");
    // No inherited state is inferred, even if an optional extension allows it.
    c.pipeline={};c.set={};c.layout=0;
}
void DrawProbe::draw(VkCommandBuffer cb,const char* kind,bool potentiallyNonempty,bool indirect){
    auto it=p->commands.find(h(cb));if(it==p->commands.end())return;auto& c=it->second;++c.ordinal;++p->observations[0];
    if(!potentiallyNonempty){++p->observations[1];return;}
    auto pipeline=p->pipelines.find(c.pipeline.handle);auto set=p->sets.find(c.set.handle);
    if(pipeline==p->pipelines.end() || pipeline->second.gen!=c.pipeline.gen){++p->observations[2];return;}
    if(set==p->sets.end() || set->second.gen!=c.set.gen){++p->observations[3];return;}
    if(pipeline->second.layout!=c.layout){++p->observations[4];return;}
    ++p->observations[5];
    Impl::Key key{c.pipeline.gen,c.set.gen,set->second.revision,kind,c.pass?c.pass->id:0};auto existing=c.draws.find(key);
    if(existing!=c.draws.end()){++existing->second.count;existing->second.last=c.ordinal;return;}
    if(c.draws.size()>=1024 || p->totalDraws>=262144){c.truncated=true;++p->observations[6];return;}
    c.draws[key]={c.pipeline,c.set,set->second.revision,c.ordinal,c.ordinal,1,kind,indirect,c.pass};++p->totalDraws;
}
std::map<uint64_t,Json> DrawProbe::links(uint32_t count,const VkCommandBuffer* commands){
    std::map<uint64_t,Json> result;std::set<uint64_t> visited;
    for(uint32_t i=0;i<count;++i)p->collect(h(commands[i]),0,visited,result);
    return result;
}
}
