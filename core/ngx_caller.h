#pragma once
#include <windows.h>
#include <cstddef>
#include <cstring>
#include "logging.h"
namespace dlssnr {
static HMODULE g_spoofCaller = nullptr;
static decltype(&GetModuleFileNameW) g_realGetModuleFileNameW = nullptr;
// ---------------------------------------------------------------------------
// Caller-identity spoof: IAT hook of KERNEL32!GetModuleFileNameW inside the
// snippet/core modules so they see "nvngx.dll" as the caller.
// ---------------------------------------------------------------------------
static DWORD WINAPI SpoofedGetModuleFileNameW(HMODULE module, LPWSTR filename, DWORD size) noexcept {
    if (module == g_spoofCaller) {
        static constexpr wchar_t AUTHORIZED_CALLER[] = L"nvngx.dll";
        constexpr DWORD LEN = ARRAYSIZE(AUTHORIZED_CALLER) - 1;
        if (!filename || !size) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return 0; }
        if (size <= LEN) {
            if (size > 1) std::memcpy(filename, AUTHORIZED_CALLER, (size - 1) * sizeof(wchar_t));
            filename[size - 1] = L'\0';
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return size;
        }
        std::memcpy(filename, AUTHORIZED_CALLER, sizeof(AUTHORIZED_CALLER));
        return LEN;
    }
    if (g_realGetModuleFileNameW) return g_realGetModuleFileNameW(module, filename, size);
    SetLastError(ERROR_INVALID_FUNCTION);
    return 0;
}

struct SpoofState { void** slot = nullptr; decltype(&GetModuleFileNameW) orig = nullptr; };

static void** FindImportedFunctionSlot(HMODULE module, const char* functionName) noexcept {
    if (!module || !functionName) return nullptr;
    auto* base = reinterpret_cast<std::byte*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return nullptr;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return nullptr;
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress || !dir.Size || dir.VirtualAddress >= nt->OptionalHeader.SizeOfImage)
        return nullptr;
    auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
    const auto* descEnd = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress + dir.Size);
    for (; desc < descEnd && desc->Name; ++desc) {
        if (desc->Name >= nt->OptionalHeader.SizeOfImage) continue;
        const char* lib = reinterpret_cast<const char*>(base + desc->Name);
        if (_stricmp(lib, "KERNEL32.dll") != 0 &&
            _stricmp(lib, "api-ms-win-core-libraryloader-l1-2-0.dll") != 0 &&
            _stricmp(lib, "api-ms-win-core-libraryloader-l1-1-0.dll") != 0) continue;
        if (!desc->OriginalFirstThunk || !desc->FirstThunk) continue;
        auto* nameThunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + desc->OriginalFirstThunk);
        auto* addrThunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + desc->FirstThunk);
        for (; nameThunk->u1.AddressOfData; ++nameThunk, ++addrThunk) {
            if (IMAGE_SNAP_BY_ORDINAL64(nameThunk->u1.Ordinal)) continue;
            const uint32_t rva = static_cast<uint32_t>(nameThunk->u1.AddressOfData);
            if (rva >= nt->OptionalHeader.SizeOfImage) return nullptr;
            const auto* imp = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + rva);
            if (std::strcmp(reinterpret_cast<const char*>(imp->Name), functionName) == 0)
                return reinterpret_cast<void**>(&addrThunk->u1.Function);
        }
    }
    return nullptr;
}

static bool InstallCallerSpoof(HMODULE module, SpoofState& state, HMODULE caller) {
    g_spoofCaller = caller;
    state.slot = FindImportedFunctionSlot(module, "GetModuleFileNameW");
    if (!state.slot) { Log("[spoof] module %p has no GetModuleFileNameW import", (void*)module); return false; }
    DWORD old = 0;
    if (!VirtualProtect(state.slot, sizeof(void*), PAGE_READWRITE, &old)) {
        Log("[spoof] VirtualProtect failed (%lu)", GetLastError()); return false;
    }
    state.orig = reinterpret_cast<decltype(state.orig)>(
        InterlockedExchangePointer(reinterpret_cast<void* volatile*>(state.slot),
                                   reinterpret_cast<void*>(&SpoofedGetModuleFileNameW)));
    VirtualProtect(state.slot, sizeof(void*), old, &old);
    FlushInstructionCache(GetCurrentProcess(), state.slot, sizeof(void*));
    if (!state.orig) { Log("[spoof] original import was null"); return false; }
    g_realGetModuleFileNameW = state.orig;
    Log("[spoof] GetModuleFileNameW IAT hooked at %p (module %p)", (void*)state.slot, (void*)module);
    return true;
}

static void RemoveCallerSpoof(SpoofState& state) {
    if (!state.slot || !state.orig) return;
    DWORD old = 0;
    if (VirtualProtect(state.slot, sizeof(void*), PAGE_READWRITE, &old)) {
        InterlockedExchangePointer(reinterpret_cast<void* volatile*>(state.slot),
                                   reinterpret_cast<void*>(state.orig));
        VirtualProtect(state.slot, sizeof(void*), old, &old);
    }
    state = {};
}

}
