#pragma once
#include <cerrno>
#include <cstddef>
#include <fcntl.h>
#include <sys/uio.h>
#include <unistd.h>

namespace dlssfg {
// A bounded, fault-contained read of our own mapping. NVIDIA device-local CPU
// mappings can be VM_IO/PFNMAP: get_user_pages in process_vm_readv rejects them.
// Ordinary pipe write copies through copy_from_user instead of pinning pages.
// Neither path installs signal handlers or directly dereferences the source.
// Caller must serialize use and keep the Vulkan mapping alive throughout.
class CpuSnapshotReader {
    int pipe_[2]{-1,-1};
    void reset() noexcept {
        for(auto& fd:pipe_) {if(fd>=0)close(fd);fd=-1;}
    }
public:
    struct Result {
        bool ok=false,usedPipe=false;
        ssize_t vmBytes=-1;
        int vmError=0,error=0;
    };
    CpuSnapshotReader()=default;
    CpuSnapshotReader(const CpuSnapshotReader&)=delete;
    CpuSnapshotReader& operator=(const CpuSnapshotReader&)=delete;
    ~CpuSnapshotReader(){reset();}
    Result read(const void* source,void* destination,size_t size) noexcept {
        Result result;
        // Linux PIPE_BUF >= 4096. One <=464-byte write to an empty private pipe
        // cannot block or interleave. On any failure discard all buffered bytes.
        if(!size || size>464){result.error=EINVAL;return result;}
        iovec local{destination,size},remote{const_cast<void*>(source),size};
        do {result.vmBytes=process_vm_readv(getpid(),&local,1,&remote,1,0);}while(result.vmBytes<0 && errno==EINTR);
        result.vmError=result.vmBytes<0?errno:0;
        if(result.vmBytes==ssize_t(size)){result.ok=true;return result;}
        result.usedPipe=true;
        if(pipe_[0]<0 && pipe2(pipe_,O_NONBLOCK|O_CLOEXEC)<0){result.error=errno;reset();return result;}
        ssize_t n;
        do {n=write(pipe_[1],source,size);}while(n<0 && errno==EINTR);
        if(n!=ssize_t(size)){result.error=n<0?errno:EIO;reset();return result;}
        do {n=::read(pipe_[0],destination,size);}while(n<0 && errno==EINTR);
        if(n!=ssize_t(size)){result.error=n<0?errno:EIO;reset();return result;}
        result.ok=true;return result;
    }
};
}
