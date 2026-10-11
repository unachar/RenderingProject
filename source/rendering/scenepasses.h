#pragma once
#include "rendertargets.h"

class ScenePasses : protected RenderTargets
{
public:
	static void BeginPass(ID3D12RootSignature* rootSignature, D3D_PRIMITIVE_TOPOLOGY topology);
	static void BeginSpritePass();
	static void BeginModelPass();
	static void BeginLinePass();
	static bool BeginShadowPass(UINT shadowIndex);
	static void EndShadowPass();
	static void EndShadowPassBatch();
	static void BeginEditorSceneOverlayPass();
	static void PrepareTransparentSceneCopy();
	static void EndEditorSceneOverlayPass();
	static void BeginScenePass();
	static void EndScenePass();
	static bool BuildOcclusionHierarchyAndBeginPhaseTwo();
	static void RenderVelocityBuffer();
	static void EndVelocityBuffer();
};
