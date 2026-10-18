#pragma once
#include "materialasset.h"

#include <algorithm>
#include <cstdint>

enum class ShadowMapMethod : int
{
	ShadowMap = 0,
	VirtualShadowMap = 1
};

enum class UpscaleMode : int
{
	Bilateral = 0,
	Fsr1,
	Nis,
	Dlss,
    Tsr
};

enum class UpscaleQuality : int
{
	UltraQuality = 0,
	Quality,
	Balanced,
	Performance,
	Custom
};

enum class GlobalIlluminationMode : int { Off, ScreenSpace, Realtime };

class RendererSettings
{
public:
	static UpscaleMode GetUpscaleMode() { return s_UpscaleMode; }
	static void SetUpscaleMode(UpscaleMode value) { SetValue(s_UpscaleMode, value); }
	static UpscaleQuality GetUpscaleQuality() { return s_UpscaleQuality; }
	static void SetUpscaleQuality(UpscaleQuality value) { SetValue(s_UpscaleQuality, value); }
	static float GetFsrSharpness() { return s_FsrSharpness; }
	static void SetFsrSharpness(float value) { SetValue(s_FsrSharpness, clamp(value, 0.0f, 2.0f)); }
	static float GetNisSharpness() { return s_NisSharpness; }
	static void SetNisSharpness(float value) { SetValue(s_NisSharpness, clamp(value, 0.0f, 1.0f)); }
    // The packed visibility payload has no emission/AO channels. Use production raster
    // GBuffer generation when the scene contains material assets; keep the user's option.
	static bool GetComputeGBufferEnabled() { return s_ComputeGBufferEnabled && !SceneHasMaterialAssets(); }
	static bool GetAuthoredComputeGBufferEnabled() { return s_ComputeGBufferEnabled; }
	static void SetComputeGBufferEnabled(bool value) { SetValue(s_ComputeGBufferEnabled, value); }
	static bool GetSsaoEnabled() { return s_SsaoEnabled; }
	static void SetSsaoEnabled(bool value) { SetValue(s_SsaoEnabled, value); }
	static float GetSsaoRadius() { return s_SsaoRadius; }
	static void SetSsaoRadius(float value) { SetValue(s_SsaoRadius, clamp(value, 0.1f, 4.0f)); }
	static float GetSsaoPower() { return s_SsaoPower; }
	static void SetSsaoPower(float value) { SetValue(s_SsaoPower, clamp(value, 0.25f, 4.0f)); }
	static bool GetSsgiEnabled() { return s_GiMode == GlobalIlluminationMode::ScreenSpace; }
    static GlobalIlluminationMode GetGiMode() { return s_GiMode; }
    static void SetGiMode(GlobalIlluminationMode value) { SetValue(s_GiMode, value); }
    static bool GetRealtimeGiEnabled() { return s_GiMode == GlobalIlluminationMode::Realtime; }
    static float GetGiDistance() { return s_GiDistance; }
    static void SetGiDistance(float value) { SetValue(s_GiDistance, clamp(value, 1.0f, 200.0f)); }
	static void SetSsgiEnabled(bool value) { SetGiMode(value ? GlobalIlluminationMode::ScreenSpace : GlobalIlluminationMode::Off); }
	static float GetSsgiIntensity() { return s_SsgiIntensity; }
	static void SetSsgiIntensity(float value) { SetValue(s_SsgiIntensity, clamp(value, 0.0f, 3.0f)); }
	// Ray traced GI is a physically based irradiance estimate; it uses its own scale (1 = energy
	// conserving) instead of the screen-space GI strength, which was tuned for SSGI.
	static float GetRealtimeGiIntensity() { return s_RealtimeGiIntensity; }
	static void SetRealtimeGiIntensity(float value) { SetValue(s_RealtimeGiIntensity, clamp(value, 0.0f, 3.0f)); }
	// World-space irradiance cache for multi-bounce realtime GI.
	static bool GetGiRadianceCacheEnabled() { return s_GiRadianceCacheEnabled; }
	static void SetGiRadianceCacheEnabled(bool value) { SetValue(s_GiRadianceCacheEnabled, value); }
	static float GetGiRadianceCacheCellSize() { return s_GiRadianceCacheCellSize; }
	static void SetGiRadianceCacheCellSize(float value) { SetValue(s_GiRadianceCacheCellSize, clamp(value, 0.05f, 2.0f)); }
	static int GetGiRadianceCacheHistory() { return s_GiRadianceCacheHistory; }
	static void SetGiRadianceCacheHistory(int value) { SetValue(s_GiRadianceCacheHistory, clamp(value, 1, 128)); }
	static bool GetRayBinningEnabled() { return s_RayBinningEnabled; }
	static void SetRayBinningEnabled(bool value) { SetValue(s_RayBinningEnabled, value); }
	static bool GetTextureStreamingEnabled() { return s_TextureStreamingEnabled; }
	static void SetTextureStreamingEnabled(bool value) { SetValue(s_TextureStreamingEnabled, value); }
	static bool GetReservedResourcesEnabled() { return s_ReservedResourcesEnabled; }
	static void SetReservedResourcesEnabled(bool value) { SetValue(s_ReservedResourcesEnabled, value); }
	static bool GetMeshShadersEnabled() { return s_MeshShadersEnabled; }
	static void SetMeshShadersEnabled(bool value) { SetValue(s_MeshShadersEnabled, value); }
	static bool GetTwoPhaseOcclusionEnabled() { return s_TwoPhaseOcclusionEnabled; }
	static void SetTwoPhaseOcclusionEnabled(bool value) { SetValue(s_TwoPhaseOcclusionEnabled, value); }
	static float GetUpscaleQualityScale(UpscaleQuality quality)
	{
		switch (quality)
		{
		case UpscaleQuality::UltraQuality: return 1.0f / 1.3f;
		case UpscaleQuality::Quality: return 1.0f / 1.5f;
		case UpscaleQuality::Balanced: return 1.0f / 1.7f;
		case UpscaleQuality::Performance: return 1.0f / 2.0f;
		default: return 1.0f;
		}
	}

