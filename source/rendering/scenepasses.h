#pragma once
#include "rendertargets.h"

class ScenePasses : protected RenderTargets
{
public:
	static void BeginPass(ID3D12RootSignature* rootSignature, D3D_PRIMITIVE_TOPOLOGY topology);
	static void BeginSpritePass();
	static void BeginModelPass();
	static void BeginLinePass();
    static bool PrepareStaticShadowCache();
    static void ReleaseStaticShadowCache();
    static bool RestoreStaticShadowDepth(UINT shadowIndex);
	static bool BeginShadowPass(UINT shadowIndex, bool preserveDepth = false, bool stationaryTarget = false);
    static void EndStaticShadowPass();
	static void EndShadowPass();
	static void EndShadowPassBatch();
	static void BeginEditorSceneOverlayPass();
    static void BeginHdrTransparentPass();
    static void EndHdrTransparentPass();
	static void EndEditorSceneOverlayPass();
	static void BeginScenePass();
	static void EndScenePass();
	static bool BuildOcclusionHierarchyAndBeginPhaseTwo();
	static void RenderVelocityBuffer();
	static void EndVelocityBuffer();
};
