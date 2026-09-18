#include <windows.h>
#include <d3d11.h>
#include <cstdio>
int main() {
    auto lib=LoadLibraryW(L"d3d11.dll");
    if (!lib) return 2;
    auto create=reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(GetProcAddress(lib,"D3D11CreateDevice"));
    if (!create) return 3;
    ID3D11Device* device=nullptr; ID3D11DeviceContext* context=nullptr; D3D_FEATURE_LEVEL level{};
    auto hr=create(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,&level,&context);
    std::printf("D3D11CreateDevice=%#lx feature=%#x\n",static_cast<unsigned long>(hr),level);
    char path[1024];
    if (GetEnvironmentVariableA("DLSSFG_PROBE_RESULT",path,sizeof(path))) {
        if (FILE* log=std::fopen(path,"w")) {
            std::fprintf(log,"D3D11CreateDevice=%#lx feature=%#x\n",static_cast<unsigned long>(hr),level);
            std::fclose(log);
        }
    }
    // Give the host test a bounded window to verify mapped native libraries.
    if (SUCCEEDED(hr)) Sleep(4000);
    if (context) context->Release();
    if (device) device->Release();
    FreeLibrary(lib);
    return FAILED(hr)?4:0;
}
