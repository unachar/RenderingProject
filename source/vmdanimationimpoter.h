#pragma once

#include "main.h"
#include <array>
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
	XMFLOAT2 XP1{0,0}, XP2{1,1};
	XMFLOAT2 YP1{0,0}, YP2{1,1};
	XMFLOAT2 ZP1{0,0}, ZP2{1,1};
	XMFLOAT2 RotP1{0,0}, RotP2{1,1};
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

struct VmdAnimation
{
	std::string ModelName{};
	uint32_t MaxFrame = 0;
	uint32_t MotionCount = 0;
	uint32_t MorphCount = 0;
	uint32_t CameraCount = 0;
	uint32_t LightCount = 0;
	uint32_t ShadowCount = 0;
	uint32_t IkCount = 0;
	std::unordered_map<std::string, std::vector<VmdKeyframe>> BoneTracks{};
	std::unordered_map<std::string, std::vector<VmdScalarKeyframe>> MorphTracks{};
	std::unordered_map<std::string, std::vector<VmdIkKeyframe>> IkTracks{};
};

namespace VmdAnimationImporter
{
	bool Load(const char* fileName, VmdAnimation& outAnimation);
	float ToFrameTime(const VmdAnimation* animation, float timeSeconds);
	void SampleBoneTrack(const std::vector<VmdKeyframe>* keys, float currentFrame,
		aiQuaternion& outRotation, aiVector3D& outPosition);
	float SampleMorphTrack(const std::vector<VmdScalarKeyframe>* keys, float currentFrame);
	bool SampleIkTrack(const std::vector<VmdIkKeyframe>* keys, float currentFrame);
}
