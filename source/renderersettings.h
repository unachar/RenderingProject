#pragma once

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
	Nis
};

enum class UpscaleQuality : int
{
	UltraQuality = 0,
	Quality,
	Balanced,
	Performance,
	Custom
};

class RendererSettings
{
public:
	static UpscaleMode GetUpscaleMode() { return s_UpscaleMode; }
	static void SetUpscaleMode(UpscaleMode value) { SetValue(s_UpscaleMode, value); }
	static UpscaleQuality GetUpscaleQuality() { return s_UpscaleQuality; }
	static void SetUpscaleQuality(UpscaleQuality value) { SetValue(s_UpscaleQuality, value); }
	static float GetFsrSharpness() { return s_FsrSharpness; }
	static void SetFsrSharpness(float value) { SetValue(s_FsrSharpness, std::clamp(value, 0.0f, 2.0f)); }
	static float GetNisSharpness() { return s_NisSharpness; }
	static void SetNisSharpness(float value) { SetValue(s_NisSharpness, std::clamp(value, 0.0f, 1.0f)); }
	static bool GetComputeGBufferEnabled() { return s_ComputeGBufferEnabled; }
	static void SetComputeGBufferEnabled(bool value) { SetValue(s_ComputeGBufferEnabled, value); }
	static bool GetSsaoEnabled() { return s_SsaoEnabled; }
	static void SetSsaoEnabled(bool value) { SetValue(s_SsaoEnabled, value); }
	static float GetSsaoRadius() { return s_SsaoRadius; }
	static void SetSsaoRadius(float value) { SetValue(s_SsaoRadius, std::clamp(value, 0.1f, 4.0f)); }
	static float GetSsaoPower() { return s_SsaoPower; }
	static void SetSsaoPower(float value) { SetValue(s_SsaoPower, std::clamp(value, 0.25f, 4.0f)); }
	static bool GetSsgiEnabled() { return s_SsgiEnabled; }
	static void SetSsgiEnabled(bool value) { SetValue(s_SsgiEnabled, value); }
	static float GetSsgiIntensity() { return s_SsgiIntensity; }
	static void SetSsgiIntensity(float value) { SetValue(s_SsgiIntensity, std::clamp(value, 0.0f, 3.0f)); }
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
	static void SetVirtualClipmapLevels(int value) { SetValue(s_VirtualClipmapLevels, std::clamp(value, 1, 4)); }
	static float GetVirtualFirstLevelRadius() { return s_VirtualFirstLevelRadius; }
	static void SetVirtualFirstLevelRadius(float value) { SetValue(s_VirtualFirstLevelRadius, std::clamp(value, 4.0f, 64.0f)); }
	static int GetShadowFilterRadius() { return s_ShadowFilterRadius; }
	static void SetShadowFilterRadius(int value) { SetValue(s_ShadowFilterRadius, std::clamp(value, 0, 3)); }
	static float GetShadowDepthBias()
	{
		if (s_ShadowMapMethod != ShadowMapMethod::VirtualShadowMap)
		{
			return s_ShadowDepthBias;
		}

		// The first resident VSM level covers 4 pages of 128 texels per axis.
		// Scale the minimum bias from that world-space texel size instead of using
		// a fixed near-zero depth value. This removes stable receiver-plane acne
		// bands while still allowing a larger authored bias through the UI.
		const float automaticBias = s_VirtualFirstLevelRadius / 256000.0f;
		return std::max(s_VirtualShadowDepthBias, automaticBias);
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
	static void SetConventionalShadowDepthBias(float value) { SetValue(s_ShadowDepthBias, std::clamp(value, 0.0f, 0.01f)); }
	static float GetAuthoredVirtualShadowDepthBias() { return s_VirtualShadowDepthBias; }
	static void SetVirtualShadowDepthBias(float value) { SetValue(s_VirtualShadowDepthBias, std::clamp(value, 0.0f, 0.01f)); }
	static float GetShadowNormalBias()
	{
		if (s_ShadowMapMethod != ShadowMapMethod::VirtualShadowMap)
		{
			return s_ShadowNormalBias;
		}

		// Keep the receiver at least 0.2 of a finest-level texel away from its
		// own shadow depth. The projection code converts this reference value to
		// the actual depth range of every clipmap level.
		const float automaticBias = s_VirtualFirstLevelRadius / 384000.0f;
		return std::max(s_VirtualShadowNormalBias, automaticBias);
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
	static void SetConventionalShadowNormalBias(float value) { SetValue(s_ShadowNormalBias, std::clamp(value, 0.0f, 0.01f)); }
	static float GetAuthoredVirtualShadowNormalBias() { return s_VirtualShadowNormalBias; }
	static void SetVirtualShadowNormalBias(float value) { SetValue(s_VirtualShadowNormalBias, std::clamp(value, 0.0f, 0.01f)); }
	static float GetShadowResolutionTransition() { return s_ShadowResolutionTransition; }
	static void SetShadowResolutionTransition(float value) { SetValue(s_ShadowResolutionTransition, std::clamp(value, 0.05f, 0.40f)); }
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
	static int GetScreenLightBudget() { return s_ScreenLightBudget; }
	static void SetScreenLightBudget(int value) { SetValue(s_ScreenLightBudget, std::clamp(value, 1, 32)); }
	static int GetTileLightBudget() { return s_TileLightBudget; }
	static void SetTileLightBudget(int value) { SetValue(s_TileLightBudget, std::clamp(value, 1, 4)); }
	static int GetDecalLightBudget() { return s_DecalLightBudget; }
	static void SetDecalLightBudget(int value) { SetValue(s_DecalLightBudget, std::clamp(value, 0, 110)); }
	static int GetDecalTileLightBudget() { return s_DecalTileLightBudget; }
	static void SetDecalTileLightBudget(int value) { SetValue(s_DecalTileLightBudget, std::clamp(value, 0, 4)); }
	static int GetVolumetricLightBudget() { return s_VolumetricLightBudget; }
	static void SetVolumetricLightBudget(int value) { SetValue(s_VolumetricLightBudget, std::clamp(value, 0, 5)); }
	static int GetShadowLightBudget() { return s_ShadowLightBudget; }
	static void SetShadowLightBudget(int value) { SetValue(s_ShadowLightBudget, std::clamp(value, 1, 8)); }
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

	inline static UpscaleMode s_UpscaleMode = UpscaleMode::Fsr1;
	inline static UpscaleQuality s_UpscaleQuality = UpscaleQuality::Performance;
	inline static float s_FsrSharpness = 0.2f;
	inline static float s_NisSharpness = 0.5f;
	inline static bool s_ComputeGBufferEnabled = true;
	inline static bool s_SsaoEnabled = true;
	inline static float s_SsaoRadius = 1.15f;
	inline static float s_SsaoPower = 1.35f;
	inline static bool s_SsgiEnabled = true;
	inline static float s_SsgiIntensity = 0.45f;
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
	inline static float s_ShadowDistance = 96.0f;
	inline static bool s_ContactShadowsEnabled = true;
	inline static float s_ContactShadowLength = 0.65f;
	inline static int s_ContactShadowSteps = 16;
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
