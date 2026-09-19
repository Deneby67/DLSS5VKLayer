#include "../ngx_capture/params.h"
#include "../ngx_capture/color_overlay.h"
#include <vector>
#include <cstdio>
#include <cstdlib>
using namespace ngx_capture;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %u: %s (error=%lu)\n", __LINE__, #x, GetLastError()); return 1; } } while (0)
static unsigned callbacks;
static void Callback(float progress) { if (progress == 0.5f) ++callbacks; }
static bool Report(const Json& result) {
    wchar_t path[32768]{};
    if (!GetModuleFileNameW(nullptr, path, 32768)) return false;
    auto* base = wcsrchr(path, L'\\'); if (!base) return false;
    wcscpy(base+1, L"probe-result.json");
    auto text = result.dump(2) + "\n";
    HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0; BOOL ok = WriteFile(file, text.data(), DWORD(text.size()), &written, nullptr);
    CloseHandle(file); return ok && written == text.size();
}
template<class F> F Function(HMODULE m, const char* name) { return reinterpret_cast<F>(GetProcAddress(m, name)); }
template<class T> void Set(void* p, const char* key, T value, unsigned slot) {
    using F = void(*)(void*, const char*, T);
    reinterpret_cast<F>((*reinterpret_cast<void***>(p))[slot])(p, key, value);
}
int main(int argc, char** argv) {
    // Three real version.dll imports force the production proxy's entry path.
    wchar_t path[32768]{}; GetSystemDirectoryW(path, 32000); wcscat(path, L"\\kernel32.dll");
    DWORD ignored = 0; auto bytes = GetFileVersionInfoSizeW(path, &ignored);
    CHECK(bytes > 0);
    std::vector<unsigned char> version(bytes);
    CHECK(GetFileVersionInfoW(path, 0, bytes, version.data()));
    void* value = nullptr; UINT size = 0;
    CHECK(VerQueryValueA(version.data(), "\\", &value, &size) && size >= sizeof(VS_FIXEDFILEINFO));
    CHECK(static_cast<VS_FIXEDFILEINFO*>(value)->dwSignature == 0xfeef04bd);
    bool real = argc > 1 && !strcmp(argv[1], "--real");
    HMODULE module;
    if (real) {
        GetSystemDirectoryW(path, 32000); wcscat(path, L"\\nvngx.dll"); module = LoadLibraryW(path);
    } else module = LoadLibraryW(L"nvngx.dll");
    CHECK(module);
    Sleep(400); // Let the bounded loader observer attach; never part of the benchmark.
    void* p = nullptr;
    if (real) {
        auto allocate = Function<unsigned(*)(void**)>(module, "NVSDK_NGX_VULKAN_AllocateParameters");
        CHECK(allocate);
        auto result = allocate(&p);
        printf("real AllocateParameters = %#x\n", result); CHECK(result == 1 && p);
    } else {
        auto get = Function<void*(*)()>(module, "TestParameters"); CHECK(get); p = get();
    }
    Resource color{}; color.data.image = {101, 102, 1, 0, 1, 0, 1, 97, 640, 360};
    Resource depth = color; depth.data.image.format = 100;
    Resource motion = color; motion.data.image.format = 83;
    Resource output = color; output.data.image.width = 1280; output.data.image.height = 720; output.readWrite = 1;
    Set<unsigned>(p, "Width", 640, 4); Set<unsigned>(p, "Height", 360, 4);
    Set<unsigned>(p, "OutWidth", 1280, 4); Set<unsigned>(p, "OutHeight", 720, 4);
    Set<int>(p, "Reset", 0, 3); Set<int>(p, "DLSS.Feature.Create.Flags", 9, 3);
    Set<float>(p, "Jitter.Offset.X", .25f, 6); Set<float>(p, "Jitter.Offset.Y", -.25f, 6);
    Set<float>(p, "MV.Scale.X", 1.f, 6); Set<float>(p, "MV.Scale.Y", -1.f, 6);
    Set<void*>(p, "Color", &color, 0); Set<void*>(p, "Depth", &depth, 0);
    Set<void*>(p, "MotionVectors", &motion, 0); Set<void*>(p, "Output", &output, 0);
    auto before = Snapshot(p);
    ColorOverlay overlay(p,&output);
    auto overlaid=Snapshot(&overlay);
    CHECK(overlaid["resources"]["Color"]["resource"]["extent"] == Json({1280,720}));
    CHECK(overlaid["resources"]["MotionVectors"] == before["resources"]["MotionVectors"]);
    CHECK(overlaid["values"] == before["values"]);
    CHECK(Snapshot(p) == before);
    CHECK(before["values"]["Width"]["value"] == 640);
    CHECK(before["values"]["Jitter.Offset.X"]["value"] == .25f);
    CHECK(before["resources"]["Color"]["resource"]["image"] == 102);
    CHECK(before["resources"]["MotionVectors"]["resource"]["format"] == 83);
    CHECK(Snapshot(reinterpret_cast<void*>(1))["values"]["Width"]["result"] == ReadFault);
    Set<void*>(p, "Depth", reinterpret_cast<void*>(1), 0);
    CHECK(Snapshot(p)["resources"]["Depth"]["resource"]["status"] == "unreadable");
    Set<void*>(p, "Depth", &depth, 0);
    if (real) {
        CHECK(GetModuleFileNameW(module, path, 32768));
        CHECK(wcsstr(path, L"\\windows\\system32\\nvngx.dll"));
        CHECK(Function<unsigned(*)(void*)>(module, "NVSDK_NGX_VULKAN_DestroyParameters")(p) == 1);
        CHECK(Report({{"passed", true}, {"real_nvidia_parameters", true}, {"snapshot", before}}));
        puts("PASS: real NVIDIA parameter ABI and version forwarding"); return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "--armed")) {
        wchar_t dir[32768]{}, request[32768]{};
        CHECK(GetEnvironmentVariableW(L"DLSSFG_NGX_CAPTURE_DIR", dir, 32768));
        CHECK(swprintf(request, 32768, L"%ls\\request-%lu.txt", dir, GetCurrentProcessId()) > 0);
        HANDLE file = CreateFileW(request, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(file != INVALID_HANDLE_VALUE);
        const char text[] = "123456 2 5000\n"; DWORD n = 0;
        CHECK(WriteFile(file, text, sizeof(text)-1, &n, nullptr)); CloseHandle(file);
    }
    auto create = Function<unsigned(*)(void*, int, void*, void**)>(module, "NVSDK_NGX_VULKAN_CreateFeature");
    auto create1 = Function<unsigned(*)(void*, void*, int, void*, void**)>(module, "NVSDK_NGX_VULKAN_CreateFeature1");
    auto evaluate = Function<unsigned(*)(void*, const void*, const void*, void*)>(module, "NVSDK_NGX_VULKAN_EvaluateFeature");
    auto release = Function<unsigned(*)(void*)>(module, "NVSDK_NGX_VULKAN_ReleaseFeature");
    CHECK(create && create1 && evaluate && release);
    void* handle = nullptr; auto cmd = reinterpret_cast<void*>(0x1234);
    CHECK(create(cmd, 1, p, &handle) == 1 && handle); CHECK(GetLastError() == 171);
    CHECK(evaluate(cmd, handle, p, reinterpret_cast<void*>(&Callback)) == 1); CHECK(GetLastError() == 172);
    CHECK(evaluate(nullptr, handle, p, nullptr) == 0xbad00005u);
    CHECK(callbacks == 1);
    CHECK(Function<unsigned(*)()>(module, "TestCalls")() == 2);
    CHECK(Function<unsigned(*)()>(module, "TestCallbacks")() == 1);
    CHECK(Snapshot(p) == before); // The observer may Get, never Set/Reset.
    double ns = 0;
    if (argc > 1 && !strcmp(argv[1], "--bench")) {
        LARGE_INTEGER start, end, frequency;
        QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&start);
        for (unsigned i=0; i<100000; ++i) CHECK(evaluate(cmd, handle, p, nullptr) == 1);
        QueryPerformanceCounter(&end);
        ns = double(end.QuadPart-start.QuadPart) / frequency.QuadPart * 1e9 / 100000;
    }
    CHECK(release(handle) == 1); CHECK(GetLastError() == 173);
    CHECK(create1(reinterpret_cast<void*>(0x5678), cmd, 1, p, &handle) == 1);
    CHECK(release(handle) == 1);
    CHECK(!GetProcAddress(module, "NonexistentExport"));
    CHECK(GetLastError() == ERROR_PROC_NOT_FOUND);
    CHECK(GetProcAddress(module, MAKEINTRESOURCEA(65000)) == nullptr); // Ordinals are never parsed as strings.
    CHECK(Report({{"passed", true}, {"real_nvidia_parameters", false}, {"idle_evaluate_ns", ns}}));
    puts("PASS: exact create/evaluate/release forwarding, callback, failures, parameter immutability, version exports");
    return 0;
}
