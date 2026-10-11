#pragma once
#include "rendertargets.h"

class PostProcessPass : protected RenderTargets
{
public:
	static void UpdateCameraConstants();
	static void ApplyPostProcess(const PostProcessComponent& config);
	static void ApplyAntiAliasing();
};
