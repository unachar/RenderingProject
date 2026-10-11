#pragma once
#include "rendererstate.h"
#include <string>


	bool RendererPathEndsWith(const string& value, const char* suffix);
	string ResolvePixelShaderPathForRenderMode(const string& psPath, RenderMode renderMode);
