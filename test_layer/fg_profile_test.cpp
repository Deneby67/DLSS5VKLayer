#include "../common/fg_profile.h"
#include "../third_party/nlohmann/json.hpp"
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace dlssfg;
using Json=nlohmann::json;
static void Check(bool v,const char* message) { if (!v) throw std::runtime_error(message); }
int main(int argc,char** argv) {
    if (argc!=2) return 2; // caller provides an isolated temporary directory
    const auto path=std::filesystem::path(argv[1])/"profile.json";
    const std::string hash(64,'a');
    Json binding={{"shader_sha256",hash},{"stage","compute"},{"set",0},
                  {"binding",3},{"array_element",0}};
    Json mv=binding, depth=binding, matrix=binding;
    mv.update({{"format","R16G16_SFLOAT"},{"sample_grid","current"},{"units","uv"},
               {"direction","current_to_previous"},{"scale",{1,-1}},{"includes_jitter",false}});
    depth.update({{"format","D32_SFLOAT"},{"encoding","hardware_zero_to_one"},{"reversed",true}});
    matrix.update({{"byte_offset",256},{"order","column_major"}});
    Json good={{"schema_version",1},{"id","fixture"},{"enabled",true},{"executable","Game.exe"},
        {"executable_sha256",hash},{"optical_flow_fallback",true},{"motion",mv},{"depth",depth},
        {"camera",{{"projection","perspective_zero_to_one"},{"world_to_view",matrix},
                   {"view_to_clip",matrix},{"clip_to_previous_clip",matrix}}}};
    const auto write=[&](const Json& j) { std::ofstream(path)<<j.dump(2); };
    write(good); auto profile=LoadProfile(path);
    Check(profile.valid,"valid fixture rejected");
    Check(MatchesExecutable(profile.profile,"Z:\\games\\GAME.EXE",hash),"basename/hash match failed");
    Check(!MatchesExecutable(profile.profile,"Game.exe",std::string(64,'b')),"different executable build accepted");
    Check(!MatchesExecutable(profile.profile,"Launcher.exe",hash),"launcher accepted");
    for (int mutation=0;mutation<8;++mutation) {
        auto bad=good;
        switch (mutation) {
        case 0: bad["schema_version"]=2; break;
        case 1: bad["motion"]["shader_sha256"]="0x12345678"; break;
        case 2: bad["camera"]["world_to_view"]["byte_offset"]=257; break;
        case 3: bad["camera"]["world_to_view"]["byte_offset"]=65536; break;
        case 4: bad["motion"]["sample_grid"]="previous"; break;
        case 5: bad["depth"]["set"]=-1; break;
        case 6: bad["motion"]["scale"]={1,0}; break;
        case 7: bad.erase("executable_sha256"); break;
        }
        write(bad); auto result=LoadProfile(path);
        Check(!result.valid && !result.profile.enabled,"unsafe profile accepted");
    }
    CapturedInputs i; i.colorFrame=i.motionFrame=i.depthFrame=i.cameraFrame=i.opticalFlowFrame=42;
    i.profileMatches=i.motionValid=i.depthValid=i.cameraValid=i.opticalFlowReady=true;
    auto d=ChooseSources(profile,i);
    Check(d.canGenerate && d.motion==MotionSource::Engine,"engine vectors not preferred");
    i.motionFrame=41; d=ChooseSources(profile,i);
    Check(d.canGenerate && d.motion==MotionSource::OpticalFlow,"stale engine vectors did not fall back");
    i.opticalFlowFrame=41; Check(!ChooseSources(profile,i).canGenerate,"stale fallback accepted");
    i.opticalFlowFrame=42; i.depthFrame=41;
    Check(!ChooseSources(profile,i).canGenerate,"stale depth accepted");
    i.depthFrame=42; i.cameraValid=false;
    Check(!ChooseSources(profile,i).canGenerate,"missing camera accepted");
    i.cameraValid=true; i.profileMatches=false;
    Check(!ChooseSources(profile,i).canGenerate,"unmatched game accepted");
    i.profileMatches=true; auto noFallback=profile; noFallback.profile.opticalFallback=false;
    Check(!ChooseSources(noFallback,i).canGenerate,"disabled fallback ignored");
    i.colorFrame=i.motionFrame=i.depthFrame=i.cameraFrame=i.opticalFlowFrame=0;
    Check(!ChooseSources(profile,i).canGenerate,"empty frame accepted");

    auto v=MotionToPixels(profile.profile,{.25f,.5f},800,600,{0,0},{0,0});
    Check(v[0]==200 && v[1]==-300,"UV conversion/orientation failed");
    auto p=profile.profile; p.motionUnits=Units::NDC; p.previousToCurrent=true;
    v=MotionToPixels(p,{.25f,.5f},800,600,{0,0},{0,0});
    Check(v[0]==-100 && v[1]==150,"NDC/sign conversion failed");
    p.motionUnits=Units::Pixels; p.previousToCurrent=false; p.motionScale={1,1}; p.motionIncludesJitter=true;
    v=MotionToPixels(p,{8,2},800,600,{.25f,-.25f},{-.25f,.25f});
    Check(v[0]==8.5f && v[1]==1.5f,"jitter removal failed");
    bool rejected=false;
    try { MotionToPixels(p,{std::numeric_limits<float>::infinity(),0},800,600,{0,0},{0,0}); }
    catch (const std::exception&) { rejected=true; }
    Check(rejected,"non-finite motion accepted");

    write(good); ProfileFile file;
    Check(file.reload(path) && file.current().valid,"initial profile load failed");
    Check(!file.reload(path),"unchanged profile reloaded");
    std::ofstream(path)<<"{ broken";
    Check(file.reload(path) && !file.current().valid,"bad edit retained old profile");
    std::filesystem::remove(path);
    Check(file.reload(path) && !file.current().valid,"deleted profile retained");
    std::cout<<"PASS: profile parsing, build matching, frame consistency, fallback, motion conversion, reload\n";
}
