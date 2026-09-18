#include "fg_profile.h"
#include "../third_party/nlohmann/json.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace dlssfg {
using Json=nlohmann::json;
static std::string Lower(std::string s) {
    for (char& c:s) if (c>='A' && c<='Z') c+=32;
    return s;
}
static std::string Base(std::string s) {
    auto p=s.find_last_of("/\\");
    return Lower(p==std::string::npos?s:s.substr(p+1));
}
static void Require(bool value,const std::string& reason) { if (!value) throw std::runtime_error(reason); }
static bool Hash(const std::string& s) {
    return s.size()==64 && s.find_first_not_of("0123456789abcdefABCDEF")==std::string::npos;
}
static uint32_t Uint(const Json& j,const char* name,uint32_t limit) {
    const auto& value=j.at(name);
    Require(value.is_number_unsigned(),std::string(name)+" must be a nonnegative integer");
    auto n=value.get<uint64_t>(); Require(n<=limit,std::string(name)+" exceeds its limit");
    return uint32_t(n);
}
static Binding ReadBinding(const Json& j) {
    Binding b;
    b.shaderSha256=Lower(j.at("shader_sha256").get<std::string>());
    Require(Hash(b.shaderSha256),"shader_sha256 must contain 64 hexadecimal digits");
    b.stage=j.at("stage").get<std::string>();
    Require(b.stage=="vertex" || b.stage=="fragment" || b.stage=="compute","unsupported shader stage");
    b.set=Uint(j,"set",31); b.binding=Uint(j,"binding",65535); b.element=Uint(j,"array_element",65535);
    return b;
}
static TextureBinding ReadTexture(const Json& j) {
    TextureBinding t; static_cast<Binding&>(t)=ReadBinding(j);
    t.format=j.at("format").get<std::string>(); return t;
}
static MatrixBinding ReadMatrix(const Json& j) {
    MatrixBinding m; static_cast<Binding&>(m)=ReadBinding(j);
    m.byteOffset=Uint(j,"byte_offset",65536-64);
    Require(m.byteOffset%4==0,"matrix byte_offset must be aligned to a float");
    const auto order=j.at("order").get<std::string>();
    Require(order=="row_major" || order=="column_major","unsupported matrix order");
    m.columnMajor=order=="column_major"; return m;
}
LoadResult LoadProfile(const std::filesystem::path& path) {
    LoadResult result;
    try {
        Require(std::filesystem::is_regular_file(path),"profile file not found");
        Require(std::filesystem::file_size(path)<=256*1024,"profile exceeds 256 KiB");
        std::ifstream file(path); Require(bool(file),"cannot read profile");
        const auto j=Json::parse(file);
        Require(Uint(j,"schema_version",1)==1,"unsupported profile schema version");
        auto& p=result.profile;
        p.id=j.at("id").get<std::string>();
        Require(!p.id.empty() && p.id.size()<=64 && p.id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789._-")==std::string::npos,"invalid profile id");
        p.enabled=j.at("enabled").get<bool>();
        p.executable=Base(j.at("executable").get<std::string>());
        Require(!p.executable.empty(),"executable is empty");
        p.opticalFallback=j.value("optical_flow_fallback",true);
        if (!p.enabled) { result.valid=true; result.reason="profile disabled; capture calibration required"; return result; }
        p.executableSha256=Lower(j.at("executable_sha256").get<std::string>());
        Require(Hash(p.executableSha256),"enabled profiles require an executable SHA-256");
        const auto& mv=j.at("motion"); p.motion=ReadTexture(mv);
        Require(mv.at("sample_grid")=="current","previous-frame motion fields require reprojection, not just sign inversion");
        Require(p.motion.format=="R16G16_SFLOAT" || p.motion.format=="R32G32_SFLOAT" || p.motion.format=="R16G16_SNORM","unsupported motion texture format");
        const auto units=mv.at("units").get<std::string>();
        Require(units=="pixels" || units=="uv" || units=="ndc","unsupported motion units");
        p.motionUnits=units=="pixels"?Units::Pixels:(units=="uv"?Units::UV:Units::NDC);
        const auto direction=mv.at("direction").get<std::string>();
        Require(direction=="current_to_previous" || direction=="previous_to_current","unsupported motion direction");
        p.previousToCurrent=direction=="previous_to_current";
        p.motionScale=mv.at("scale").get<std::array<float,2>>();
        for (float s:p.motionScale) Require(std::isfinite(s) && s!=0 && std::abs(s)<=16,"invalid motion scale");
        p.motionIncludesJitter=mv.at("includes_jitter").get<bool>();
        p.depth=ReadTexture(j.at("depth"));
        Require(p.depth.format=="D32_SFLOAT" || p.depth.format=="D32_SFLOAT_S8_UINT" || p.depth.format=="D24_UNORM_S8_UINT" || p.depth.format=="D16_UNORM" || p.depth.format=="R32_SFLOAT","unsupported depth texture format");
        Require(j.at("depth").at("encoding")=="hardware_zero_to_one","only hardware depth in [0,1] is supported");
        p.depthReversed=j.at("depth").at("reversed").get<bool>();
        const auto& camera=j.at("camera");
        Require(camera.at("projection")=="perspective_zero_to_one","unsupported camera projection");
        p.worldToView=ReadMatrix(camera.at("world_to_view"));
        p.viewToClip=ReadMatrix(camera.at("view_to_clip"));
        p.clipToPreviousClip=ReadMatrix(camera.at("clip_to_previous_clip"));
        result.valid=true; result.reason="profile parsed; live resources have not been validated";
    } catch (const std::exception& e) { result.profile.enabled=false; result.reason=e.what(); }
    return result;
}
bool MatchesExecutable(const Profile& p,const std::string& exe,const std::string& hash) {
    return p.enabled && Base(exe)==p.executable && Hash(hash) && Lower(hash)==p.executableSha256;
}
SourceDecision ChooseSources(const LoadResult& p,const CapturedInputs& i) {
    SourceDecision d;
    const bool matched=p.valid && p.profile.enabled && i.profileMatches;
    if (matched && i.motionValid && i.motionFrame==i.colorFrame && i.colorFrame) d.motion=MotionSource::Engine;
    else if ((!p.valid || p.profile.opticalFallback) && i.opticalFlowReady &&
             i.colorFrame && i.opticalFlowFrame==i.colorFrame) d.motion=MotionSource::OpticalFlow;
    if (!i.colorFrame) d.reason="no current color frame";
    else if (!matched) d.reason="no matching validated game profile; depth and camera are unavailable";
    else if (!i.depthValid || i.depthFrame!=i.colorFrame) d.reason="depth is missing or belongs to another frame";
    else if (!i.cameraValid || i.cameraFrame!=i.colorFrame) d.reason="camera is missing or belongs to another frame";
    else if (d.motion==MotionSource::None) d.reason="no motion vectors for this frame";
    else { d.canGenerate=true; d.reason=d.motion==MotionSource::Engine?"engine motion vectors":"optical flow fallback"; }
    return d;
}
std::array<float,2> MotionToPixels(const Profile& p,std::array<float,2> v,uint32_t w,uint32_t h,
                                 std::array<float,2> current,std::array<float,2> previous) {
    Require(w>0 && h>0,"motion dimensions must be positive");
    for (unsigned a=0;a<2;++a) {
        Require(std::isfinite(v[a]) && std::isfinite(current[a]) && std::isfinite(previous[a]),"non-finite motion or jitter");
        float scale=p.motionScale[a];
        if (p.motionUnits!=Units::Pixels) scale*=float(a?h:w)*(p.motionUnits==Units::NDC?0.5f:1.0f);
        v[a]*=scale*(p.previousToCurrent?-1.0f:1.0f);
        if (p.motionIncludesJitter) v[a]-=previous[a]-current[a];
        Require(std::isfinite(v[a]),"motion conversion overflow");
    }
    return v;
}
bool ProfileFile::reload(const std::filesystem::path& path) {
    std::error_code ec;
    auto t=std::filesystem::last_write_time(path,ec);
    if (ec) {
        bool changed=known_ || path_!=path;
        path_=path; known_=false; current_=LoadProfile(path); return changed;
    }
    auto size=std::filesystem::file_size(path,ec);
    if (!ec && known_ && path_==path && time_==t && size_==size) return false;
    path_=path; time_=t; size_=size; known_=true; current_=LoadProfile(path); return true;
}
}
