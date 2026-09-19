// Owned synthetic loader fixture: an indirect export table, not NVIDIA code.
#include <windows.h>
extern "C" {
__declspec(align(8)) uintptr_t testNgxReady = 0;
__declspec(align(8)) void* testNgxDispatch[4]{};
}
BOOL WINAPI DllMain(HMODULE self, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH) {
        wchar_t path[32768]{};
        if (!GetModuleFileNameW(self,path,32768)) return FALSE;
        auto* base=wcsrchr(path,L'\\'); if (!base) return FALSE;
        wcscpy(base+1,L"_nvngx.dll");
        auto module=LoadLibraryW(path); if (!module) return FALSE;
        const char* names[]={"NVSDK_NGX_VULKAN_CreateFeature","NVSDK_NGX_VULKAN_CreateFeature1",
            "NVSDK_NGX_VULKAN_EvaluateFeature","NVSDK_NGX_VULKAN_ReleaseFeature"};
        for(unsigned i=0;i<4;++i) {
            testNgxDispatch[i]=reinterpret_cast<void*>(GetProcAddress(module,names[i]));
            if (!testNgxDispatch[i]) return FALSE;
        }
        testNgxReady=1;
    }
    return TRUE;
}
