#include "../layer_linux/src/draw_probe.h"
#include <cassert>
#include <iostream>
using namespace dlssfg;
template<class T>T fake(uintptr_t value){return reinterpret_cast<T>(value);}
int main(){
    DrawProbe p;auto shader=fake<VkShaderModule>(1);auto layout=fake<VkPipelineLayout>(2);
    auto pipeline=fake<VkPipeline>(3);auto set=fake<VkDescriptorSet>(4);auto pool=fake<VkDescriptorPool>(5);
    auto cb=fake<VkCommandBuffer>(6);auto cp=fake<VkCommandPool>(7);auto secondary=fake<VkCommandBuffer>(8);
    p.shader(shader,std::string(64,'a'));p.layout(layout);
    VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};stage.module=shader;stage.stage=VK_SHADER_STAGE_VERTEX_BIT;stage.pName="main";
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};info.layout=layout;info.stageCount=1;info.pStages=&stage;
    p.pipeline(pipeline,info);p.set(set,pool);p.command(cb,cp);p.command(secondary,cp);
    auto image=fake<VkImage>(10);auto view=fake<VkImageView>(11);
    VkImageCreateInfo im{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};im.format=VK_FORMAT_R16G16_SFLOAT;im.extent={1920,1080,1};im.usage=VK_IMAGE_USAGE_SAMPLED_BIT;
    p.image(image,im);VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=image;vi.format=im.format;vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};p.view(view,vi);
    VkDescriptorImageInfo ii{};ii.imageView=view;ii.imageLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=set;write.dstBinding=2;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;write.pImageInfo=&ii;p.descriptor(write);
    auto bind=[&](VkCommandBuffer c){p.bindPipeline(c,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);p.bindSets(c,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,1,&set,0);};
    auto query=[&]{return p.links(1,&cb);};
    bind(cb);assert(query().empty()); // Bound but never drawn must not become evidence.
    p.draw(cb,"direct",false,false);assert(query().empty());
    p.draw(cb,"direct",true,false);auto links=query();assert(links.size()==1 && links.at(4).size()==1);
    assert(links.at(4)[0]["first_draw_ordinal"]==2 && links.at(4)[0]["stages"][0]["sha256"]==std::string(64,'a'));
    assert(links.at(4)[0]["set0_image_bindings"][0]["view_format"]==VK_FORMAT_R16G16_SFLOAT);
    p.destroy(VK_OBJECT_TYPE_IMAGE,10);p.image(image,im);
    assert(query().at(4)[0]["set0_image_bindings"].empty()); // Do not relabel the old view with a reused image handle.
    p.write(set);assert(query().empty()); // Descriptor revision invalidates recorded associations.
    p.reset(cb);bind(cb);p.draw(cb,"direct",true,false);p.pool(pool);p.set(set,pool);assert(query().empty());
    p.reset(cb);bind(cb);p.invalidateSets(cb);p.draw(cb,"direct",true,false);assert(query().empty());
    p.reset(cb);bind(cb);p.bindSets(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,1,&set,1);p.draw(cb,"direct",true,false);assert(query().empty());
    auto other=fake<VkPipelineLayout>(9);p.layout(other);p.reset(cb);bind(cb);
    p.bindSets(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,other,1,0,nullptr,0);p.draw(cb,"direct",true,false);assert(query().empty());
    p.reset(cb);bind(cb);p.execute(cb,0,nullptr);p.draw(cb,"direct",true,false);assert(query().empty());
    bind(secondary);p.draw(secondary,"indirect",true,true);assert(query().empty()); // Not executed yet.
    p.execute(cb,1,&secondary);links=query();assert(links.at(4)[0]["indirect_count_unknown"]==true);
    p.reset(secondary);assert(query().empty()); // Never attach evidence from a rerecorded child.
    p.reset(cb);bind(cb);p.draw(cb,"direct",true,false);p.destroy(VK_OBJECT_TYPE_PIPELINE,3);p.pipeline(pipeline,info);assert(query().empty());
    p.reset(cb);bind(cb);p.draw(cb,"direct",true,false);p.destroy(VK_OBJECT_TYPE_SHADER_MODULE,1);
    assert(!query().empty()); // Pipeline owns its shader metadata after module destruction.
    p.commandPool(cp,false);assert(query().empty());
    bind(cb);p.draw(cb,"direct",true,false);p.commandPool(cp,true);assert(query().empty());
    DrawProbe unsupported;unsupported.disable("unsupported feature");unsupported.command(cb,cp);unsupported.layout(layout);unsupported.shader(shader,std::string(64,'a'));
    unsupported.pipeline(pipeline,info);unsupported.set(set,pool);unsupported.bindPipeline(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
    unsupported.bindSets(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,1,&set,0);unsupported.draw(cb,"direct",true,false);
    assert(unsupported.links(1,&cb).empty());
    std::cout<<"PASS: draw-only association, zero draw, descriptor revisions, handle generations, layout mismatch, dynamic offsets, secondary reset, shader lifetime and unsupported mode\n";
}
