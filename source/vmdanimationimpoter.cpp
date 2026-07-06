#include "pch.h"
#include "vmdanimationimpoter.h"
#include "modelimportutils.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

using namespace std;

namespace
{
	string DecodeAsciiFixedString(const char* data, size_t length)
	{
		size_t byteCount = 0;
		while (byteCount < length && data[byteCount] != '\0')
		{
			++byteCount;
		}
		return string(data, byteCount);
	}

	string DecodeShiftJisFixedString(const char* data, size_t length)
	{
		size_t byteCount = 0;
		while (byteCount < length && data[byteCount] != '\0')
		{
			++byteCount;
		}
		if (byteCount == 0)
		{
			return {};
		}

		int wideLength = MultiByteToWideChar(932, MB_ERR_INVALID_CHARS, data, static_cast<int>(byteCount), nullptr, 0);
		if (wideLength <= 0)
		{
			wideLength = MultiByteToWideChar(932, 0, data, static_cast<int>(byteCount), nullptr, 0);
		}
		if (wideLength <= 0)
		{
			return string(data, byteCount);
		}

		wstring wideText(static_cast<size_t>(wideLength), L'\0');
		MultiByteToWideChar(932, 0, data, static_cast<int>(byteCount), wideText.data(), wideLength);

		const int utf8Length = WideCharToMultiByte(CP_UTF8, 0, wideText.data(), wideLength, nullptr, 0, nullptr, nullptr);
		if (utf8Length <= 0)
		{
			return string(data, byteCount);
		}

		string utf8Text(static_cast<size_t>(utf8Length), '\0');
		WideCharToMultiByte(CP_UTF8, 0, wideText.data(), wideLength, utf8Text.data(), utf8Length, nullptr, nullptr);
		return utf8Text;
	}

	template <typename T>
	bool ReadBinary(ifstream& stream, T& value)
	{
		return static_cast<bool>(stream.read(reinterpret_cast<char*>(&value), sizeof(T)));
	}

	template <size_t N>
	bool ReadBinaryArray(ifstream& stream, array<char, N>& value)
	{
		return static_cast<bool>(stream.read(value.data(), static_cast<streamsize>(value.size())));
	}

	uint64_t GetRemainingBytes(ifstream& stream)
	{
		const streampos current = stream.tellg();
		if (current < 0)
		{
			return 0;
		}

		stream.seekg(0, ios::end);
		const streampos end = stream.tellg();
		stream.seekg(current, ios::beg);
		if (end < current)
		{
			return 0;
		}
		return static_cast<uint64_t>(end - current);
	}

	bool ReadOptionalSectionCount(ifstream& stream, uint32_t& count, const char* sectionName, const char* fileName)
	{
		const uint64_t remainingBytes = GetRemainingBytes(stream);
		if (remainingBytes == 0)
		{
			count = 0;
			return true;
		}
		if (remainingBytes < sizeof(uint32_t))
		{
			Debug::Log("ERROR: VMD %s count is truncated: %s\n", sectionName, fileName);
			return false;
		}
		return ReadBinary(stream, count);
	}

	bool SkipBinaryBytes(ifstream& stream, uint64_t byteCount, const char* sectionName, const char* fileName)
	{
		if (GetRemainingBytes(stream) < byteCount)
		{
			Debug::Log("ERROR: VMD %s data is truncated: %s\n", sectionName, fileName);
			return false;
		}

		stream.seekg(static_cast<streamoff>(byteCount), ios::cur);
		return static_cast<bool>(stream);
	}

	float NormalizeVmdInterpolationByte(unsigned char value)
	{
		return clamp(static_cast<float>(value) / 127.0f, 0.0f, 1.0f);
	}

	float GetYFromXOnBezier(float x, const XMFLOAT2& p1, const XMFLOAT2& p2, uint8_t iterationCount)
	{
		x = clamp(x, 0.0f, 1.0f);
		if (fabsf(p1.x - p1.y) < 0.0001f && fabsf(p2.x - p2.y) < 0.0001f)
		{
			return x;
		}

		float t = x;
		const float k0 = 1.0f + 3.0f * p1.x - 3.0f * p2.x;
		const float k1 = 3.0f * p2.x - 6.0f * p1.x;
		const float k2 = 3.0f * p1.x;

		for (uint8_t i = 0; i < iterationCount; ++i)
		{
			const float ft = k0 * t * t * t + k1 * t * t + k2 * t - x;
			if (fabsf(ft) <= 0.00001f)
			{
				break;
			}
			t = clamp(t - ft / 2.0f, 0.0f, 1.0f);
		}

		const float r = 1.0f - t;
		return t * t * t + 3.0f * t * t * r * p2.y + 3.0f * t * r * r * p1.y;
	}

