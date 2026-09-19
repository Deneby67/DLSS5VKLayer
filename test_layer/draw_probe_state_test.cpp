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
    p.view(view,vi);auto rp=fake<VkRenderPass>(12);auto fb=fake<VkFramebuffer>(13);
    VkAttachmentDescription attachment{};attachment.format=im.format;attachment.samples=VK_SAMPLE_COUNT_1_BIT;
    VkAttachmentReference ref{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subs[2]{};for(auto& sub:subs)sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;
    subs[0].colorAttachmentCount=1;subs[0].pColorAttachments=&ref;
    VkRenderPassCreateInfo rpInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};rpInfo.attachmentCount=1;rpInfo.pAttachments=&attachment;rpInfo.subpassCount=2;rpInfo.pSubpasses=subs;p.renderPass(rp,rpInfo);
    VkFramebufferCreateInfo fbInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};fbInfo.renderPass=rp;fbInfo.attachmentCount=1;fbInfo.pAttachments=&view;p.framebuffer(fb,fbInfo);
    VkRenderPassBeginInfo beginPass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};beginPass.renderPass=rp;beginPass.framebuffer=fb;beginPass.renderArea.extent={1920,1080};
    p.reset(cb);bind(cb);p.beginPass(cb,beginPass);p.draw(cb,"direct",true,false);p.nextSubpass(cb);p.draw(cb,"direct",true,false);p.endPass(cb);p.draw(cb,"direct",true,false);
    links=query();assert(links.at(4).size()==3);
    unsigned withAttachment=0,emptySubpass=0,unresolved=0;
    for(const auto& l:links.at(4)){const auto& t=l["render_targets"];
        if(t["status"]=="no resolved render scope")++unresolved;
        else if(t["subpass"]==1 && t["attachments"].empty())++emptySubpass;
        else if(t["attachments"].size()==1 && t["attachments"][0]["valid"]==true)++withAttachment;
    }
    assert(withAttachment==1 && emptySubpass==1 && unresolved==1);
    fbInfo.flags=VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT;fbInfo.pAttachments=nullptr;p.framebuffer(fb,fbInfo);
    VkRenderPassAttachmentBeginInfo imageless{VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO};imageless.attachmentCount=1;imageless.pAttachments=&view;beginPass.pNext=&imageless;
    VkCommandBufferInheritanceInfo inherit{VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO};inherit.renderPass=rp;
    VkCommandBufferBeginInfo start{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};start.flags=VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;start.pInheritanceInfo=&inherit;
    p.reset(cb);p.reset(secondary);p.inheritance(secondary,start);bind(secondary);p.draw(secondary,"direct",true,false);
    p.beginPass(cb,beginPass);p.execute(cb,1,&secondary);p.endPass(cb);
    assert(query().at(4)[0]["render_targets"]["attachments"][0]["valid"]==true);
    p.destroy(VK_OBJECT_TYPE_FRAMEBUFFER,13);p.framebuffer(fb,fbInfo);
    assert(query().at(4)[0]["render_targets"]["status"]=="stale render scope");
    VkRenderingAttachmentInfo dynamicColor{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};dynamicColor.imageView=view;dynamicColor.imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkRenderingInfo dynamic{VK_STRUCTURE_TYPE_RENDERING_INFO};dynamic.colorAttachmentCount=1;dynamic.pColorAttachments=&dynamicColor;dynamic.renderArea.extent={1920,1080};
    p.reset(cb);bind(cb);p.beginRendering(cb,dynamic);p.draw(cb,"direct",true,false);p.endPass(cb);
    assert(query().at(4)[0]["render_targets"]["dynamic_rendering"]==true);
    assert(query().at(4)[0]["render_targets"]["attachments"][0]["valid"]==true);
    p.destroy(VK_OBJECT_TYPE_IMAGE_VIEW,11);p.view(view,vi);
    assert(query().at(4)[0]["render_targets"]["attachments"][0]["valid"]==false);
    p.commandPool(cp,false);assert(query().empty());
    bind(cb);p.draw(cb,"direct",true,false);p.commandPool(cp,true);assert(query().empty());
    DrawProbe unsupported;unsupported.disable("unsupported feature");unsupported.command(cb,cp);unsupported.layout(layout);unsupported.shader(shader,std::string(64,'a'));
    unsupported.pipeline(pipeline,info);unsupported.set(set,pool);unsupported.bindPipeline(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
    unsupported.bindSets(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,1,&set,0);unsupported.draw(cb,"direct",true,false);
    assert(unsupported.links(1,&cb).empty());
    std::cout<<"PASS: draw associations, descriptor generations, shader lifetime, subpasses, imageless/inherited/dynamic targets, stale views/framebuffers and unsupported mode\n";
}
