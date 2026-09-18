#include "../layer_linux/src/camera_probe.h"
#include "../layer_linux/src/cpu_snapshot_reader.h"
#include "../third_party/nlohmann/json.hpp"
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include <cassert>
#include <iostream>
#include <unistd.h>
#include <sys/mman.h>
using namespace dlssfg;
template<class T> T fake(uintptr_t p){return reinterpret_cast<T>(p);}
int main(int argc,char** argv) {
    {
        CpuSnapshotReader reader;std::array<unsigned char,464> input{},output{};input.fill(73);
        auto page=sysconf(_SC_PAGESIZE);assert(page>=464);
        auto mapping=static_cast<unsigned char*>(mmap(nullptr,page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
        assert(mapping!=MAP_FAILED);memset(mapping,9,page);assert(mprotect(mapping+page,page,PROT_NONE)==0);
        for(int i=0;i<16;++i){
            auto bad=reader.read(reinterpret_cast<void*>(1),output.data(),output.size());assert(!bad.ok && bad.error==EFAULT);
            auto partial=reader.read(mapping+page-232,output.data(),output.size());assert(!partial.ok && partial.error==EFAULT);
            auto good=reader.read(input.data(),output.data(),output.size());assert(good.ok && output==input);
        }
        assert(munmap(mapping,page*2)==0);
        std::cout<<"PASS: guarded partial mapping, invalid address and reader recovery\n";
    }
    assert(argc==2);std::filesystem::path root=argv[1];std::filesystem::create_directories(root);
    VkPhysicalDeviceMemoryProperties props{};props.memoryTypeCount=2;props.memoryTypes[0].propertyFlags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;props.memoryTypes[1].propertyFlags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    CameraProbe p(root,1,props);auto buffer=fake<VkBuffer>(1);auto memory=fake<VkDeviceMemory>(2);auto set=fake<VkDescriptorSet>(3);auto pool=fake<VkDescriptorPool>(4);auto command=fake<VkCommandBuffer>(5);auto cp=fake<VkCommandPool>(6);auto queue=fake<VkQueue>(7);
    std::array<unsigned char,1024> bytes{};bytes.fill(42);
    p.buffer(buffer,512,0);p.allocate(memory,bytes.size(),0);p.bind(buffer,memory,128);p.map(memory,64,bytes.size()-64,bytes.data()+64);
    p.set(set,pool);p.command(command,cp);
    VkDescriptorBufferInfo bi{buffer,0,464};VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w.dstSet=set;w.dstBinding=29;w.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;w.descriptorCount=1;w.pBufferInfo=&bi;
    auto update=[&]{p.updates(1,&w,0,nullptr);};auto record=[&]{p.begin(command);p.bindSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,0,1,&set);};
    auto submit=[&]{auto id=p.submit(queue,1,&command);p.result(id,VK_SUCCESS);std::this_thread::sleep_for(std::chrono::milliseconds(110));};
    update();record();
    {std::ofstream f(root/"camera/request.json");f<<"{\"id\":1,\"device\":1,\"duration_ms\":1800,\"max_samples\":32}";}
    submit(); // Good data.
    p.freeMemory(memory);p.allocate(memory,bytes.size(),0);p.map(memory,64,bytes.size()-64,bytes.data()+64);submit(); // Old allocation generation.
    p.bind(buffer,memory,128);p.destroyBuffer(buffer);p.buffer(buffer,512,0);p.bind(buffer,memory,128);submit(); // Old buffer generation.
    update();p.pool(pool);p.set(set,pool);update();submit(); // Old set generation.
    record();p.map(memory,0,1024,reinterpret_cast<void*>(1));submit(); // Kernel read fails without signal.
    p.unmap(memory);submit(); // Explicitly unmapped.
    p.freeMemory(memory);p.allocate(memory,bytes.size(),1);p.bind(buffer,memory,128);p.map(memory,0,bytes.size(),bytes.data());submit(); // Not coherent.
    p.freeMemory(memory);p.allocate(memory,bytes.size(),0);p.bind(buffer,memory,128);p.map(memory,0,200,bytes.data());submit(); // Partial mapping.
    p.map(memory,0,bytes.size(),bytes.data());p.invalidate(set);submit(); // Template/copy invalidation.
    update();p.commandPool(cp,false);submit(); // Old command bindings removed.
    std::this_thread::sleep_for(std::chrono::milliseconds(900));p.present();
    std::ifstream f(root/"camera/samples-1-1.jsonl");std::string line;unsigned samples=0;nlohmann::json end;
    while(std::getline(f,line)) {auto j=nlohmann::json::parse(line);if(j["event"]=="cpu_snapshot_before_submit") {++samples;std::string expected;for(int i=0;i<464;++i)expected+="2a";assert(j["bytes_hex"]==expected);}if(j["event"]=="end")end=j;}
    assert(samples==1);auto misses=end.at("misses");
    for(const char* why:{"untracked or sparse memory binding","stale buffer","stale descriptor set","CPU mapping read failed","memory not mapped","memory not host coherent","outside mapped range","no tracked bound sets in sampled submission"})assert(misses.at(why).get<unsigned>()>=1);
    std::cout<<"PASS: stale allocation/buffer/set generations, bad pointer, unmap, noncoherent and partial mappings, command reset\n";
}
