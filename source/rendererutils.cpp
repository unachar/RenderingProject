#include "pch.h"
#include "rendererutils.h"


	bool RendererPathEndsWith(const string& value, const char* suffix)
	{
		const size_t suffixLength = strlen(suffix);
		return value.size() >= suffixLength &&
			value.compare(value.size() - suffixLength, suffixLength, suffix) == 0;
	}

	string ResolvePixelShaderPathForRenderMode(const string& psPath, RenderMode renderMode)
	{
		if (renderMode != RenderMode::DEFERRED)
		{
			return psPath;
		}

		if (RendererPathEndsWith(psPath, "colorshaderPS.cso"))
		{
			return "shader/hlsl/build/colorshaderPS_MRT.cso";
		}
		if (RendererPathEndsWith(psPath, "colorshader3dPS.cso"))
		{
			return "shader/hlsl/build/colorshader3dPS_MRT.cso";
		}
		if (RendererPathEndsWith(psPath, "modelshaderPS.cso") ||
			RendererPathEndsWith(psPath, "modellightingPS.cso") ||
			RendererPathEndsWith(psPath, "modellighingspeculerPS.cso"))
		{
			return "shader/hlsl/build/DeferredPS.cso";
		}

		return psPath;
	}
