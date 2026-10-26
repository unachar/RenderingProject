#pragma once

#include "main.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "scene.h"

struct VmdKeyframe
{
    uint32_t Frame = 0;
    aiVector3D Position{};
    aiQuaternion Rotation{};
    XMFLOAT2 XP1{0, 0}, XP2{1, 1};
    XMFLOAT2 YP1{0, 0}, YP2{1, 1};
    XMFLOAT2 ZP1{0, 0}, ZP2{1, 1};
    XMFLOAT2 RotP1{0, 0}, RotP2{1, 1};
};

struct VmdScalarKeyframe
{
    uint32_t Frame = 0;
    float Value = 0.0f;
};

struct VmdIkKeyframe
{
    uint32_t Frame = 0;
    bool Enable = true;
};

struct VmdCameraSample
{
    XMFLOAT3 Center{};
    XMFLOAT3 Rotation{};
    float Distance = -45.0f;
    float Fov = DirectX::XM_PI / 6.0f;
    bool Perspective = true;
};

struct VmdCameraKeyframe : VmdCameraSample
{
    uint32_t Frame = 0;
    // Position X/Y/Z, Euler rotation, distance, field of view.
    array<XMFLOAT2, 6> P1{};
    array<XMFLOAT2, 6> P2{{{1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}}};
};

struct VmdAnimation
{
    string ModelName{};
    uint32_t MaxFrame = 0;
    uint32_t MotionCount = 0;
    uint32_t MorphCount = 0;
    uint32_t CameraCount = 0;
    uint32_t LightCount = 0;
    uint32_t ShadowCount = 0;
    uint32_t IkCount = 0;
    unordered_map<string, vector<VmdKeyframe>> BoneTracks{};
    unordered_map<string, vector<VmdScalarKeyframe>> MorphTracks{};
    unordered_map<string, vector<VmdIkKeyframe>> IkTracks{};
    vector<VmdCameraKeyframe> CameraTrack{};
};

struct VmdTrackSampleCursor
{
    const void* Track = nullptr;
    size_t NextIndex = 0;
    float LastFrame = -1.0f;
};

bool LoadVmdAnimationFile(const char* fileName, VmdAnimation& outAnimation);
VmdCameraSample SampleVmdCameraTrack(const vector<VmdCameraKeyframe>& keys, float currentFrame);
float VmdToFrameTime(const VmdAnimation* animation, float timeSeconds);
void ResetVmdSampleCursor(VmdTrackSampleCursor& cursor);
void SampleVmdBoneTrack(const vector<VmdKeyframe>* keys, float currentFrame, aiQuaternion& outRotation,
                        aiVector3D& outPosition);
void SampleVmdBoneTrackCached(const vector<VmdKeyframe>* keys, float currentFrame, VmdTrackSampleCursor& cursor,
                              aiQuaternion& outRotation, aiVector3D& outPosition);
float SampleVmdMorphTrack(const vector<VmdScalarKeyframe>* keys, float currentFrame);
float SampleVmdMorphTrackCached(const vector<VmdScalarKeyframe>* keys, float currentFrame,
                                VmdTrackSampleCursor& cursor);
bool SampleVmdIkTrack(const vector<VmdIkKeyframe>* keys, float currentFrame);
bool SampleVmdIkTrackCached(const vector<VmdIkKeyframe>* keys, float currentFrame, VmdTrackSampleCursor& cursor);
