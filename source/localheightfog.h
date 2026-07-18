#pragma once

#include <DirectXMath.h>
#include <algorithm>
#include <vector>

enum class LocalFogShape : int
{
	Height = 0,
	Sphere = 1
};

struct LocalHeightFogVolume
{
	bool Enabled = true;
	LocalFogShape Shape = LocalFogShape::Height;
	DirectX::XMFLOAT3 Position = { 0.0f, 1.0f, 0.0f };
	float Radius = 8.0f;
	float HeightFalloff = 1.0f;
	float Density = 0.25f;
	DirectX::XMFLOAT3 Color = { 0.72f, 0.80f, 0.92f };
};

class LocalHeightFog
{
public:
	static constexpr size_t MaxVolumes = 16;
	static const std::vector<LocalHeightFogVolume>& GetVolumes() { return s_Volumes; }
	static std::vector<LocalHeightFogVolume>& GetMutableVolumes() { return s_Volumes; }
	static bool Add(LocalFogShape shape = LocalFogShape::Height)
	{
		if (s_Volumes.size() >= MaxVolumes) return false;
		LocalHeightFogVolume volume{};
		volume.Shape = shape;
		s_Volumes.push_back(volume);
		return true;
	}
	static void Remove(size_t index)
	{
		if (index < s_Volumes.size()) s_Volumes.erase(s_Volumes.begin() + index);
	}
	static void Clear() { s_Volumes.clear(); }

private:
	inline static std::vector<LocalHeightFogVolume> s_Volumes{};
};