	aiVector3D ConvertVmdPositionToAssimpLeftHanded(const aiVector3D& position)
	{
		return aiVector3D(position.x, position.y, position.z);
	}

	aiQuaternion ConvertVmdRotationToAssimpLeftHanded(aiQuaternion rotation)
	{
		rotation.Normalize();
		return rotation;
	}

	template <typename Key, typename Less>
	void SortUniqueTrack(vector<Key>& keys, Less less)
	{
		sort(keys.begin(), keys.end(), less);

		vector<Key> uniqueKeys;
		uniqueKeys.reserve(keys.size());
		for (const Key& key : keys)
		{
			if (!uniqueKeys.empty() && uniqueKeys.back().Frame == key.Frame)
			{
				uniqueKeys.back() = key;
			}
			else
			{
				uniqueKeys.push_back(key);
			}
		}
		keys.swap(uniqueKeys);
	}
}

bool VmdAnimationImporter::Load(const char* fileName, VmdAnimation& outAnimation)
{
	const filesystem::path animPath = ModelImportUtils::FromUtf8(fileName);
	ifstream stream(animPath, ios::binary);
	if (!stream)
	{
		Debug::Log("ERROR: Failed to open VMD animation: %s\n", fileName);
		return false;
	}

	array<char, 30> header{};
	array<char, 20> modelName{};
	if (!ReadBinaryArray(stream, header) || !ReadBinaryArray(stream, modelName))
	{
		Debug::Log("ERROR: VMD header is too short: %s\n", fileName);
		return false;
	}

	const string headerText = DecodeAsciiFixedString(header.data(), header.size());
	if (headerText.rfind("Vocaloid Motion Data", 0) != 0)
	{
		Debug::Log("ERROR: Invalid VMD header: %s (header='%s')\n", fileName, headerText.c_str());
		return false;
	}

	VmdAnimation animation{};
	animation.ModelName = DecodeShiftJisFixedString(modelName.data(), modelName.size());
	if (!ReadBinary(stream, animation.MotionCount))
	{
		Debug::Log("ERROR: VMD motion count is missing: %s\n", fileName);
		return false;
	}

	for (uint32_t i = 0; i < animation.MotionCount; ++i)
	{
		array<char, 15> boneNameBytes{};
		uint32_t frame = 0;
		float px = 0.0f;
		float py = 0.0f;
		float pz = 0.0f;
		float qx = 0.0f;
		float qy = 0.0f;
		float qz = 0.0f;
		float qw = 1.0f;
		array<char, 64> interpolation{};

		if (!ReadBinaryArray(stream, boneNameBytes) ||
			!ReadBinary(stream, frame) ||
			!ReadBinary(stream, px) ||
			!ReadBinary(stream, py) ||
			!ReadBinary(stream, pz) ||
			!ReadBinary(stream, qx) ||
			!ReadBinary(stream, qy) ||
			!ReadBinary(stream, qz) ||
			!ReadBinary(stream, qw) ||
			!ReadBinaryArray(stream, interpolation))
		{
			Debug::Log("ERROR: VMD motion data is truncated: %s (motion=%u/%u)\n",
				fileName, i, animation.MotionCount);
			return false;
		}

		const string boneName = DecodeShiftJisFixedString(boneNameBytes.data(), boneNameBytes.size());
		if (boneName.empty())
		{
			continue;
		}

		VmdKeyframe keyframe{};
		keyframe.Frame = frame;
		keyframe.Position = ConvertVmdPositionToAssimpLeftHanded(aiVector3D(px, py, pz));
		keyframe.Rotation = ConvertVmdRotationToAssimpLeftHanded(aiQuaternion(qw, qx, qy, qz));
		keyframe.XP1 = XMFLOAT2(
			NormalizeVmdInterpolationByte(static_cast<unsigned char>(interpolation[0])),
			NormalizeVmdInterpolationByte(static_cast<unsigned char>(interpolation[4])));
		keyframe.XP2 = XMFLOAT2(
			NormalizeVmdInterpolationByte(static_cast<unsigned char>(interpolation[8])),
			NormalizeVmdInterpolationByte(static_cast<unsigned char>(interpolation[12])));
		keyframe.YP1 = XMFLOAT2(
			NormalizeVmdInterpolationByte(static_cast<unsigned char>(interpolation[1])),
			NormalizeVmdInterpolationByte(static_cast<unsigned char>(interpolation[5])));
		keyframe.YP2 = XMFLOAT2(
			NormalizeVmdInterpolationByte(static_cast<unsigned char>(interpolation[9])),
			NormalizeVmdInterpolationByte(static_cast<unsigned char>(interpolation[13])));
		keyframe.ZP1 = XMFLOAT2(
			NormalizeVmdInterpolationByte(static_cast<unsigned char>(interpolation[2])),
			NormalizeVmdInterpolationByte(static_cast<unsigned char>(interpolation[6])));
		keyframe.ZP2 = XMFLOAT2(
			NormalizeVmdInterpolationByte(static_cast<unsigned char>(interpolation[10])),
			NormalizeVmdInterpolationByte(static_cast<unsigned char>(interpolation[14])));
		keyframe.RotP1 = XMFLOAT2(
			NormalizeVmdInterpolationByte(static_cast<unsigned char>(interpolation[3])),
			NormalizeVmdInterpolationByte(static_cast<unsigned char>(interpolation[7])));
		keyframe.RotP2 = XMFLOAT2(
			NormalizeVmdInterpolationByte(static_cast<unsigned char>(interpolation[11])),
			NormalizeVmdInterpolationByte(static_cast<unsigned char>(interpolation[15])));
		animation.BoneTracks[boneName].push_back(keyframe);
		animation.MaxFrame = max(animation.MaxFrame, frame);
	}

	if (!ReadOptionalSectionCount(stream, animation.MorphCount, "morph", fileName))
	{
		return false;
	}
	for (uint32_t i = 0; i < animation.MorphCount; ++i)
	{
		array<char, 15> morphNameBytes{};
		uint32_t frame = 0;
		float weight = 0.0f;
		if (!ReadBinaryArray(stream, morphNameBytes) ||
			!ReadBinary(stream, frame) ||
			!ReadBinary(stream, weight))
		{
			Debug::Log("ERROR: VMD morph data is truncated: %s (morph=%u/%u)\n",
				fileName, i, animation.MorphCount);
			return false;
		}

		const string morphName = DecodeShiftJisFixedString(morphNameBytes.data(), morphNameBytes.size());
		if (!morphName.empty())
		{
			animation.MorphTracks[morphName].push_back({ frame, weight });
			animation.MaxFrame = max(animation.MaxFrame, frame);
		}
	}

	if (!ReadOptionalSectionCount(stream, animation.CameraCount, "camera", fileName) ||
		!SkipBinaryBytes(stream, static_cast<uint64_t>(animation.CameraCount) * 61u, "camera", fileName))
	{
		return false;
	}

	if (!ReadOptionalSectionCount(stream, animation.LightCount, "light", fileName) ||
		!SkipBinaryBytes(stream, static_cast<uint64_t>(animation.LightCount) * 28u, "light", fileName))
	{
		return false;
	}

	if (!ReadOptionalSectionCount(stream, animation.ShadowCount, "shadow", fileName) ||
		!SkipBinaryBytes(stream, static_cast<uint64_t>(animation.ShadowCount) * 9u, "shadow", fileName))
	{
		return false;
	}

	if (!ReadOptionalSectionCount(stream, animation.IkCount, "IK", fileName))
	{
		return false;
	}
	for (uint32_t i = 0; i < animation.IkCount; ++i)
	{
		uint32_t frame = 0;
		unsigned char show = 0;
		uint32_t ikInfoCount = 0;
		if (!ReadBinary(stream, frame) ||
			!ReadBinary(stream, show) ||
			!ReadBinary(stream, ikInfoCount))
		{
			Debug::Log("ERROR: VMD IK data is truncated: %s (ik=%u/%u)\n",
				fileName, i, animation.IkCount);
			return false;
		}

		for (uint32_t info = 0; info < ikInfoCount; ++info)
		{
			array<char, 20> ikNameBytes{};
			unsigned char enable = 0;
			if (!ReadBinaryArray(stream, ikNameBytes) || !ReadBinary(stream, enable))
			{
				Debug::Log("ERROR: VMD IK info is truncated: %s (ik=%u/%u, info=%u/%u)\n",
					fileName, i, animation.IkCount, info, ikInfoCount);
				return false;
			}

			const string ikName = DecodeShiftJisFixedString(ikNameBytes.data(), ikNameBytes.size());
			if (!ikName.empty())
			{
				animation.IkTracks[ikName].push_back({ frame, enable != 0 });
				animation.MaxFrame = max(animation.MaxFrame, frame);
			}
		}
	}

	for (auto& pair : animation.BoneTracks)
	{
		SortUniqueTrack(pair.second, [](const VmdKeyframe& a, const VmdKeyframe& b)
			{
				return a.Frame < b.Frame;
			});
	}

	for (auto& pair : animation.MorphTracks)
	{
		SortUniqueTrack(pair.second, [](const VmdScalarKeyframe& a, const VmdScalarKeyframe& b)
			{
				return a.Frame < b.Frame;
			});
	}

	for (auto& pair : animation.IkTracks)
	{
		SortUniqueTrack(pair.second, [](const VmdIkKeyframe& a, const VmdIkKeyframe& b)
			{
				return a.Frame < b.Frame;
			});
	}

	outAnimation = std::move(animation);
	return true;
}

