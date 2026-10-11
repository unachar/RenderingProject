#pragma once
#include "rendertargets.h"

class RenderFrame : protected RenderTargets
{
public:
	static void BeginDraw();
	static void BeginBackBufferPass();
	static void SetDescriptorHeap();
	static void EndDraw();
};
