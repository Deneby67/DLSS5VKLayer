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
    struct Pipeline {uint64_t gen,layout;Json stages;};
    struct Draw {Ref pipeline,set;uint64_t revision,first,last,count;std::string kind;bool indirect;};
    using Key=std::tuple<uint64_t,uint64_t,uint64_t,std::string>;
    struct Command {
        uint64_t gen=0,pool=0,ordinal=0,layout=0;
        Ref pipeline,set;
        bool truncated=false;
        std::map<Key,Draw> draws;
        std::map<uint64_t,uint64_t> children;
    };
    std::map<uint64_t,std::string> shaders;
    std::map<uint64_t,uint64_t> layouts;
    std::map<uint64_t,Pipeline> pipelines;
    std::map<uint64_t,Set> sets;
    std::map<uint64_t,Image> images;
    std::map<uint64_t,View> views;
    std::map<uint64_t,Command> commands;
    uint64_t next=0,totalDraws=0;
    std::array<uint64_t,7> observations{};
    std::string disabled;
    uint64_t gen() {
        if(shaders.size()+layouts.size()+pipelines.size()+sets.size()+commands.size()+images.size()+views.size()>500000)
            throw std::runtime_error("draw association object limit");
        return ++next;
    }
    void clear(Command& c) {totalDraws-=c.draws.size();auto pool=c.pool;c=Command{};c.pool=pool;c.gen=gen();}
    void collect(uint64_t cb,uint64_t generation,std::set<uint64_t>& visited,std::map<uint64_t,Json>& result) {
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
                {"command_examples_truncated",c.truncated},{"gpu_execution_verified",false},
                {"descriptor_binding_used_by_shader_verified",false}});
            if(result.size()>8192)throw std::runtime_error("draw submitted set limit");
        }
        for(auto child:c.children)collect(child.first,child.second,visited,result);
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
    for(uint32_t i=0;i<count;++i){auto child=p->commands.find(h(children[i]));if(child!=p->commands.end())c.children[h(children[i])]=child->second.gen;}
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
    Impl::Key key{c.pipeline.gen,c.set.gen,set->second.revision,kind};auto existing=c.draws.find(key);
    if(existing!=c.draws.end()){++existing->second.count;existing->second.last=c.ordinal;return;}
    if(c.draws.size()>=1024 || p->totalDraws>=262144){c.truncated=true;++p->observations[6];return;}
    c.draws[key]={c.pipeline,c.set,set->second.revision,c.ordinal,c.ordinal,1,kind,indirect};++p->totalDraws;
}
std::map<uint64_t,Json> DrawProbe::links(uint32_t count,const VkCommandBuffer* commands){
    std::map<uint64_t,Json> result;std::set<uint64_t> visited;
    for(uint32_t i=0;i<count;++i)p->collect(h(commands[i]),0,visited,result);
    return result;
}
}
