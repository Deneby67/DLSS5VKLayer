#pragma once
#include <windows.h>
#include <cstdio>
#include <cstring>

// Private, bounded startup breadcrumbs. No DllMain I/O, no thread suspension,
// GPU waits, pointers or graphics parameters in the journal.
class BootstrapTrace {
    const char* name;
    LONG call=0;
    bool completed=false;
    static LONG Ticket(const char* name) {
        static const char* names[]={"resolve","vkEnumeratePhysicalDevices","vkCreateDevice",
            "vkCreateCommandPool","vkAllocateCommandBuffers","vkBeginCommandBuffer",
            "vkCreateInstance","vkCreateWin32SurfaceKHR","vkCreateSwapchainKHR",
            "vkQueueSubmit","vkWaitForFences","vkAcquireNextImageKHR","vkQueuePresentKHR"};
        static volatile LONG counts[sizeof(names)/sizeof(*names)]{};
        for(unsigned i=0;i<sizeof(names)/sizeof(*names);++i) if(!strcmp(name,names[i])) {
            // Stop touching a shared counter after the bounded startup window.
            if(InterlockedCompareExchange(&counts[i],0,0)>=32) return 0;
            auto n=InterlockedIncrement(&counts[i]);return n<=32?n:0;
        }
        return 0;
    }
    void write(const char* phase,long result) {
        if(!call)return;
        auto error=GetLastError();
        wchar_t directory[32768]{},path[32768]{};
        DWORD n=GetEnvironmentVariableW(L"DLSSNR_BOOTSTRAP_DIR",directory,32768);
        if(n && n<32768 && swprintf(path,32768,L"%ls/bootstrap-%lu.log",directory,GetCurrentProcessId())>0) {
            HANDLE file=CreateFileW(path,FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,
                nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
            if(file!=INVALID_HANDLE_VALUE) {
                char line[256];int size=snprintf(line,sizeof(line),"tick=%llu tid=%lu api=%s call=%ld %s result=%ld\n",
                    GetTickCount64(),GetCurrentThreadId(),name,call,phase,result);
                DWORD written=0;if(size>0 && size<int(sizeof(line)))WriteFile(file,line,DWORD(size),&written,nullptr);
                CloseHandle(file);
            }
        }
        SetLastError(error);
    }
public:
    BootstrapTrace(const char* api,bool active):name(api) {if(active)call=Ticket(api);write("enter",0);}
    void done(long result){completed=true;write("exit",result);}
    ~BootstrapTrace(){if(!completed)write("leave",0);}
};
