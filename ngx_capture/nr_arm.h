#pragma once
#include <windows.h>
#include <cstdio>
#include <string>
#include "../core/logging.h"

// Poll only under renderLock. An installer-selected control file keeps NR off
// through startup, then permits a session-bound enable/disable without restart.
struct InlineArm {
    wchar_t file[32768]{};
    ULONGLONG token=0,nextPoll=0;
    bool configured=false,announced=false,armed=false;
    void configure() {
        token=GetTickCount64();
        auto n=GetEnvironmentVariableW(L"DLSSNR_ARM_FILE",file,32768);
        configured=n!=0;
        if(n>=32768)file[0]=0; // Invalid selection stays off.
    }
    bool set(bool on) {
        if(!configured || !file[0])return false;
        std::wstring temporary=std::wstring(file)+L".key.tmp";
        HANDLE out=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(out==INVALID_HANDLE_VALUE)return false;
        char data[128];int size=snprintf(data,sizeof(data),"%lu %llu %u\n",GetCurrentProcessId(),token,unsigned(on));
        DWORD written=0;bool ok=WriteFile(out,data,size,&written,nullptr) && written==DWORD(size);
        CloseHandle(out);
        ok=ok && MoveFileExW(temporary.c_str(),file,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
        if(!ok){DeleteFileW(temporary.c_str());return false;}
        nextPoll=0;return true;
    }
    bool allow() {
        if(!configured)return true; // Ungated isolated fixtures only; installer always sets a path.
        if(!announced) {
            announced=true;
            dlssnr::Log("[nr-inline] arm-ready pid=%lu token=%llu; NR disabled until matching request",GetCurrentProcessId(),token);
        }
        auto now=GetTickCount64();if(now<nextPoll)return armed;nextPoll=now+250;
        bool requested=false;
        HANDLE input=CreateFileW(file,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(input!=INVALID_HANDLE_VALUE) {
            char data[128]{};DWORD size=0;unsigned long pid=0;unsigned long long supplied=0;unsigned mode=0;char extra=0;
            if(ReadFile(input,data,sizeof(data)-1,&size,nullptr) && size<sizeof(data)-1 &&
               sscanf(data,"%lu %llu %u %c",&pid,&supplied,&mode,&extra)==3)
                requested=pid==GetCurrentProcessId() && supplied==token && mode==1;
            CloseHandle(input);
        }
        if(requested!=armed)dlssnr::Log("[nr-inline] arm-state %s",requested?"enabled":"disabled");
        armed=requested;return armed;
    }
};
