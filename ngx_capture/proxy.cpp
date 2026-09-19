// RDR2-only NGX observations. Validated pointer redirection; no code-byte patches or GPU work.
#include "params.h"
#include "dispatch.h"
#include <atomic>
#include <map>
#include <string>
#include <cwchar>
#include <cstdio>

namespace {
using namespace ngx_capture;
using Create = Result (__cdecl*)(void*, int, void*, void**);
using Create1 = Result (__cdecl*)(void*, void*, int, void*, void**);
using Evaluate = Result (__cdecl*)(void*, const void*, const void*, void*);
using Release = Result (__cdecl*)(void*);
constexpr unsigned ProviderCount = 1;
struct Provider { HMODULE module = nullptr; std::atomic<uintptr_t> fn[4]{}; } providers[ProviderCount];
SRWLOCK logLock = SRWLOCK_INIT;
wchar_t directory[32768]{};
HANDLE logFile = INVALID_HANDLE_VALUE;
bool logInitialized = false;
bool nrRequested = false;
uint64_t bytes = 0, calls = 0, generation = 0;
uint64_t lastRequest = 0, request = 0, remaining = 0, deadline = 0, nextPoll = 0;
struct Feature { uint64_t generation; int id; };
std::map<std::pair<unsigned, uintptr_t>, Feature> features;
class Lock {
    SRWLOCK* p;
public:
    explicit Lock(SRWLOCK& l) : p(&l) { AcquireSRWLockExclusive(p); }
    ~Lock() { ReleaseSRWLockExclusive(p); }
};
uint64_t Ptr(const void* p) { return reinterpret_cast<uintptr_t>(p); }
void Write(Json event) {
    if (!logInitialized) {
        logInitialized = true;
        wchar_t path[32768];
        if (swprintf(path, 32768, L"%ls\\ngx-%lu-%llu.jsonl", directory, GetCurrentProcessId(), GetTickCount64()) < 0) return;
        logFile = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    if (logFile == INVALID_HANDLE_VALUE) return;
    event["schema"] = 1; event["pid"] = GetCurrentProcessId(); event["tick_ms"] = GetTickCount64();
    std::string line = event.dump(-1, ' ', false, Json::error_handler_t::replace) + '\n';
    if (line.size() > (8ull << 20) - bytes) { CloseHandle(logFile); logFile = INVALID_HANDLE_VALUE; return; }
    DWORD wrote = 0;
    if (!WriteFile(logFile, line.data(), DWORD(line.size()), &wrote, nullptr) || wrote != line.size()) {
        CloseHandle(logFile); logFile = INVALID_HANDLE_VALUE; return;
    }
    bytes += wrote;
}
void Poll() {
    const auto now = GetTickCount64();
    if (deadline && now >= deadline) {
        Write({{"event", "capture_end"}, {"request", request}, {"reason", "duration"}});
        remaining = 0; deadline = 0;
    }
    if (now < nextPoll) return;
    nextPoll = now + 250;
    wchar_t path[32768];
    if (swprintf(path, 32768, L"%ls\\request-%lu.txt", directory, GetCurrentProcessId()) < 0) return;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    char data[128]{}; DWORD count = 0;
    BOOL ok = ReadFile(file, data, sizeof(data)-1, &count, nullptr); CloseHandle(file);
    unsigned long long id = 0; unsigned samples = 0, duration = 0; char extra = 0;
    if (!ok || count >= sizeof(data)-1 || sscanf(data, "%llu %u %u %c", &id, &samples, &duration, &extra) != 3 ||
        !id || id == lastRequest || samples < 1 || samples > 256 || duration < 100 || duration > 30000) return;
    if (deadline) Write({{"event", "capture_end"}, {"request", request}, {"reason", "superseded"}});
    lastRequest = request = id; remaining = samples; deadline = now + duration;
    Write({{"event", "capture_begin"}, {"request", request}, {"max_samples", samples}, {"duration_ms", duration}});
}
template<class F> void Diagnostic(F&& fn) noexcept {
    DWORD error = GetLastError();
    try { fn(); } catch (...) { /* Diagnostic failure must not replace the real NGX call. */ }
    SetLastError(error);
}
void Created(unsigned provider, void* cmd, int id, void* params, void** out, Result result, const Json& before) {
    void* handle = nullptr;
    if (result == Success) Read(out, &handle, sizeof(handle));
    Lock lock(logLock);
    uint64_t gen = 0;
    if (handle && features.size() < 4096) {
        gen = ++generation; features[{provider, Ptr(handle)}] = {gen, id};
    }
    Write({{"event", "create"}, {"provider", provider}, {"feature", id}, {"result", result},
        {"command_buffer", Ptr(cmd)}, {"handle", Ptr(handle)}, {"handle_generation", gen},
        {"parameters", Ptr(params)}, {"before", before}});
}
template<unsigned P> Result __cdecl CreateHook(void* cmd, int id, void* params, void** out) {
    auto fn = reinterpret_cast<Create>(providers[P].fn[0].load());
    Json before;
    Diagnostic([&] { before = Snapshot(params); });
    auto result = fn(cmd, id, params, out);
    Diagnostic([&] { Created(P, cmd, id, params, out, result, before); });
    return result;
}
template<unsigned P> Result __cdecl Create1Hook(void* device, void* cmd, int id, void* params, void** out) {
    auto fn = reinterpret_cast<Create1>(providers[P].fn[1].load());
    Json before;
    Diagnostic([&] { before = Snapshot(params); });
    auto result = fn(device, cmd, id, params, out);
    Diagnostic([&] { Created(P, cmd, id, params, out, result, before); });
    return result;
}
template<unsigned P> Result __cdecl EvaluateHook(void* cmd, const void* handle, const void* params, void* callback) {
    auto fn = reinterpret_cast<Evaluate>(providers[P].fn[2].load());
    uint64_t call = 0, req = 0, gen = 0; int feature = -1;
    Diagnostic([&] {
        Lock lock(logLock); Poll();
        auto it = features.find({P, Ptr(handle)});
        if (it != features.end()) { gen = it->second.generation; feature = it->second.id; }
        if (!remaining || logFile == INVALID_HANDLE_VALUE) return;
        --remaining; call = ++calls; req = request;
    });
    if (call) Diagnostic([&] {
        auto before = Snapshot(params);
        Lock lock(logLock);
        Write({{"event", "evaluate_before"}, {"call", call}, {"request", req}, {"provider", P},
            {"feature", feature}, {"handle_generation", gen}, {"handle", Ptr(handle)},
            {"command_buffer", Ptr(cmd)}, {"parameters", Ptr(params)}, {"callback", Ptr(callback)},
            {"before", before}, {"pipeline_order", nrRequested?Json({"NR","SR"}):Json({"SR"})},
            {"mode", nrRequested?"nr_before_sr_requested":"observe_only"}});
    });
    // Exactly one unchanged real call, outside all diagnostic exception guards/locks.
    using Inline = Result(__cdecl*)(void*,const void*,const void*,void*,Evaluate);
    Inline inlineNr=nullptr;
    if(feature==1 && nrRequested) Diagnostic([&] {
            auto module=GetModuleHandleW(L"vulkan-1.dll");
            if(module) inlineNr=(Inline)GetProcAddress(module,"DlssNrEvaluate");
    });
    auto result = inlineNr ? inlineNr(cmd,handle,params,callback,fn) : fn(cmd, handle, params, callback);
    if (call) Diagnostic([&] {
        Lock lock(logLock);
        Write({{"event", "evaluate_after"}, {"call", call}, {"request", req}, {"result", result},
               {"gpu_completion_verified", false}});
        if (!remaining && deadline && request == req) {
            Write({{"event", "capture_end"}, {"request", req}, {"reason", "sample_limit"}}); deadline = 0;
        }
    });
    return result;
}
template<unsigned P> Result __cdecl ReleaseHook(void* handle) {
    auto fn = reinterpret_cast<Release>(providers[P].fn[3].load());
    auto result = fn(handle);
    Diagnostic([&] {
        Lock lock(logLock);
        if (result == Success) features.erase({P, Ptr(handle)});
        Write({{"event", "release"}, {"provider", P}, {"handle", Ptr(handle)}, {"result", result}});
    });
    return result;
}
template<unsigned P> FARPROC Hook(unsigned op) {
    switch (op) {
        case 0: return reinterpret_cast<FARPROC>(&CreateHook<P>);
        case 1: return reinterpret_cast<FARPROC>(&Create1Hook<P>);
        case 2: return reinterpret_cast<FARPROC>(&EvaluateHook<P>);
        default: return reinterpret_cast<FARPROC>(&ReleaseHook<P>);
    }
}
DWORD WINAPI AttachWorker(void*) {
    const char* names[] = {"NVSDK_NGX_VULKAN_CreateFeature", "NVSDK_NGX_VULKAN_CreateFeature1",
        "NVSDK_NGX_VULKAN_EvaluateFeature", "NVSDK_NGX_VULKAN_ReleaseFeature"};
    bool hooked[4]{};
    const auto until = GetTickCount64() + 120000;
    Diagnostic([&] { Lock lock(logLock); Write({{"event", "attach_wait"}, {"mode", "observe_only"}}); });
    while (GetTickCount64() < until) {
        auto module = GetModuleHandleW(L"nvngx.dll"), backend = GetModuleHandleW(L"_nvngx.dll");
        if (module && backend) for (unsigned op=0; op<4; ++op) {
            if (hooked[op]) continue;
            auto* slot = DispatchSlot(module, reinterpret_cast<void*>(GetProcAddress(module,names[op])));
            auto real = GetProcAddress(backend,names[op]);
            void* current = nullptr;
            // Refuse existing third-party redirects and unknown stub layouts.
            if (!slot || !real || !Read(slot,&current,sizeof(current)) || current != reinterpret_cast<void*>(real)) continue;
            providers[0].fn[op].store(reinterpret_cast<uintptr_t>(real));
            DWORD old=0,unused=0;
            if (!VirtualProtect(slot,sizeof(*slot),PAGE_READWRITE,&old)) continue;
            auto before = InterlockedCompareExchangePointer(slot,reinterpret_cast<void*>(Hook<0>(op)),current);
            VirtualProtect(slot,sizeof(*slot),old,&unused);
            if (before != current) continue;
            hooked[op] = true;
            Diagnostic([&] { Lock lock(logLock); Write({{"event", "intercept"}, {"provider", 0},
                {"export", names[op]}, {"module", "nvngx.dll"}, {"method", "validated_indirect_dispatch_slot"},
                {"mode", "observe_only"}}); });
        }
        if (hooked[0] && hooked[1] && hooked[2] && hooked[3]) return 0;
        Sleep(100);
    }
    Diagnostic([&] { Lock lock(logLock); Write({{"event", "attach_timeout"},
        {"evaluate_hooked", hooked[2]}, {"reason", "loader absent, unsupported thunk or preexisting redirect"}}); });
    return 0;
}
}
BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(self);
        wchar_t executable[32768]{};
        DWORD n = GetModuleFileNameW(nullptr, executable, 32768);
        if (!n || n >= 32768) return TRUE;
        const wchar_t* base = wcsrchr(executable, L'\\'); base = base ? base+1 : executable;
        if (_wcsicmp(base, L"RDR2.exe")) return TRUE;
        wchar_t nrFlag[8]{};
        nrRequested=GetEnvironmentVariableW(L"DLSSNR_INLINE",nrFlag,8)==1 && nrFlag[0]=='1';
        n = GetEnvironmentVariableW(L"DLSSFG_NGX_CAPTURE_DIR", directory, 32768);
        if (!n || n >= 32768 || directory[1] != L':' || (directory[2] != L'/' && directory[2] != L'\\')) return TRUE;
        // The worker does not run under the loader lock and exits after attachment.
        // This statically imported DLL lives for the process lifetime.
        HANDLE worker = CreateThread(nullptr,0,&AttachWorker,nullptr,0,nullptr);
        if (worker) CloseHandle(worker);
    }
    // Installation failure only disables observation. Version exports still forward.
    return TRUE;
}