float VmdAnimationImporter::ToFrameTime(const VmdAnimation* animation, float timeSeconds)
{
	float currentFrame = max(0.0f, timeSeconds) * 30.0f;
	if (animation && animation->MaxFrame > 0)
	{
		currentFrame = fmod(currentFrame, static_cast<float>(animation->MaxFrame) + 1.0f);
	}
	return currentFrame;
}

void VmdAnimationImporter::SampleBoneTrack(const vector<VmdKeyframe>* keys, float currentFrame,
	aiQuaternion& outRotation, aiVector3D& outPosition)
{
	outRotation = aiQuaternion(1.0f, 0.0f, 0.0f, 0.0f);
	outPosition = aiVector3D(0.0f, 0.0f, 0.0f);

	if (!keys || keys->empty())
	{
		return;
	}

	auto nextIt = lower_bound(keys->begin(), keys->end(), currentFrame,
		[](const VmdKeyframe& key, float frame)
		{
			return static_cast<float>(key.Frame) < frame;
		});

	if (nextIt == keys->begin())
	{
		outRotation = nextIt->Rotation;
		outPosition = nextIt->Position;
		return;
	}

	if (nextIt == keys->end())
	{
		outRotation = keys->back().Rotation;
		outPosition = keys->back().Position;
		return;
	}

	const VmdKeyframe& next = *nextIt;
	const VmdKeyframe& prev = *(nextIt - 1);
	if (next.Frame == prev.Frame)
	{
		outRotation = next.Rotation;
		outPosition = next.Position;
		return;
	}

	const float linearFactor = clamp(
		(currentFrame - static_cast<float>(prev.Frame)) / static_cast<float>(next.Frame - prev.Frame),
		0.0f, 1.0f);
	const float xFactor = GetYFromXOnBezier(linearFactor, next.XP1, next.XP2, 12);
	const float yFactor = GetYFromXOnBezier(linearFactor, next.YP1, next.YP2, 12);
	const float zFactor = GetYFromXOnBezier(linearFactor, next.ZP1, next.ZP2, 12);
	const float rotFactor = GetYFromXOnBezier(linearFactor, next.RotP1, next.RotP2, 12);
	outPosition.x = prev.Position.x * (1.0f - xFactor) + next.Position.x * xFactor;
	outPosition.y = prev.Position.y * (1.0f - yFactor) + next.Position.y * yFactor;
	outPosition.z = prev.Position.z * (1.0f - zFactor) + next.Position.z * zFactor;
	aiQuaternion::Interpolate(outRotation, prev.Rotation, next.Rotation, rotFactor);
	outRotation.Normalize();
}

