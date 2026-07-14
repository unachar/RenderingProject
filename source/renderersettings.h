#pragma once

#include <algorithm>
#include <cstdint>

enum class ShadowMapMethod : int
{
	ShadowMap = 0,
	VirtualShadowMap = 1
};

class RendererSettings
{
public:
	static ShadowMapMethod GetShadowMapMethod() { return s_ShadowMapMethod; }
	static void SetShadowMapMethod(ShadowMapMethod value) { SetValue(s_ShadowMapMethod, value); }

	static int GetVirtualClipmapLevels() { return s_VirtualClipmapLevels; }
	static void SetVirtualClipmapLevels(int value) { SetValue(s_VirtualClipmapLevels, std::clamp(value, 1, 4)); }
	static float GetVirtualFirstLevelRadius() { return s_VirtualFirstLevelRadius; }
	static void SetVirtualFirstLevelRadius(float value) { SetValue(s_VirtualFirstLevelRadius, std::clamp(value, 4.0f, 64.0f)); }
	static int GetShadowFilterRadius() { return s_ShadowFilterRadius; }
	static void SetShadowFilterRadius(int value) { SetValue(s_ShadowFilterRadius, std::clamp(value, 0, 3)); }
	static float GetShadowDepthBias() { return s_ShadowDepthBias; }
	static void SetShadowDepthBias(float value) { SetValue(s_ShadowDepthBias, std::clamp(value, 0.0f, 0.01f)); }
	static float GetShadowNormalBias() { return s_ShadowNormalBias; }
	static void SetShadowNormalBias(float value) { SetValue(s_ShadowNormalBias, std::clamp(value, 0.0f, 0.01f)); }
	static bool GetStabilizeVirtualClipmaps() { return s_StabilizeVirtualClipmaps; }
	static void SetStabilizeVirtualClipmaps(bool value) { SetValue(s_StabilizeVirtualClipmaps, value); }
	static bool GetCacheVirtualShadowPages() { return s_CacheVirtualShadowPages; }
	static void SetCacheVirtualShadowPages(bool value) { SetValue(s_CacheVirtualShadowPages, value); }
	static int GetShadowCascadeCount() { return s_ShadowCascadeCount; }
	static void SetShadowCascadeCount(int value) { SetValue(s_ShadowCascadeCount, std::clamp(value, 1, 4)); }
	static float GetShadowDistance() { return s_ShadowDistance; }
	static void SetShadowDistance(float value) { SetValue(s_ShadowDistance, std::clamp(value, 8.0f, 512.0f)); }
	static bool GetContactShadowsEnabled() { return s_ContactShadowsEnabled; }
	static void SetContactShadowsEnabled(bool value) { SetValue(s_ContactShadowsEnabled, value); }
	static float GetContactShadowLength() { return s_ContactShadowLength; }
	static void SetContactShadowLength(float value) { SetValue(s_ContactShadowLength, std::clamp(value, 0.05f, 5.0f)); }
	static int GetContactShadowSteps() { return s_ContactShadowSteps; }
	static void SetContactShadowSteps(int value) { SetValue(s_ContactShadowSteps, std::clamp(value, 4, 24)); }
	static int GetVirtualShadowDebugMode() { return s_VirtualShadowDebugMode; }
	static void SetVirtualShadowDebugMode(int value) { SetValue(s_VirtualShadowDebugMode, std::clamp(value, 0, 3)); }
	static bool GetDistanceFieldShadowsEnabled() { return s_DistanceFieldShadowsEnabled; }
	static void SetDistanceFieldShadowsEnabled(bool value) { SetValue(s_DistanceFieldShadowsEnabled, value); }
	static float GetDistanceFieldShadowDistance() { return s_DistanceFieldShadowDistance; }
	static void SetDistanceFieldShadowDistance(float value) { SetValue(s_DistanceFieldShadowDistance, std::clamp(value, 2.0f, 100.0f)); }
	static int GetDistanceFieldShadowSteps() { return s_DistanceFieldShadowSteps; }
	static void SetDistanceFieldShadowSteps(int value) { SetValue(s_DistanceFieldShadowSteps, std::clamp(value, 4, 24)); }
	static uint64_t GetRevision() { return s_Revision; }

	static void ResetShadowDefaults()
	{
		s_ShadowMapMethod = ShadowMapMethod::VirtualShadowMap;
		s_VirtualClipmapLevels = 4;
		s_VirtualFirstLevelRadius = 16.0f;
		s_ShadowFilterRadius = 1;
		s_ShadowDepthBias = 0.000008f;
		s_ShadowNormalBias = 0.00001f;
		s_StabilizeVirtualClipmaps = true;
		s_CacheVirtualShadowPages = true;
		s_ShadowCascadeCount = 4;
		s_ShadowDistance = 96.0f;
		s_ContactShadowsEnabled = true;
		s_ContactShadowLength = 0.65f;
		s_ContactShadowSteps = 16;
		s_VirtualShadowDebugMode = 0;
		s_DistanceFieldShadowsEnabled = false;
		s_DistanceFieldShadowDistance = 30.0f;
		s_DistanceFieldShadowSteps = 12;
		++s_Revision;
	}

private:
	template<typename T>
	static void SetValue(T& destination, const T& value)
	{
		if (destination != value)
		{
			destination = value;
			++s_Revision;
		}
	}

	inline static ShadowMapMethod s_ShadowMapMethod = ShadowMapMethod::VirtualShadowMap;
	inline static int s_VirtualClipmapLevels = 4;
	inline static float s_VirtualFirstLevelRadius = 16.0f;
	inline static int s_ShadowFilterRadius = 1;
	inline static float s_ShadowDepthBias = 0.000008f;
	inline static float s_ShadowNormalBias = 0.00001f;
	inline static bool s_StabilizeVirtualClipmaps = true;
	inline static bool s_CacheVirtualShadowPages = true;
	inline static int s_ShadowCascadeCount = 4;
	inline static float s_ShadowDistance = 96.0f;
	inline static bool s_ContactShadowsEnabled = true;
	inline static float s_ContactShadowLength = 0.65f;
	inline static int s_ContactShadowSteps = 16;
	inline static int s_VirtualShadowDebugMode = 0;
	inline static bool s_DistanceFieldShadowsEnabled = false;
	inline static float s_DistanceFieldShadowDistance = 30.0f;
	inline static int s_DistanceFieldShadowSteps = 12;
	inline static uint64_t s_Revision = 1;
};
