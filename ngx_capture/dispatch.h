#pragma once
#include "params.h"
#include <cstring>
namespace ngx_capture {
// Recognize only the driver's x64 indirect export stub. No offsets are pinned
// to a game or driver build; both pointers must stay inside the loaded image.
inline void** DispatchSlot(HMODULE module, const void* function) noexcept {
    unsigned char code[71]{};
    if (!Read(function, code, sizeof(code))) return nullptr;
    for (unsigned i=0; i<20; ++i) if (code[i] != 0x90) return nullptr;
    const unsigned char prefix[] = {0x48,0xb8};
    const unsigned char middle[] = {0x48,0x8b,0,0x48,0x83,0xf8,0,0x74,0xed,0x48,0xb8};
    const unsigned char suffix[] = {0x48,0x8b,0,0x48,0x83,0xf8,0,0x75,0x0b,0x48,0xb8,2,0,0xd0,0xba,0,0,0,0,0xc3,0x50,0xc3};
    if (memcmp(code+20,prefix,sizeof(prefix)) || memcmp(code+30,middle,sizeof(middle)) ||
        memcmp(code+49,suffix,sizeof(suffix))) return nullptr;
    IMAGE_DOS_HEADER dos{}; IMAGE_NT_HEADERS64 nt{};
    auto base = reinterpret_cast<uintptr_t>(module);
    if (!Read(module,&dos,sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 ||
        !Read(reinterpret_cast<void*>(base+dos.e_lfanew),&nt,sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return nullptr;
    uintptr_t ready=0,slot=0,initialized=0;
    memcpy(&ready,code+22,8); memcpy(&slot,code+41,8);
    auto inside = [&](uintptr_t p) { return p>=base && p-base<=nt.OptionalHeader.SizeOfImage-8 && !(p&7); };
    if (nt.OptionalHeader.SizeOfImage<8 || !inside(ready) || !inside(slot) ||
        !Read(reinterpret_cast<void*>(ready),&initialized,sizeof(initialized)) || !initialized) return nullptr;
    return reinterpret_cast<void**>(slot);
}
}
