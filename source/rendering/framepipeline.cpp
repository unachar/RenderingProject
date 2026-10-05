#include "pch.h"
#include "framepipeline.h"
#include "systemmanager.h"
#include "rendergraph.h"
#include "graphicsdevice.h"
#include "postprocesssystem.h"
#include "rendertargets.h"
#include "scenepasses.h"
#include "postprocesspass.h"
#include "lightingresources.h"

void FramePipeline::Execute()
{
	ID3D12GraphicsCommandList* commandList = GraphicsDevice::GetCommandList();
	static RenderGraph graph;
	static bool graphReady = false;

	if (!graphReady)
	{
		if (graph.GetPassCount() == 0)
		{
			RenderGraph::ImportedResource shadowDepthResource{};
			shadowDepthResource.Name = "Shadow Depth";
			shadowDepthResource.Resource = RenderTargets::GetShadowDepthResource();
			shadowDepthResource.InitialState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			shadowDepthResource.FinalState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			shadowDepthResource.HasFinalState = true;
			const auto shadowDepth = graph.ImportResource(shadowDepthResource);

			const auto geometry = graph.CreateLogicalResource("Geometry Buffers");
			const auto sceneDepth = graph.CreateLogicalResource("Scene Depth");
			const auto velocity = graph.CreateLogicalResource("Velocity");
			const auto sceneColor = graph.CreateLogicalResource("Scene Color");
			const auto editorScene = graph.CreateLogicalResource("Editor Scene");

			graph.AddPass(
				"Shadow",
				[shadowDepth](RenderGraph::PassBuilder& builder)
				{
					builder.Write(shadowDepth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
				},
				[](ID3D12GraphicsCommandList* passCommandList)
				{
					RenderProfiler::ScopedEvent profile("Shadow", passCommandList);
					const UINT shadowLightCount = LightingResources::GetShadowLightCount();
					for (UINT shadowIndex = 0; shadowIndex < shadowLightCount; ++shadowIndex)
					{
						if (!LightingResources::ShouldRenderShadowPass(shadowIndex))
						{
							continue;
						}
						if (ScenePasses::BeginShadowPass(shadowIndex))
						{
							SystemManager::DrawSystem(RenderPass::ShadowMap, false);
							ScenePasses::EndShadowPass();
						}
					}
					ScenePasses::EndShadowPassBatch();
					if (shadowLightCount > 0)
					{
						LightingResources::SetCurrentShadowPassIndex(0);
						LightingResources::UpdateShadowConstantBuffer();
					}
				});

			graph.AddPass(
				"GBuffer / Opaque",
				[shadowDepth, geometry, sceneDepth](RenderGraph::PassBuilder& builder)
				{
					builder.Read(shadowDepth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
						.Write(geometry)
						.Write(sceneDepth);
				},
				[](ID3D12GraphicsCommandList* passCommandList)
				{
					RenderProfiler::ScopedEvent profile("GBuffer / Opaque", passCommandList);
					ScenePasses::BeginScenePass();
					SystemManager::DrawSystem(RenderPass::PrimaryScene, false);
					if (ScenePasses::BuildOcclusionHierarchyAndBeginPhaseTwo())
					{
						SystemManager::DrawSystem(RenderPass::OcclusionPhase2, false);
					}
					ScenePasses::EndScenePass();
				});

			graph.AddPass(
				"Velocity",
				[sceneDepth, velocity](RenderGraph::PassBuilder& builder)
				{
					builder.Read(sceneDepth).Write(velocity);
				},
				[](ID3D12GraphicsCommandList* passCommandList)
				{
					RenderProfiler::ScopedEvent profile("Velocity", passCommandList);
					ScenePasses::RenderVelocityBuffer();
					SystemManager::DrawSystem(RenderPass::Velocity, false);
					ScenePasses::EndVelocityBuffer();
				});

			graph.AddPass(
				"Deferred Lighting / PostProcess",
				[shadowDepth, geometry, sceneDepth, velocity, sceneColor, editorScene](
					RenderGraph::PassBuilder& builder)
				{
					builder.Read(shadowDepth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
						.Read(geometry)
						.Read(sceneDepth)
						.Read(velocity)
						.Write(sceneColor)
						.Write(editorScene);
				},
				[](ID3D12GraphicsCommandList* passCommandList)
				{
					RenderProfiler::ScopedEvent profile("Deferred Lighting / PostProcess", passCommandList);
					PostProcessSystem postProcess;
					postProcess.Draw(RenderPass::PrimaryScene, false);
				});

			graph.AddPass(
				"Transparent / Overlay",
				[sceneColor, editorScene](RenderGraph::PassBuilder& builder)
				{
					builder.Read(sceneColor).ReadWrite(editorScene);
				},
				[](ID3D12GraphicsCommandList* passCommandList)
				{
					RenderProfiler::ScopedEvent profile("Transparent / Overlay", passCommandList);
					ScenePasses::PrepareTransparentSceneCopy();
					ScenePasses::BeginEditorSceneOverlayPass();
					SystemManager::DrawSystem(RenderPass::OverlayScene, false);
					ScenePasses::EndEditorSceneOverlayPass();
				});

			graph.AddPass(
				"AntiAliasing",
				[sceneDepth, velocity, editorScene](RenderGraph::PassBuilder& builder)
				{
					builder.Read(sceneDepth).Read(velocity).ReadWrite(editorScene);
				},
				[](ID3D12GraphicsCommandList* passCommandList)
				{
					RenderProfiler::ScopedEvent profile("AntiAliasing", passCommandList);
					PostProcessPass::ApplyAntiAliasing();
				});
		}

		graphReady = graph.Compile();
		if (!graphReady)
		{
			Debug::Log("ERROR: Frame RenderGraph compile failed: %s\n", graph.GetLastError().c_str());
			return;
		}
	}

	if (!graph.Execute(commandList))
	{
		Debug::Log("ERROR: Frame RenderGraph execution failed: %s\n", graph.GetLastError().c_str());
	}
}
