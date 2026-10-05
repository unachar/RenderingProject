#pragma once
#include "rendererstate.h"

class RenderConfiguration : protected RendererState
{
public:
	static void Resize(UINT width, UINT height);
	static void SetRenderMode(RenderMode mode);
	static void ApplyPendingRenderMode();
	static void SetHdr(bool enabled);
	static void ApplyPendingHdr();
	static void SetResolutionScale(float scale);
	static void ApplyPendingResolutionScale();
	static void InvalidateScenePipelineCache();
	static RenderMode GetRenderMode() { return m_RenderMode; }
	static RenderMode GetRequestedRenderMode()
	{
		if (m_HasPendingRenderMode)
		{
			return m_PendingRenderMode;
		}
		return m_RenderMode;
	}
	static float GetResolutionScale()
	{
		if (m_HasPendingResolutionScale)
		{
			return m_PendingResolutionScale;
		}
		return m_ResolutionScale;
	}
	static int GetMonitorTextureIndex() { return m_MonitorTextureSrvIndex; }
	static void SetMonitorTextureIndex(int srvIndex) { m_MonitorTextureSrvIndex = srvIndex; }
};