	static ShadowMapMethod GetShadowMapMethod() { return s_ShadowMapMethod; }
	static void SetShadowMapMethod(ShadowMapMethod value) { SetValue(s_ShadowMapMethod, value); }

	static int GetVirtualClipmapLevels() { return s_VirtualClipmapLevels; }
	static void SetVirtualClipmapLevels(int value) { SetValue(s_VirtualClipmapLevels, clamp(value, 1, 4)); }
	static float GetVirtualFirstLevelRadius() { return s_VirtualFirstLevelRadius; }
	static void SetVirtualFirstLevelRadius(float value) { SetValue(s_VirtualFirstLevelRadius, clamp(value, 4.0f, 64.0f)); }
	static int GetShadowFilterRadius() { return s_ShadowFilterRadius; }
	static void SetShadowFilterRadius(int value) { SetValue(s_ShadowFilterRadius, clamp(value, 0, 3)); }
	static float GetShadowDepthBias()
	{
		if (s_ShadowMapMethod != ShadowMapMethod::VirtualShadowMap)
		{
			return s_ShadowDepthBias;
		}


		const float automaticBias = s_VirtualFirstLevelRadius / 256000.0f;
		return max(s_VirtualShadowDepthBias, automaticBias);
	}
	static void SetShadowDepthBias(float value)
	{
		if (s_ShadowMapMethod == ShadowMapMethod::VirtualShadowMap)
		{
			SetVirtualShadowDepthBias(value);
		}
		else
		{
			SetConventionalShadowDepthBias(value);
		}
	}
	static float GetConventionalShadowDepthBias() { return s_ShadowDepthBias; }
	static void SetConventionalShadowDepthBias(float value) { SetValue(s_ShadowDepthBias, clamp(value, 0.0f, 0.01f)); }
	static float GetAuthoredVirtualShadowDepthBias() { return s_VirtualShadowDepthBias; }
	static void SetVirtualShadowDepthBias(float value) { SetValue(s_VirtualShadowDepthBias, clamp(value, 0.0f, 0.01f)); }
	static float GetShadowNormalBias()
	{
		if (s_ShadowMapMethod != ShadowMapMethod::VirtualShadowMap)
		{
			return s_ShadowNormalBias;
		}


		const float automaticBias = s_VirtualFirstLevelRadius / 384000.0f;
		return max(s_VirtualShadowNormalBias, automaticBias);
	}
	static void SetShadowNormalBias(float value)
	{
		if (s_ShadowMapMethod == ShadowMapMethod::VirtualShadowMap)
		{
			SetVirtualShadowNormalBias(value);
		}
		else
		{
			SetConventionalShadowNormalBias(value);
		}
	}
	static float GetConventionalShadowNormalBias() { return s_ShadowNormalBias; }
	static void SetConventionalShadowNormalBias(float value) { SetValue(s_ShadowNormalBias, clamp(value, 0.0f, 0.01f)); }
	static float GetAuthoredVirtualShadowNormalBias() { return s_VirtualShadowNormalBias; }
	static void SetVirtualShadowNormalBias(float value) { SetValue(s_VirtualShadowNormalBias, clamp(value, 0.0f, 0.01f)); }
	static float GetShadowResolutionTransition() { return s_ShadowResolutionTransition; }
	static void SetShadowResolutionTransition(float value) { SetValue(s_ShadowResolutionTransition, clamp(value, 0.05f, 0.40f)); }
	static bool GetStabilizeVirtualClipmaps() { return s_StabilizeVirtualClipmaps; }
	static void SetStabilizeVirtualClipmaps(bool value) { SetValue(s_StabilizeVirtualClipmaps, value); }
	static bool GetCacheVirtualShadowPages() { return s_CacheVirtualShadowPages; }
	static void SetCacheVirtualShadowPages(bool value) { SetValue(s_CacheVirtualShadowPages, value); }
	static int GetShadowCascadeCount() { return s_ShadowCascadeCount; }
	static void SetShadowCascadeCount(int value) { SetValue(s_ShadowCascadeCount, clamp(value, 1, 4)); }
	static float GetShadowDistance() { return s_ShadowDistance; }
	static void SetShadowDistance(float value) { SetValue(s_ShadowDistance, clamp(value, 8.0f, 512.0f)); }
	static bool GetContactShadowsEnabled() { return s_ContactShadowsEnabled; }
	static void SetContactShadowsEnabled(bool value) { SetValue(s_ContactShadowsEnabled, value); }
	static float GetContactShadowLength() { return s_ContactShadowLength; }
	static void SetContactShadowLength(float value) { SetValue(s_ContactShadowLength, clamp(value, 0.05f, 5.0f)); }
	static int GetContactShadowSteps() { return s_ContactShadowSteps; }
	static void SetContactShadowSteps(int value) { SetValue(s_ContactShadowSteps, clamp(value, 4, 24)); }
	static int GetVirtualShadowDebugMode() { return s_VirtualShadowDebugMode; }
	static void SetVirtualShadowDebugMode(int value) { SetValue(s_VirtualShadowDebugMode, clamp(value, 0, 3)); }
	static bool GetDistanceFieldShadowsEnabled() { return s_DistanceFieldShadowsEnabled; }
	static void SetDistanceFieldShadowsEnabled(bool value) { SetValue(s_DistanceFieldShadowsEnabled, value); }
	static float GetDistanceFieldShadowDistance() { return s_DistanceFieldShadowDistance; }
	static void SetDistanceFieldShadowDistance(float value) { SetValue(s_DistanceFieldShadowDistance, clamp(value, 2.0f, 100.0f)); }
	static int GetDistanceFieldShadowSteps() { return s_DistanceFieldShadowSteps; }
	static void SetDistanceFieldShadowSteps(int value) { SetValue(s_DistanceFieldShadowSteps, clamp(value, 4, 24)); }
	static int GetScreenLightBudget() { return s_ScreenLightBudget; }
	static void SetScreenLightBudget(int value) { SetValue(s_ScreenLightBudget, clamp(value, 1, 32)); }
	static int GetTileLightBudget() { return s_TileLightBudget; }
	static void SetTileLightBudget(int value) { SetValue(s_TileLightBudget, clamp(value, 1, 4)); }
	static int GetDecalLightBudget() { return s_DecalLightBudget; }
	static void SetDecalLightBudget(int value) { SetValue(s_DecalLightBudget, clamp(value, 0, 110)); }
	static int GetDecalTileLightBudget() { return s_DecalTileLightBudget; }
	static void SetDecalTileLightBudget(int value) { SetValue(s_DecalTileLightBudget, clamp(value, 0, 4)); }
	static int GetVolumetricLightBudget() { return s_VolumetricLightBudget; }
	static void SetVolumetricLightBudget(int value) { SetValue(s_VolumetricLightBudget, clamp(value, 0, 5)); }
	static int GetShadowLightBudget() { return s_ShadowLightBudget; }
	static void SetShadowLightBudget(int value) { SetValue(s_ShadowLightBudget, clamp(value, 1, 8)); }
	static uint64_t GetRevision() { return s_Revision; }

