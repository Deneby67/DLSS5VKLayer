#pragma once
// Read-only view of the public NGX x64 MSVC ABI. No SDK implementation copied.
// Explicit slots avoid MinGW's different ordering of overloaded virtual methods.
#include <windows.h>
#include <cstdint>
#include <cmath>
#include "../third_party/nlohmann/json.hpp"

namespace ngx_capture {
using Json = nlohmann::json;
using Result = uint32_t;
constexpr Result Success = 1, ReadFault = 0xffffffffu;
inline bool Read(const void* src, void* dst, size_t size) noexcept {
    SIZE_T copied = 0;
    return src && ReadProcessMemory(GetCurrentProcess(), src, dst, size, &copied) && copied == size;
}
template<class T> Result Get(const void* object, const char* key, T* value, unsigned slot) noexcept {
    using Fn = Result (__cdecl*)(const void*, const char*, T*);
    uintptr_t table = 0;
    Fn fn = nullptr;
    if (!Read(object, &table, sizeof(table)) || !Read(reinterpret_cast<void*>(table + slot * 8), &fn, sizeof(fn)) || !fn)
        return ReadFault;
    // Only the diagnostic getter is guarded; never catch the application's NGX call.
    __try { return fn(object, key, value); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return ReadFault; }
}
struct Resource {
    union {
        struct { uint64_t view, image; uint32_t aspect, baseMip, levels, baseLayer, layers, format, width, height; } image;
        struct { uint64_t buffer; uint32_t bytes; } buffer;
    } data;
    uint32_t type;
    uint8_t readWrite;
    uint8_t padding[3];
};
static_assert(sizeof(Resource) == 56 && offsetof(Resource, type) == 48, "NGX resource ABI");
inline Json ResourceValue(const void* p) {
    Resource r{};
    if (!p) return {{"status", "null"}};
    if (!Read(p, &r, sizeof(r))) return {{"status", "unreadable"}};
    if (r.type == 0) {
        const auto& i = r.data.image;
        if (!i.image || !i.view || !i.width || !i.height || i.width > 32768 || i.height > 32768 ||
            !i.aspect || !i.levels || !i.layers || r.readWrite > 1)
            return {{"status", "invalid_image_descriptor"}};
        return {{"status", "image_descriptor"}, {"image", i.image}, {"view", i.view},
            {"format", i.format}, {"extent", {i.width, i.height}}, {"aspect", i.aspect},
            {"base_mip", i.baseMip}, {"mips", i.levels}, {"base_layer", i.baseLayer},
            {"layers", i.layers}, {"read_write", r.readWrite != 0}, {"gpu_contents_captured", false}};
    }
    if (r.type == 1) return {{"status", "buffer_descriptor"}, {"buffer", r.data.buffer.buffer}, {"size", r.data.buffer.bytes}};
    return {{"status", "unknown_resource_type"}, {"type", r.type}};
}
template<class T> Json Scalar(const void* p, const char* key, unsigned slot, unsigned alias = 0) {
    T value{};
    Result result = Get(p, key, &value, slot);
    bool encoded = false;
    if (result != Success && result != ReadFault && alias) {
        const char encodedKey[] = {'#', char(alias), 0};
        result = Get(p, encodedKey, &value, slot); encoded = true;
    }
    Json out = {{"result", result}, {"encoded_alias", encoded}};
    if (result == Success) {
        if constexpr (std::is_floating_point_v<T>) {
            if (!std::isfinite(value)) { out["nonfinite"] = true; return out; }
        }
        out["value"] = value;
    }
    return out;
}
inline Json Snapshot(const void* p) {
    Json values = Json::object(), resources = Json::object(), matrices = Json::object();
    struct Key { const char* name; unsigned alias; };
    for (const auto& k : {Key{"Width", 0x10}, {"Height", 0x11}, {"OutWidth", 0x12}, {"OutHeight", 0x13},
            {"DLSS.Render.Subrect.Dimensions.Width", 0}, {"DLSS.Render.Subrect.Dimensions.Height", 0},
            {"DLSS.Input.Color.Subrect.Base.X", 0}, {"DLSS.Input.Color.Subrect.Base.Y", 0},
            {"DLSS.Input.Depth.Subrect.Base.X", 0}, {"DLSS.Input.Depth.Subrect.Base.Y", 0},
            {"DLSS.Input.MV.Subrect.Base.X", 0}, {"DLSS.Input.MV.Subrect.Base.Y", 0},
            {"DLSS.Output.Subrect.Base.X", 0}, {"DLSS.Output.Subrect.Base.Y", 0}})
        values[k.name] = Scalar<unsigned>(p, k.name, 12, k.alias);
    for (const auto& k : {Key{"Reset", 0x25}, {"DLSS.Feature.Create.Flags", 0}, {"PerfQualityValue", 0x3f}})
        values[k.name] = Scalar<int>(p, k.name, 11, k.alias);
    for (const auto& k : {Key{"Jitter.Offset.X", 0}, {"Jitter.Offset.Y", 0}, {"MV.Scale.X", 0x2c},
            {"MV.Scale.Y", 0x2d}, {"MV.Offset.X", 0x38}, {"MV.Offset.Y", 0x39},
            {"FrameTimeDeltaInMsec", 0}, {"DLSS.Pre.Exposure", 0}, {"DLSS.Exposure.Scale", 0}})
        values[k.name] = Scalar<float>(p, k.name, 14, k.alias);
    for (const auto& k : {Key{"Color", 0x1e}, {"Output", 0x22}, {"Depth", 0x3d}, {"MotionVectors", 0x27},
            {"ExposureTexture", 0}, {"DLSS.Input.Bias.Current.Color.Mask", 0}}) {
        void* value = nullptr;
        auto result = Get(p, k.name, &value, 8);
        bool encoded = false;
        if (result != Success && result != ReadFault && k.alias) {
            char name[] = {'#', char(k.alias), 0}; result = Get(p, name, &value, 8); encoded = true;
        }
        resources[k.name] = {{"result", result}, {"encoded_alias", encoded}};
        if (result == Success) resources[k.name]["resource"] = ResourceValue(value);
    }
    for (const char* key : {"InvViewProjectionMatrix", "ClipToPrevClipMatrix"}) {
        void* value = nullptr; auto result = Get(p, key, &value, 8);
        matrices[key] = {{"result", result}};
        float data[16]{};
        if (result == Success && Read(value, data, sizeof(data))) {
            bool finite = true; for (float v : data) finite &= std::isfinite(v);
            matrices[key]["finite"] = finite;
            if (finite) matrices[key]["raw_values"] = data;
        }
    }
    return {{"values", values}, {"resources", resources}, {"optional_matrices", matrices},
        {"gpu_completion_verified", false}, {"camera_semantics_verified", false},
        {"handle_namespace", "Windows NGX caller; not assumed native Linux Vulkan handles"}};
}
}
