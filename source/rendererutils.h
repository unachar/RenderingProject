#pragma once
#include "rendererstate.h"
#include <string>

namespace RendererUtils
{
	bool EndsWith(const string& value, const char* suffix);
	string ResolvePixelShaderPathForRenderMode(const string& psPath, RenderMode renderMode);
}
