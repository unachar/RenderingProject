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
#include "vertexdeformation.h"
#include "meshshaderpipeline.h"
#include "raytracing.h"
#include "gpuscene/gpuscene.h"
#include "gpudriven/gpudrivenrenderer.h"
#include "gpudriven/gpudrivenvalidation.h"

void FramePipeline::Execute()
{
	ID3D12GraphicsCommandList* commandList = GraphicsDevice::GetCommandList();
	VertexDeformation::PrepareFrame(commandList, GraphicsDevice::GetFrameIndex());
	MeshShaderPipeline::BeginFrame();
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

			const auto gpuScene = graph.CreateLogicalResource("GPU Scene");
            graph.AddPass("GPU Scene", [gpuScene](RenderGraph::PassBuilder& builder) { builder.Write(gpuScene); },
                [](ID3D12GraphicsCommandList* list) { RenderProfiler::ScopedEvent profile("GPU Scene", list); GpuScene::Execute(list); });
            const auto geometry = graph.CreateLogicalResource("Geometry Buffers");
			const auto sceneDepth = graph.CreateLogicalResource("Scene Depth");
			const auto velocity = graph.CreateLogicalResource("Velocity");
			const auto sceneColor = graph.CreateLogicalResource("Scene Color");
			const auto editorScene = graph.CreateLogicalResource("Editor Scene");

			graph.AddPass(
				"Shadow",
				[shadowDepth, gpuScene](RenderGraph::PassBuilder& builder)
				{
					builder.Read(gpuScene).Write(shadowDepth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
				},
				[](ID3D12GraphicsCommandList* passCommandList)
				{
					RenderProfiler::ScopedEvent profile("Shadow", passCommandList);
					// Phase 3 GBV exercises the production GPU Scene/opaque passes.
					// Full-frame CPU fallback and auxiliary passes are tested separately.
					if (GPUDrivenValidation::IsGpuValidationRun()) return;
					if (Raytracing::UsesRaytracedShadows()) return;
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
				[shadowDepth, geometry, sceneDepth, gpuScene](RenderGraph::PassBuilder& builder)
				{
					builder.Read(gpuScene).Read(shadowDepth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
						.Write(geometry)
						.Write(sceneDepth);
				},
				[](ID3D12GraphicsCommandList* passCommandList)
				{
					RenderProfiler::ScopedEvent profile("GBuffer / Opaque", passCommandList);
					const auto submissionStart = std::chrono::steady_clock::now();
					ScenePasses::BeginScenePass();
					GPUDrivenRenderer::Draw(passCommandList);
					if (!GPUDrivenValidation::IsGpuValidationRun()) SystemManager::DrawSystem(RenderPass::PrimaryScene, false);
					if (ScenePasses::BuildOcclusionHierarchyAndBeginPhaseTwo())
					{
						SystemManager::DrawSystem(RenderPass::OcclusionPhase2, false);
					}
					GPUDrivenRenderer::BuildHiZ(passCommandList);
					ScenePasses::EndScenePass();
					GPUDrivenRenderer::RecordOpaqueSubmission(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - submissionStart).count());
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
					if (GPUDrivenValidation::IsGpuValidationRun()) return;
					ScenePasses::RenderVelocityBuffer();
					SystemManager::DrawSystem(RenderPass::Velocity, false);
					GPUDrivenRenderer::DrawVelocity(passCommandList);
					ScenePasses::EndVelocityBuffer();
				});

			graph.AddPass(
				"Deferred Lighting / PostProcess",
				[shadowDepth, geometry, sceneDepth, velocity, sceneColor, editorScene, gpuScene](
					RenderGraph::PassBuilder& builder)
				{
					builder.Read(gpuScene).Read(shadowDepth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
						.Read(geometry)
						.Read(sceneDepth)
						.Read(velocity)
						.Write(sceneColor)
						.Write(editorScene);
				},
				[](ID3D12GraphicsCommandList* passCommandList)
				{
					RenderProfiler::ScopedEvent profile("Deferred Lighting / PostProcess", passCommandList);
					if (GPUDrivenValidation::IsGpuValidationRun()) return;
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
					if (GPUDrivenValidation::IsGpuValidationRun()) return;
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
					if (GPUDrivenValidation::IsGpuValidationRun()) return;
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
