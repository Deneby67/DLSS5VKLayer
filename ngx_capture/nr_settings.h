#pragma once
#include "../core/ngx_snippet.h"
#include <cmath>

// Header-only attachment: never initialise, resize or touch the frame transport.
// The GUI owns the mapping; a game starts disarmed even if Enabled persisted.
struct InlineSettings {
    HANDLE file=INVALID_HANDLE_VALUE,mapping=nullptr;
    ShmHeader* header=nullptr;
    bool configured=false;
    ULONGLONG nextOpen=0;
    dlssnr::NgxTuning tuning{};
    float sharpness=0;
    bool enabled=true;
    bool read() {
        if(!header) {
            auto now=GetTickCount64();if(now<nextOpen)return !configured;nextOpen=now+1000;
            wchar_t path[32768]{};
            auto n=GetEnvironmentVariableW(L"DLSSNR_INLINE_SHM",path,32768);
            configured=n!=0;
            if(!configured)return true; // Isolated fixtures without GUI settings.
            if(n>=32768)return false;
            file=CreateFileW(path,GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
            LARGE_INTEGER size{};
            if(file==INVALID_HANDLE_VALUE)return false;
            if(GetFileSizeEx(file,&size) && size.QuadPart>=LONGLONG(kHeaderBytes)) {
                mapping=CreateFileMappingW(file,nullptr,PAGE_READWRITE,0,0,nullptr);
                if(mapping)header=(ShmHeader*)MapViewOfFile(mapping,FILE_MAP_READ|FILE_MAP_WRITE,0,0,kHeaderBytes);
            }
            if(!header){close();return false;}
        }
        if(header->magic!=kShmMagic || header->version!=kShmVersion)return false;
        const auto seq=header->controlSeq.load();
        dlssnr::NgxTuning t;
        t.intensity=BitsToFloat(header->intensityBits.load());
        t.localTone=BitsToFloat(header->localToneBits.load());
        t.localStructure=BitsToFloat(header->localStructureBits.load());
        t.skinStructure=BitsToFloat(header->skinStructureBits.load());
        t.style=header->style.load();t.preset=header->preset.load();t.autoMask=header->autoMask.load();
        float s=BitsToFloat(header->sharpnessBits.load());
        bool on=header->enabled.load()!=0;
        if(seq!=header->controlSeq.load())return false;
        auto valid=[](float v,float lo,float hi){return std::isfinite(v) && v>=lo && v<=hi;};
        if(!valid(t.intensity,0,4) || !valid(t.localTone,0,4) || !valid(t.localStructure,0,4) ||
           !valid(t.skinStructure,-1,4) || !valid(s,0,1) || t.style>2 || t.preset>15 || t.autoMask>1)return false;
        tuning=t;sharpness=s;enabled=on;return true;
    }
    void setEnabled(bool on) {
        if(header && header->magic==kShmMagic && header->version==kShmVersion) {
            header->enabled.store(on?1:0);header->controlSeq.fetch_add(1);enabled=on;
        }
    }
    void close() {
        if(header)UnmapViewOfFile(header);
        if(mapping)CloseHandle(mapping);
        if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);
        header=nullptr;mapping=nullptr;file=INVALID_HANDLE_VALUE;
    }
};

struct InlineKeyEdge {
    bool held=false;
    bool update(bool down,bool foreground) {
        const bool pressed=foreground && down && !held;
        held=down;return pressed;
    }
};
// Resolve existing user32 only while evaluating SR, never while holding the loader lock.
inline bool InlineF2(InlineKeyEdge& edge) {
    auto user=GetModuleHandleW(L"user32.dll");if(!user)return false;
    auto key=(SHORT(WINAPI*)(int))GetProcAddress(user,"GetAsyncKeyState");
    auto window=(HWND(WINAPI*)())GetProcAddress(user,"GetForegroundWindow");
    auto owner=(DWORD(WINAPI*)(HWND,LPDWORD))GetProcAddress(user,"GetWindowThreadProcessId");
    if(!key || !window || !owner)return false;
    DWORD pid=0;owner(window(),&pid);
    return edge.update((key(VK_F2)&0x8000)!=0,pid==GetCurrentProcessId());
}
