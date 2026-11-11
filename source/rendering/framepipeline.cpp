#include "pch.h"
#include "../effects/niagarascene.h"
#include "transmittanceshadows.h"
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
#include "impostorrenderer.h"
#include "materialsystem.h"

void FramePipeline::Execute()
{
    ID3D12GraphicsCommandList* commandList = GraphicsDevice::GetCommandList();
    TransmittanceShadows::PrepareFrame();
    ScenePasses::PrepareStaticShadowCache();
    VertexDeformation::PrepareFrame(commandList, GraphicsDevice::GetFrameIndex());
    MeshShaderPipeline::BeginFrame();
    static RenderGraph graph;
    static bool graphReady = false;
    static ComPtr<ID3D12Resource> importedShadow;
    if (importedShadow.Get() != RenderTargets::GetShadowDepthResource())
    {
        graph.Reset();
        graphReady = false;
        importedShadow = RenderTargets::GetShadowDepthResource();
    }

    UINT sceneWidth = GraphicsDevice::GetSceneWidth();
    UINT sceneHeight = GraphicsDevice::GetSceneHeight();

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
            graph.AddPass(
                "GPU Scene",
                [gpuScene](RenderGraph::PassBuilder& builder) {
                    builder.Write(gpuScene);
                },
                [](ID3D12GraphicsCommandList* list) {
                    RenderProfiler::ScopedEvent profile("GPU Scene", list);
                    GpuScene::Execute(list);
                });

            // ========== Shadow Passes ==========
            graph.AddPass(
                "Shadow",
                [shadowDepth, gpuScene](RenderGraph::PassBuilder& builder) {
                    builder.Read(gpuScene).Write(shadowDepth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
                },
                [](ID3D12GraphicsCommandList* passCommandList) {
                    RenderProfiler::ScopedEvent profile("Shadow", passCommandList);
                    if (GPUDrivenValidation::IsGpuValidationRun()) return;
                    if (Raytracing::UsesRaytracedShadows()) return;
                    
                    const UINT shadowLightCount = LightingResources::GetShadowLightCount();
                    for (UINT shadowIndex = 0; shadowIndex < shadowLightCount; ++shadowIndex)
                    {
                        if (!LightingResources::ShouldRenderShadowPass(shadowIndex)) continue;
                        
                        if (LightingResources::UseStaticShadowCache(shadowIndex))
                        {
                            if (LightingResources::ShouldRenderStaticShadowPass(shadowIndex) &&
                                ScenePasses::BeginShadowPass(shadowIndex, false, true))
                            {
                                RenderProfiler::ScopedEvent stationary("Stationary Shadow Update", passCommandList);
                                LightingResources::SetShadowCasterSubset(LightingResources::ShadowCasterSubset::Stationary);
                                SystemManager::DrawSystem(RenderPass::ShadowMap, false);
                                ScenePasses::EndStaticShadowPass();
                            }
                            const bool restored = ScenePasses::RestoreStaticShadowDepth(shadowIndex);
                            if (ScenePasses::BeginShadowPass(shadowIndex, restored))
                            {
                                LightingResources::SetShadowCasterSubset(
                                    restored ? LightingResources::ShadowCasterSubset::Moving
                                             : LightingResources::ShadowCasterSubset::All);
                                SystemManager::DrawSystem(RenderPass::ShadowMap, false);
                                ScenePasses::EndShadowPass();
                            }
                            LightingResources::SetShadowCasterSubset(LightingResources::ShadowCasterSubset::All);
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

            const auto transmission = graph.CreateLogicalResource("Transparent Shadow Transmission");
            graph.AddPass(
                "Transparent Shadows",
                [shadowDepth, gpuScene, transmission](RenderGraph::PassBuilder& builder) {
                    builder.Read(gpuScene)
                        .Read(shadowDepth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
                        .Write(transmission);
                },
                [](ID3D12GraphicsCommandList* list) {
                    RenderProfiler::ScopedEvent profile("Transparent Shadows", list);
                    if (GPUDrivenValidation::IsGpuValidationRun() || !TransmittanceShadows::HasCasters()) return;
                    
                    for (UINT shadow = 0; shadow < LightingResources::GetShadowLightCount(); ++shadow)
                    {
                        if (!LightingResources::ShouldRenderShadowPass(shadow)) continue;
                        for (UINT peel = 0; peel < RendererState::g_kTRANSMITTANCE_SHADOW_LAYERS; ++peel)
                        {
                            if (!TransmittanceShadows::BeginPass(shadow, peel)) continue;
                            SystemManager::DrawSystem(RenderPass::TransmittanceShadow, false);
                            TransmittanceShadows::EndPass();
                        }
                    }
                    ScenePasses::EndShadowPassBatch();
                    LightingResources::SetCurrentShadowPassIndex(0);
                    LightingResources::UpdateShadowConstantBuffer();
                });

            // ========== GBuffer / Opaque Pass (Hybrid - Minimal GBuffer) ==========
            const auto gbufferBaseColorMetal = graph.CreateLogicalResource("GBuffer BaseColor+Metal");
            const auto gbufferNormalRoughness = graph.CreateLogicalResource("GBuffer Normal+Roughness");
            const auto gbufferMotionVector = graph.CreateLogicalResource("GBuffer MotionVector");
            const auto gbufferEmissionAO = graph.CreateLogicalResource("GBuffer Emission+AO");
            const auto sceneDepth = graph.CreateLogicalResource("Scene Depth");
            const auto sceneColor = graph.CreateLogicalResource("Scene Color (HDR)");
            const auto editorScene = graph.CreateLogicalResource("Editor Scene");

            graph.AddPass(
                "GBuffer / Opaque (Hybrid)",
                [shadowDepth, gbufferBaseColorMetal, gbufferNormalRoughness, 
                 gbufferMotionVector, gbufferEmissionAO, sceneDepth, gpuScene, transmission]
                (RenderGraph::PassBuilder& builder) {
                    builder.Read(transmission)
                        .Read(gpuScene)
                        .Read(shadowDepth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
                        .Write(gbufferBaseColorMetal)
                        .Write(gbufferNormalRoughness)
                        .Write(gbufferMotionVector)
                        .Write(gbufferEmissionAO)
                        .Write(sceneDepth);
                },
                [](ID3D12GraphicsCommandList* passCommandList) {
                    RenderProfiler::ScopedEvent profile("GBuffer / Opaque (Hybrid)", passCommandList);
                    const auto submissionStart = chrono::steady_clock::now();
                    if (!GPUDrivenValidation::IsGpuValidationRun())
                    {
                        RenderProfiler::ScopedEvent p("GBuffer: Impostor Bake", passCommandList);
                        ImpostorRenderer::ProcessPendingBakes(passCommandList);
                    }
                    
                    {
                        RenderProfiler::ScopedEvent p("GBuffer: Begin", passCommandList);
                        ScenePasses::BeginScenePass();
                    }
                    
                    {
                        RenderProfiler::ScopedEvent p("GBuffer: GPU Driven Draw", passCommandList);
                        GPUDrivenRenderer::Draw(passCommandList);
                    }
                    
                    if (!GPUDrivenValidation::IsGpuValidationRun())
                    {
                        RenderProfiler::ScopedEvent p("GBuffer: Systems (CPU path)", passCommandList);
                        SystemManager::DrawSystem(RenderPass::PrimaryScene, false);
                    }
                    
                    bool phaseTwo = false;
                    {
                        RenderProfiler::ScopedEvent p("GBuffer: Occlusion Hierarchy", passCommandList);
                        phaseTwo = ScenePasses::BuildOcclusionHierarchyAndBeginPhaseTwo();
                    }
                    if (phaseTwo)
                    {
                        RenderProfiler::ScopedEvent p("GBuffer: Systems Phase 2", passCommandList);
                        SystemManager::DrawSystem(RenderPass::OcclusionPhase2, false);
                    }
                    {
                        RenderProfiler::ScopedEvent p("GBuffer: GPU Driven Phase 2", passCommandList);
                        GPUDrivenRenderer::DrawSecondPass(passCommandList);
                    }
                    {
                        RenderProfiler::ScopedEvent p("GBuffer: Build HiZ", passCommandList);
                        GPUDrivenRenderer::BuildHiZ(passCommandList);
                    }
                    {
                        RenderProfiler::ScopedEvent p("GBuffer: End", passCommandList);
                        ScenePasses::EndScenePass();
                    }
                    
                    GPUDrivenRenderer::RecordOpaqueSubmission(
                        chrono::duration<double, milli>(chrono::steady_clock::now() - submissionStart).count());
                });

            // ========== Velocity Pass ==========
            graph.AddPass(
                "Velocity",
                [sceneDepth, gbufferMotionVector](RenderGraph::PassBuilder& builder) {
                    builder.Read(sceneDepth).Write(gbufferMotionVector);
                },
                [](ID3D12GraphicsCommandList* passCommandList) {
                    RenderProfiler::ScopedEvent profile("Velocity", passCommandList);
                    if (GPUDrivenValidation::IsGpuValidationRun()) return;
                    ScenePasses::RenderVelocityBuffer();
                    SystemManager::DrawSystem(RenderPass::Velocity, false);
                    GPUDrivenRenderer::DrawVelocity(passCommandList);
                    ScenePasses::EndVelocityBuffer();
                });

            // ========== Deferred Lighting Pass (Opaque only) ==========
            graph.AddPass(
                "Deferred Lighting (Opaque)",
                [shadowDepth, gbufferBaseColorMetal, gbufferNormalRoughness, 
                 gbufferMotionVector, gbufferEmissionAO, sceneDepth, sceneColor, gpuScene]
                (RenderGraph::PassBuilder& builder) {
                    builder.Read(gpuScene)
                        .Read(shadowDepth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
                        .Read(gbufferBaseColorMetal, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
                        .Read(gbufferNormalRoughness, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
                        .Read(gbufferMotionVector, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
                        .Read(gbufferEmissionAO, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
                        .Read(sceneDepth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
                        .Write(sceneColor);
                },
                [](ID3D12GraphicsCommandList* passCommandList) {
                    RenderProfiler::ScopedEvent profile("Deferred Lighting (Opaque)", passCommandList);
                    if (GPUDrivenValidation::IsGpuValidationRun()) return;
                    
                    ScenePasses::BeginDeferredLightingPass();
                    PostProcessSystem postProcess;
                    postProcess.Draw(RenderPass::PrimaryScene, false);
                    ScenePasses::EndDeferredLightingPass();
                });

            // ========== Post Process / AntiAliasing ==========
            graph.AddPass(
                "PostProcess / AntiAliasing",
                [sceneDepth, gbufferMotionVector, sceneColor, editorScene]
                (RenderGraph::PassBuilder& builder) {
                    builder.Read(sceneDepth)
                           .Read(gbufferMotionVector, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
                           .ReadWrite(sceneColor)
                           .ReadWrite(editorScene);
                },
                [](ID3D12GraphicsCommandList* passCommandList) {
                    RenderProfiler::ScopedEvent profile("PostProcess / AntiAliasing", passCommandList);
                    if (GPUDrivenValidation::IsGpuValidationRun()) return;
                    PostProcessPass::ApplyAntiAliasing();
                    PostProcessSystem postProcess;
                    postProcess.Draw(RenderPass::PrimaryScene, false);
                });
        }
    }

    graphReady = graph.Compile();
    if (!graphReady)
    {
        Debug::Log("ERROR: Frame RenderGraph compile failed: %s\n", graph.GetLastError().c_str());
        return;
    }

    if (!graph.Execute(commandList))
    {
        Debug::Log("ERROR: Frame RenderGraph execution failed: %s\n", graph.GetLastError().c_str());
    }
}