#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

namespace dlssfg {
enum class MotionSource { None, Engine, OpticalFlow };
enum class Units { Pixels, UV, NDC };
struct Binding {
    std::string shaderSha256, stage;
    uint32_t set=0, binding=0, element=0;
};
struct TextureBinding : Binding { std::string format; };
struct MatrixBinding : Binding { uint32_t byteOffset=0; bool columnMajor=false; };
struct Profile {
    std::string id, executable, executableSha256;
    bool enabled=false, opticalFallback=true;
    TextureBinding motion, depth;
    Units motionUnits=Units::Pixels;
    std::array<float,2> motionScale{1,1};
    bool previousToCurrent=false, motionIncludesJitter=false, depthReversed=false;
    MatrixBinding worldToView, viewToClip, clipToPreviousClip;
};
struct LoadResult {
    Profile profile;
    bool valid=false;
    std::string reason;
};
LoadResult LoadProfile(const std::filesystem::path& path);
bool MatchesExecutable(const Profile& profile, const std::string& executable,
                       const std::string& sha256);

// The capture layer must stamp every resource after its writes complete. A
// handle being present alone is not evidence that it belongs to this frame.
struct CapturedInputs {
    uint64_t colorFrame=0, motionFrame=0, depthFrame=0, cameraFrame=0, opticalFlowFrame=0;
    bool profileMatches=false, motionValid=false, depthValid=false, cameraValid=false;
    bool opticalFlowReady=false;
};
struct SourceDecision {
    MotionSource motion=MotionSource::None;
    bool canGenerate=false;
    std::string reason;
};
SourceDecision ChooseSources(const LoadResult& profile, const CapturedInputs& inputs);
// Converts a sampled engine vector to current->previous pixel displacement.
// Dimensions are those of the motion field, not the output swapchain.
std::array<float,2> MotionToPixels(const Profile& profile, std::array<float,2> motion,
                                 uint32_t width, uint32_t height,
                                 std::array<float,2> currentJitter,
                                 std::array<float,2> previousJitter);

// Reload at a frame boundary. Invalid edits replace the profile with a failure
// result: a previously valid profile must not survive a deliberate broken edit.
class ProfileFile {
public:
    bool reload(const std::filesystem::path& path);
    const LoadResult& current() const { return current_; }
private:
    std::filesystem::path path_;
    std::filesystem::file_time_type time_{};
    uintmax_t size_=0;
    bool known_=false;
    LoadResult current_;
};
}