	static void ResetShadowDefaults()
	{
		s_ShadowMapMethod = ShadowMapMethod::VirtualShadowMap;
		s_VirtualClipmapLevels = 4;
		s_VirtualFirstLevelRadius = 16.0f;
		s_ShadowFilterRadius = 1;
		s_ShadowDepthBias = 0.000063f;
		s_ShadowNormalBias = 0.000042f;
		s_VirtualShadowDepthBias = 0.000063f;
		s_VirtualShadowNormalBias = 0.000042f;
		s_ShadowResolutionTransition = 0.20f;
		s_StabilizeVirtualClipmaps = true;
		s_CacheVirtualShadowPages = true;
		s_ShadowCascadeCount = 4;
		s_ShadowDistance = 64.0f;
		s_ContactShadowsEnabled = true;
		s_ContactShadowLength = 0.65f;
		s_ContactShadowSteps = 12;
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

	inline static UpscaleMode s_UpscaleMode = UpscaleMode::Fsr1;
	inline static UpscaleQuality s_UpscaleQuality = UpscaleQuality::Performance;
	inline static float s_FsrSharpness = 0.2f;
	inline static float s_NisSharpness = 0.5f;
	inline static bool s_ComputeGBufferEnabled = true;
	inline static bool s_SsaoEnabled = true;
	inline static float s_SsaoRadius = 1.15f;
	inline static float s_SsaoPower = 1.35f;
	inline static GlobalIlluminationMode s_GiMode = GlobalIlluminationMode::ScreenSpace;
    inline static float s_GiDistance = 40.0f;
	inline static float s_SsgiIntensity = 0.45f;
	inline static float s_RealtimeGiIntensity = 1.0f;
	inline static bool s_GiRadianceCacheEnabled = true;
	inline static float s_GiRadianceCacheCellSize = 0.25f;
	inline static int s_GiRadianceCacheHistory = 32;
	inline static bool s_RayBinningEnabled = true;
	inline static bool s_TextureStreamingEnabled = true;
	inline static bool s_ReservedResourcesEnabled = true;
	inline static bool s_MeshShadersEnabled = true;
	inline static bool s_TwoPhaseOcclusionEnabled = true;
	inline static ShadowMapMethod s_ShadowMapMethod = ShadowMapMethod::VirtualShadowMap;
	inline static int s_VirtualClipmapLevels = 4;
	inline static float s_VirtualFirstLevelRadius = 16.0f;
	inline static int s_ShadowFilterRadius = 1;
	inline static float s_ShadowDepthBias = 0.000063f;
	inline static float s_ShadowNormalBias = 0.000042f;
	inline static float s_VirtualShadowDepthBias = 0.000063f;
	inline static float s_VirtualShadowNormalBias = 0.000042f;
	inline static float s_ShadowResolutionTransition = 0.20f;
	inline static bool s_StabilizeVirtualClipmaps = true;
	inline static bool s_CacheVirtualShadowPages = true;
	inline static int s_ShadowCascadeCount = 4;
	inline static float s_ShadowDistance = 64.0f;
	inline static bool s_ContactShadowsEnabled = true;
	inline static float s_ContactShadowLength = 0.65f;
	inline static int s_ContactShadowSteps = 12;
	inline static int s_VirtualShadowDebugMode = 0;
	inline static bool s_DistanceFieldShadowsEnabled = false;
	inline static float s_DistanceFieldShadowDistance = 30.0f;
	inline static int s_DistanceFieldShadowSteps = 12;
	inline static int s_ScreenLightBudget = 32;
	inline static int s_TileLightBudget = 4;
	inline static int s_DecalLightBudget = 110;
	inline static int s_DecalTileLightBudget = 4;
	inline static int s_VolumetricLightBudget = 5;
	inline static int s_ShadowLightBudget = 4;
	inline static uint64_t s_Revision = 1;
};


