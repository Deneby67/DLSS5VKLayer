#include "camera_probe.h"
#include "cpu_snapshot_reader.h"
#include "../../third_party/nlohmann/json.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include <tuple>
#include <type_traits>
#include <fstream>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>
namespace dlssfg {
using Json=nlohmann::json;
using Clock=std::chrono::steady_clock;
template<class T> static uint64_t h(T v) { if constexpr(std::is_pointer_v<T>) return reinterpret_cast<uintptr_t>(v); else return uint64_t(v); }
static void check(bool ok,const char* why) { if(!ok) throw std::runtime_error(why); }
struct CameraProbe::Impl {
    struct Memory { uint64_t gen,size; VkMemoryPropertyFlags flags; uint64_t offset=0,length=0; uintptr_t pointer=0; };
    struct Buffer { uint64_t gen,size; bool sparse; uint64_t memory=0,memoryGen=0,offset=0; };
    struct Slice { uint64_t buffer,bufferGen,offset,range; };
    struct Set { uint64_t gen,pool; std::map<uint32_t,Slice> slices; };
    struct SetRef { uint64_t handle,gen; uint32_t index; bool operator<(const SetRef& o) const { return std::tie(handle,gen,index)<std::tie(o.handle,o.gen,o.index); } };
    struct Command { uint64_t gen,pool; std::set<SetRef> sets; std::map<uint64_t,uint64_t> children; };
    VkPhysicalDeviceMemoryProperties properties;
    std::map<uint64_t,Memory> memories;
    std::map<uint64_t,Buffer> buffers;
    std::map<uint64_t,Set> sets;
    std::map<uint64_t,Command> commands;
    std::filesystem::path directory;
    uint64_t deviceId,nextGen=0,presents=0,submissions=0,lastRequest=0,sampleCount=0,bytes=0,totalRefs=0;
    unsigned sampleLimit=0;
    int output=-1;
    Clock::time_point nextPoll{},deadline{},nextSample{};
    std::map<std::string,uint64_t> misses;
    CpuSnapshotReader reader;
    CpuSnapshotReader::Result lastRead;
    static constexpr size_t SliceBytes=464, ObjectLimit=500000, CommandRefLimit=8192;
    Impl(const std::filesystem::path& dir,uint64_t id,const VkPhysicalDeviceMemoryProperties& props):properties(props),directory(dir/"camera"),deviceId(id) {
        std::filesystem::create_directory(directory); chmod(directory.c_str(),0700);
        auto path=directory/("support-"+std::to_string(deviceId)+".json");
        {std::ofstream f(path);check(bool(f),"camera capability file unavailable");
         f<<Json({{"schema",1},{"device",deviceId},{"pid",getpid()},{"mode","CPU pre-submit candidates"}}).dump()<<"\n";check(bool(f),"camera capability write failed");}
        chmod(path.c_str(),0600);
    }
    ~Impl() { if(output>=0) close(output); }
    uint64_t gen() {
        check(memories.size()+buffers.size()+sets.size()+commands.size()<ObjectLimit,"camera tracking object limit"); return ++nextGen;
    }
    void emit(Json j) {
        if(output<0)return;
        j["device"]=deviceId; j["request"]=lastRequest; j["present_marker"]=presents;
        auto text=j.dump(-1,' ',false,Json::error_handler_t::replace)+"\n";
        check(bytes+text.size()<=8ull<<20,"camera output limit");
        size_t done=0;
        while(done<text.size()) { auto n=write(output,text.data()+done,text.size()-done); if(n<0&&errno==EINTR)continue; check(n>0,"camera write failed");done+=size_t(n); }
        bytes+=text.size();
    }
    void finish(const char* why) {
        if(output<0)return;
        emit({{"event","end"},{"reason",why},{"samples",sampleCount},{"misses",misses}});
        close(output); output=-1;
    }
    void poll() {
        auto now=Clock::now();
        if(output>=0 && (now>=deadline || sampleCount>=sampleLimit)) finish(sampleCount>=sampleLimit?"sample limit":"duration elapsed");
        if(now<nextPoll)return;
        nextPoll=now+std::chrono::milliseconds(100);
        int fd=open((directory/"request.json").c_str(),O_RDONLY|O_CLOEXEC|O_NONBLOCK|O_NOFOLLOW);
        if(fd<0)return;
        struct stat st{}; std::array<char,4097> data{}; ssize_t n=-1;
        if(fstat(fd,&st)==0 && S_ISREG(st.st_mode) && st.st_uid==getuid() && st.st_size>0 && st.st_size<=4096) n=read(fd,data.data(),4096);
        close(fd); if(n<=0)return;
        auto j=Json::parse(data.data(),data.data()+n,nullptr,false);
        if(j.is_discarded() || !j.is_object() || !j.contains("id") || !j["id"].is_number_unsigned())return;
        uint64_t id=j["id"].get<uint64_t>(); if(id<=lastRequest)return;
        if(!j.contains("device") || !j["device"].is_number_unsigned() || j["device"].get<uint64_t>()!=deviceId)return;
        if(!j.contains("duration_ms") || !j["duration_ms"].is_number_unsigned() || !j.contains("max_samples") || !j["max_samples"].is_number_unsigned())return;
        auto duration=j["duration_ms"].get<uint64_t>(); auto limit=j["max_samples"].get<uint64_t>();
        if(duration<100 || duration>30000 || limit<1 || limit>1024)return;
        finish("superseded");
        lastRequest=id; sampleLimit=unsigned(limit); sampleCount=bytes=0; misses.clear();
        output=open((directory/("samples-"+std::to_string(deviceId)+"-"+std::to_string(id)+".jsonl")).c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);
        check(output>=0,"camera output unavailable"); deadline=now+std::chrono::milliseconds(duration); nextSample=now;
        emit({{"event","begin"},{"schema",1},{"cpu_snapshots_only",true},{"gpu_completion_verified",false},
            {"bindings",{29,31}},{"slice_bytes",SliceBytes},{"max_samples",sampleLimit},
            {"limitations",{"bound set is not proof of shader consumption","no GPU-write ownership tracking","no camera semantics inferred","dynamic uniform descriptors and update templates unsupported"}}});
    }
    void collect(uint64_t cb,uint64_t expected,std::set<uint64_t>& visited,std::set<SetRef>& refs) {
        if(!visited.insert(cb).second)return;
        check(visited.size()<=256,"camera secondary command limit");
        auto it=commands.find(cb);
        if(it==commands.end() || (expected && expected!=it->second.gen)) { ++misses["unknown or rerecorded command buffer"]; return; }
        refs.insert(it->second.sets.begin(),it->second.sets.end()); check(refs.size()<=CommandRefLimit,"camera submitted set limit");
        for(auto child:it->second.children)collect(child.first,child.second,visited,refs);
    }
    bool readSlice(const Slice& slice,std::array<uint8_t,SliceBytes>& out,std::string& reason) {
        auto bi=buffers.find(slice.buffer);
        if(bi==buffers.end() || bi->second.gen!=slice.bufferGen) {reason="stale buffer";return false;}
        const auto& b=bi->second; auto mi=memories.find(b.memory);
        if(b.sparse || mi==memories.end() || mi->second.gen!=b.memoryGen) {reason="untracked or sparse memory binding";return false;}
        const auto& m=mi->second;
        if(!m.pointer) {reason="memory not mapped";return false;}
        if(!(m.flags&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) || !(m.flags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {reason="memory not host coherent";return false;}
        if(slice.offset>b.size || SliceBytes>b.size-slice.offset || b.offset>m.size || slice.offset>m.size-b.offset) {reason="buffer bounds";return false;}
        auto offset=b.offset+slice.offset;
        if(offset<m.offset || offset-m.offset>m.length || SliceBytes>m.length-(offset-m.offset)) {reason="outside mapped range";return false;}
        if(offset-m.offset>UINTPTR_MAX-m.pointer) {reason="pointer overflow";return false;}
        auto address=m.pointer+uintptr_t(offset-m.offset);
        if(out.size()-1>UINTPTR_MAX-address){reason="pointer overflow";return false;}
        lastRead=reader.read(reinterpret_cast<void*>(address),out.data(),out.size());
        if(!lastRead.ok) {
            ++misses["CPU read errno="+std::to_string(lastRead.error)+", vm errno="+std::to_string(lastRead.vmError)];
            reason="CPU mapping read failed";return false;
        }
        return true;
    }
};
CameraProbe::CameraProbe(const std::filesystem::path& d,uint64_t id,const VkPhysicalDeviceMemoryProperties& m):p(new Impl(d,id,m)) {}
CameraProbe::~CameraProbe()=default;
void CameraProbe::buffer(VkBuffer b,VkDeviceSize size,VkBufferCreateFlags flags) { p->buffers[h(b)]={p->gen(),size,bool(flags&VK_BUFFER_CREATE_SPARSE_BINDING_BIT)}; }
void CameraProbe::destroyBuffer(VkBuffer b) { p->buffers.erase(h(b)); }
void CameraProbe::allocate(VkDeviceMemory m,VkDeviceSize size,uint32_t type) { if(type>=p->properties.memoryTypeCount)return; p->memories[h(m)]={p->gen(),size,p->properties.memoryTypes[type].propertyFlags}; }
void CameraProbe::freeMemory(VkDeviceMemory m) { p->memories.erase(h(m)); }
void CameraProbe::map(VkDeviceMemory m,VkDeviceSize offset,VkDeviceSize size,void* ptr) {
    auto it=p->memories.find(h(m)); if(it==p->memories.end())return; auto& x=it->second;
    if(offset>x.size)return; if(size==VK_WHOLE_SIZE)size=x.size-offset;
    if(size>x.size-offset)return; x.offset=offset;x.length=size;x.pointer=reinterpret_cast<uintptr_t>(ptr);
}
void CameraProbe::unmap(VkDeviceMemory m) { auto it=p->memories.find(h(m)); if(it!=p->memories.end())it->second.pointer=0; }
void CameraProbe::bind(VkBuffer b,VkDeviceMemory m,VkDeviceSize offset) {
    auto bi=p->buffers.find(h(b)); if(bi==p->buffers.end())return; bi->second.memory=bi->second.memoryGen=0;
    auto mi=p->memories.find(h(m));if(mi==p->memories.end())return;
    bi->second.memory=h(m);bi->second.memoryGen=mi->second.gen;bi->second.offset=offset;
}
void CameraProbe::set(VkDescriptorSet s,VkDescriptorPool pool) { p->sets[h(s)]={p->gen(),h(pool),{}}; }
void CameraProbe::freeSet(VkDescriptorSet s) {p->sets.erase(h(s));}
void CameraProbe::pool(VkDescriptorPool pool) {for(auto it=p->sets.begin();it!=p->sets.end();) {if(it->second.pool==h(pool))it=p->sets.erase(it);else ++it;}}
void CameraProbe::invalidate(VkDescriptorSet s) {auto it=p->sets.find(h(s));if(it!=p->sets.end())it->second.slices.clear();}
void CameraProbe::updates(uint32_t count,const VkWriteDescriptorSet* writes,uint32_t copies,const VkCopyDescriptorSet* copy) {
    for(uint32_t i=0;i<count;++i) {
        const auto& w=writes[i];auto it=p->sets.find(h(w.dstSet));if(it==p->sets.end())continue;
        if(w.descriptorCount!=1) {it->second.slices.clear();continue;} // Spill/array writes are deliberately unresolved.
        if(w.dstBinding!=29 && w.dstBinding!=31)continue;
        it->second.slices.erase(w.dstBinding);
        if(w.descriptorType!=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || w.dstArrayElement!=0)continue;
        const auto& b=w.pBufferInfo[0];auto bi=p->buffers.find(h(b.buffer));if(bi==p->buffers.end())continue;
        if(b.range!=Impl::SliceBytes)continue;
        it->second.slices[w.dstBinding]={h(b.buffer),bi->second.gen,b.offset,b.range};
    }
    for(uint32_t i=0;i<copies;++i)invalidate(copy[i].dstSet);
}
void CameraProbe::command(VkCommandBuffer cb,VkCommandPool pool) {p->commands[h(cb)]={p->gen(),h(pool),{}, {}};}
void CameraProbe::freeCommand(VkCommandBuffer cb) {auto it=p->commands.find(h(cb));if(it!=p->commands.end()){p->totalRefs-=it->second.sets.size();p->commands.erase(it);}}
void CameraProbe::begin(VkCommandBuffer cb) {auto it=p->commands.find(h(cb));if(it!=p->commands.end()){p->totalRefs-=it->second.sets.size();it->second.gen=p->gen();it->second.sets.clear();it->second.children.clear();}}
void CameraProbe::commandPool(VkCommandPool pool,bool destroy) {
    for(auto it=p->commands.begin();it!=p->commands.end();) {
        if(it->second.pool!=h(pool)){++it;continue;}
        p->totalRefs-=it->second.sets.size();
        if(destroy)it=p->commands.erase(it);
        else {it->second.gen=p->gen();it->second.sets.clear();it->second.children.clear();++it;}
    }
}
void CameraProbe::bindSets(VkCommandBuffer cb,VkPipelineBindPoint point,uint32_t first,uint32_t count,const VkDescriptorSet* sets) {
    if(point!=VK_PIPELINE_BIND_POINT_GRAPHICS)return;
    auto it=p->commands.find(h(cb));if(it==p->commands.end())return;
    for(uint32_t i=0;i<count;++i) {auto s=p->sets.find(h(sets[i]));if(s==p->sets.end())continue;
        if(it->second.sets.insert({h(sets[i]),s->second.gen,first+i}).second)++p->totalRefs;
        check(p->totalRefs<=262144,"camera total command references limit");
        check(it->second.sets.size()<=Impl::CommandRefLimit,"camera command set limit");}
}
void CameraProbe::execute(VkCommandBuffer cb,uint32_t count,const VkCommandBuffer* children) {
    auto it=p->commands.find(h(cb));if(it==p->commands.end())return;
    for(uint32_t i=0;i<count;++i){auto c=p->commands.find(h(children[i]));if(c!=p->commands.end())it->second.children[h(children[i])]=c->second.gen;}
    check(it->second.children.size()<=256,"camera secondary command limit");
}
void CameraProbe::present() {++p->presents;p->poll();}
bool CameraProbe::wantsSubmit() {p->poll();return p->output>=0 && Clock::now()>=p->nextSample;}
uint64_t CameraProbe::submit(VkQueue queue,uint32_t count,const VkCommandBuffer* commands) {
    ++p->submissions;p->poll();auto now=Clock::now();if(p->output<0 || now<p->nextSample)return 0;
    std::set<uint64_t> visited;std::set<Impl::SetRef> refs;
    for(uint32_t i=0;i<count;++i)p->collect(h(commands[i]),0,visited,refs);
    unsigned recorded=0,attempted=0;std::set<std::array<uint8_t,Impl::SliceBytes>> unique;
    for(const auto& ref:refs) {
        if(ref.index!=0)continue;
        auto it=p->sets.find(ref.handle);if(it==p->sets.end() || it->second.gen!=ref.gen){++p->misses["stale descriptor set"];continue;}
        if(it->second.slices.empty())++p->misses["bound set has no supported 464-byte slice"];
        for(auto kv:it->second.slices) {
            if(recorded>=8 || attempted>=128 || p->sampleCount>=p->sampleLimit)break;
            ++attempted;
            std::array<uint8_t,Impl::SliceBytes> data{};std::string reason;
            if(!p->readSlice(kv.second,data,reason)){++p->misses[reason];continue;}
            if(!unique.insert(data).second)continue;
            std::string hex;hex.reserve(2*data.size());for(auto b:data){hex+="0123456789abcdef"[b>>4];hex+="0123456789abcdef"[b&15];}
            p->emit({{"event","cpu_snapshot_before_submit"},{"submission",p->submissions},{"queue",h(queue)},
                {"sample",++p->sampleCount},{"buffer",kv.second.buffer},{"buffer_generation",kv.second.bufferGen},
                {"offset",kv.second.offset},{"set",ref.handle},{"binding",kv.first},{"descriptor_range",kv.second.range},
                {"bytes_hex",hex},{"camera_verified",false},{"gpu_completion_verified",false},
                {"read_method",p->lastRead.usedPipe?"pipe_copy_from_user":"process_vm_readv"},
                {"vm_read_errno",p->lastRead.vmError},
                {"monotonic_ns",std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count()}});
            ++recorded;
        }
        if(recorded>=8 || attempted>=128 || p->sampleCount>=p->sampleLimit)break;
    }
    if(refs.empty())++p->misses["no tracked bound sets in sampled submission"];
    if(attempted)p->nextSample=now+std::chrono::milliseconds(100);
    return recorded?p->submissions:0;
}
void CameraProbe::result(uint64_t id,VkResult result) {if(id && p->output>=0)p->emit({{"event","submission_result"},{"submission",id},{"result",result}});}
}