float VmdAnimationImporter::SampleMorphTrack(const vector<VmdScalarKeyframe>* keys, float currentFrame)
{
	if (!keys || keys->empty())
	{
		return 0.0f;
	}

	auto nextIt = lower_bound(keys->begin(), keys->end(), currentFrame,
		[](const VmdScalarKeyframe& key, float frame)
		{
			return static_cast<float>(key.Frame) < frame;
		});
	if (nextIt == keys->begin())
	{
		return nextIt->Value;
	}
	if (nextIt == keys->end())
	{
		return keys->back().Value;
	}

	const VmdScalarKeyframe& next = *nextIt;
	const VmdScalarKeyframe& prev = *(nextIt - 1);
	if (next.Frame == prev.Frame)
	{
		return next.Value;
	}

	const float factor = clamp(
		(currentFrame - static_cast<float>(prev.Frame)) / static_cast<float>(next.Frame - prev.Frame),
		0.0f, 1.0f);
	return prev.Value * (1.0f - factor) + next.Value * factor;
}

bool VmdAnimationImporter::SampleIkTrack(const vector<VmdIkKeyframe>* keys, float currentFrame)
{
	if (!keys || keys->empty())
	{
		return true;
	}

	auto nextIt = upper_bound(keys->begin(), keys->end(), currentFrame,
		[](float frame, const VmdIkKeyframe& key)
		{
			return frame < static_cast<float>(key.Frame);
		});
	if (nextIt == keys->begin())
	{
		return keys->front().Enable;
	}
	return (nextIt - 1)->Enable;
}
