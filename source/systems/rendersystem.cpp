#include "pch.h"
#include "noisecachebindings.h"
#include "transmittanceshadows.h"
#include "materialmanager.h"
#include "vertexdeformation.h"
#include "meshshaderpipeline.h"
#include "rendersystem.h"
#include "graphicsdevice.h"
#include "shaderpipelines.h"
#include "psomanager.h"
#include "componentmanager.h"
#include "texturemanager.h"
#include "systemmanager.h"
#include "materialsystem.h"
#include "camera.h"
#include "imguimanager.h"
#include "instancingsystem.h"
#include <vector>
#include <algorithm>
#include "world.h"
#include "shaderdescription.h"
#include "scenepasses.h"
#include "materialbindings.h"
#include "frameconstants.h"
#include "lightingresources.h"
#include "renderconfiguration.h"
#include "gpudriven/gpudrivenrenderer.h"
#include "meshshader/meshshaderrenderer.h"
#include "cpuculling.h"
#include "gpuobjectculling.h"
#include "renderprofiler.h"
#include <optional>
#include <chrono>
#include "graphicslog.h"

static const MaterialComponent& DefaultMaterial()
{
    static const MaterialComponent material{};
    return material;
}

static bool IsSkyEntity(EntityID entity)
{
    return ComponentManager::HasComponent<NameComponent>(entity) &&
           ComponentManager::GetComponentUnchecked<NameComponent>(entity).Name == "Sky";
}

static bool ShouldCastShadow(EntityID entity, bool transmission = false)
{
    // 環境メッシュは背景放射輝度を供給するため、太陽を遮蔽させない。
    if (ComponentManager::HasComponent<MeshComponent>(entity) &&
        ComponentManager::GetComponentUnchecked<MeshComponent>(entity).IsEnvironment)
    {
        return false;
    }

    if (IsSkyEntity(entity) || ComponentManager::HasComponent<LightComponent>(entity))
    {
        return false;
    }
    if (!Registry::HasComponent(entity, ComponentType::MATERIAL))
    {
        return true;
    }

    const auto& material = MaterialManager::Resolve(ComponentManager::GetComponentUnchecked<MaterialComponent>(entity));
    const auto& source = ComponentManager::GetComponentUnchecked<MaterialComponent>(entity);
    for (size_t slot = 0; slot < source.MaterialSlots.size(); ++slot)
    {
        if (MaterialSystem::CastsShadowInPass(MaterialManager::Resolve(source, slot), transmission))
        {
            return true;
        }
    }
    return MaterialSystem::CastsShadowInPass(material, transmission);
}

static bool ShouldDrawToonOutline(EntityID entity)
{
    if (!Registry::HasComponent(entity, ComponentType::MATERIAL))
    {
        return false;
    }

    const auto& material = MaterialManager::Resolve(ComponentManager::GetComponentUnchecked<MaterialComponent>(entity));
    if (Registry::HasComponent(entity, ComponentType::MESH) &&
        !Registry::HasComponent(entity, ComponentType::STATIC_MODEL) &&
        !Registry::HasComponent(entity, ComponentType::ANIMATION_MODEL))
    {
        return material.ShaderClassMode == MaterialMode::Manual && material.ShaderClass == ShaderClass::Toon;
    }
    if (Registry::HasComponent(entity, ComponentType::SPRITE) &&
        ComponentManager::GetComponentUnchecked<SpriteComponent>(entity).Is3D)
    {
        return material.ShaderClassMode == MaterialMode::Manual && material.ShaderClass == ShaderClass::Toon;
    }

    return material.ShaderClassMode == MaterialMode::Auto ||
           (material.ShaderClassMode == MaterialMode::Manual && material.ShaderClass == ShaderClass::Toon);
}

static float GetCameraDepth(EntityID entity, const XMMATRIX& view)
{
    if (!Registry::HasComponent(entity, ComponentType::TRANSFORM))
    {
        return 0.0f;
    }
    const XMFLOAT3& pos = ComponentManager::GetComponentUnchecked<TransformComponent>(entity).Position;
    const XMVECTOR viewPos = XMVector3TransformCoord(XMLoadFloat3(&pos), view);
    return XMVectorGetZ(viewPos);
}

const char* GetSpriteVsPath(EntityID entity, bool is3D)
{
    if (ComponentManager::HasComponent<ShaderComponent>(entity))
    {
        const auto& shader = ComponentManager::GetComponentUnchecked<ShaderComponent>(entity);
        if (!shader.VsPath.empty())
        {
            return shader.VsPath.c_str();
        }
    }
    if (is3D)
    {
        return "shader/hlsl/build/colorshader3dVS.cso";
    }
    return "shader/hlsl/build/colorshaderVS.cso";
}

const char* GetSpritePsPath(EntityID entity, bool is3D)
{
    if (ComponentManager::HasComponent<ShaderComponent>(entity))
    {
        const auto& shader = ComponentManager::GetComponentUnchecked<ShaderComponent>(entity);
        if (!shader.PsPath.empty())
        {
            return shader.PsPath.c_str();
        }
    }
    if (is3D)
    {
        return "shader/hlsl/build/colorshader3dPS.cso";
    }
    return "shader/hlsl/build/colorshaderPS.cso";
}

const char* GetMeshVsPath(EntityID entity)
{
    if (ComponentManager::HasComponent<ShaderComponent>(entity))
    {
        const auto& shader = ComponentManager::GetComponentUnchecked<ShaderComponent>(entity);
        if (!shader.VsPath.empty())
        {
            return shader.VsPath.c_str();
        }
    }
    return "shader/hlsl/build/modelshaderVS.cso";
}

const char* GetMeshPsPath(EntityID entity)
{
    if (ComponentManager::HasComponent<ShaderComponent>(entity))
    {
        const auto& shader = ComponentManager::GetComponentUnchecked<ShaderComponent>(entity);
        if (!shader.PsPath.empty())
        {
            return shader.PsPath.c_str();
        }
    }
    return "shader/hlsl/build/modelshaderPS.cso";
}

static UINT s_MeshShaderDraws = 0;

void RenderSystem::Draw(RenderPass renderPass, bool receivingPostProcessOnly)
{
    const bool occlusionPhaseTwo = renderPass == RenderPass::OcclusionPhase2;
    if (occlusionPhaseTwo && !MeshShaderPipeline::IsSupported())
    {
        return;
    }
    if (renderPass == RenderPass::ShadowMap || renderPass == RenderPass::TransmittanceShadow)
    {
        ID3D12GraphicsCommandList* pCommandList = GraphicsDevice::GetCommandList();
        const bool transmission = renderPass == RenderPass::TransmittanceShadow;
        ID3D12PipelineState* shadowPso =
            transmission ? TransmittanceShadows::GetPipelineState() : PsoManager::GetOrCreateShadowMapPso();
        UINT8* pCbvDataBegin = FrameConstants::GetConstantBufferPtr();
        ID3D12DescriptorHeap* cbvHeap = FrameConstants::GetCbvHeap();
        if (!pCommandList || !shadowPso || !pCbvDataBegin || !cbvHeap)
        {
            return;
        }

        // 影パスの可視判定は GPU（GpuObjectCullCS）。PSO を設定する前に取得する。
        const int cullView = GpuObjectCulling::ShadowView(pCommandList);
        pCommandList->SetPipelineState(shadowPso);
        auto writeShadowCb = [&](EntityID entity) {
            XMMATRIX world =
                XMLoadFloat4x4(&ComponentManager::GetComponentUnchecked<TransformComponent>(entity).WorldMatrix);
            ConstantBuffer3D cb{};
            cb.World = XMMatrixTranspose(world);
            cb.UseTexture = 0;
            memcpy(pCbvDataBegin + (entity * FrameConstants::g_kCB_ALIGNED_SIZE), &cb, sizeof(cb));
            pCommandList->SetGraphicsRootDescriptorTable(0, FrameConstants::GetConstantBufferHandle(entity));
            const auto& material =
                MaterialManager::Resolve(ComponentManager::HasComponent<MaterialComponent>(entity)
                                             ? ComponentManager::GetComponentUnchecked<MaterialComponent>(entity)
                                             : DefaultMaterial());
            MaterialBindings::BindAssetSlot(entity, material);
        };

        for (EntityID i : World::GetView<SpriteComponent>())
        {
            const auto& sprite = ComponentManager::GetComponentUnchecked<SpriteComponent>(i);
            if (!sprite.Is3D || sprite.VertexBufferView.BufferLocation == 0 ||
                !Registry::HasComponent(i, ComponentType::TRANSFORM) || !ShouldCastShadow(i, transmission) ||
                !LightingResources::ShouldDrawEntityInCurrentShadowSubset(i) || InstancingSystem::CanInstance(i))
            {
                continue;
            }

            writeShadowCb(i);
            pCommandList->IASetVertexBuffers(0, 1, &sprite.VertexBufferView);
            GpuObjectCulling::Begin(pCommandList, cullView, i);
            pCommandList->DrawInstanced(sprite.VertexCount, 1, 0, 0);
            GpuObjectCulling::End(pCommandList);
        }

        for (EntityID i : World::GetView<MeshComponent>())
        {
            if (Registry::HasComponent(i, ComponentType::STATIC_MODEL) ||
                Registry::HasComponent(i, ComponentType::ANIMATION_MODEL))
            {
                continue;
            }

            const auto& mesh = ComponentManager::GetComponentUnchecked<MeshComponent>(i);
            if (mesh.VertexBufferView.BufferLocation == 0 || !Registry::HasComponent(i, ComponentType::TRANSFORM) ||
                !ShouldCastShadow(i, transmission) || !LightingResources::ShouldDrawEntityInCurrentShadowSubset(i) ||
                InstancingSystem::CanInstance(i))
            {
                continue;
            }

            writeShadowCb(i);
            VertexDeformationView deformation;
            const auto vertexView =
                VertexDeformation::TryResolveMesh(i, deformation) ? deformation.CurrentView : mesh.VertexBufferView;
            pCommandList->IASetVertexBuffers(0, 1, &vertexView);
            GpuObjectCulling::Begin(pCommandList, cullView, i);
            pCommandList->DrawInstanced(mesh.VertexCount, 1, 0, 0);
            GpuObjectCulling::End(pCommandList);
        }
        if (!transmission)
        {
            MeshShaderRenderer::DrawShadow(pCommandList);
        }
        return;
    }

    if (renderPass == RenderPass::Velocity)
    {
        if (RenderConfiguration::GetRenderMode() != RenderMode::DEFERRED)
        {
            return;
        }
        ID3D12GraphicsCommandList* commandList = GraphicsDevice::GetCommandList();
        ID3D12PipelineState* pso = PsoManager::GetVelocityGeometryPso();
        if (!commandList || !pso)
        {
            return;
        }
        XMMATRIX view = XMMatrixIdentity(), projection = XMMatrixIdentity();
        Camera::GetCameraMatrices(Camera::GetCameraEntity(), view, projection);
        XMMATRIX previousViewProjection = XMLoadFloat4x4(&GraphicsDevice::GetPreviousViewMatrix()) *
                                          XMLoadFloat4x4(&GraphicsDevice::GetPreviousProjectionMatrix());
        commandList->SetGraphicsRootSignature(ShaderPipelines::GetModelRootSignature());
        NoiseCacheBindings::BindMain(commandList);
        // 可視判定は GPU（GpuObjectCullCS）。PSO を設定する前に取得する。
        const int cullView = GpuObjectCulling::CameraView(commandList);
        commandList->SetPipelineState(pso);
        commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        auto draw = [&](EntityID entity, const D3D12_VERTEX_BUFFER_VIEW& vbv, UINT vertexCount) {
            if (!ComponentManager::HasComponent<TransformComponent>(entity) || vbv.BufferLocation == 0)
            {
                return;
            }
            const auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
            // 負荷削減：動いていない物は全画面のカメラ Velocity と同じ値になるので描かない
            // （安い判定を先に。マテリアル解決・インスタンシング判定は残った物だけ）
            if (RenderCulling::GetSettings().StaticVelocitySkip && transform.HasPreviousWorld &&
                memcmp(&transform.WorldMatrix, &transform.PreviousWorldMatrix, sizeof(transform.WorldMatrix)) == 0 &&
                !VertexDeformation::IsRequested(entity))
            {
                ++RenderCulling::MutableStats().VelocitySkipped;
                return;
            }
            if (MaterialSystem::IsTransparent(entity))
            {
                return;
            }
            ConstantBuffer3D cb{};
            cb.World = XMMatrixTranspose(XMLoadFloat4x4(&transform.WorldMatrix));
            cb.View = XMMatrixTranspose(view);
            cb.Projection = XMMatrixTranspose(projection);
            cb.PreviousWorld = XMMatrixTranspose(XMLoadFloat4x4(&transform.PreviousWorldMatrix));
            cb.PreviousViewProjection = XMMatrixTranspose(previousViewProjection);
            commandList->SetGraphicsRootDescriptorTable(0, FrameConstants::AllocateTransientConstantBuffer(cb));
            D3D12_VERTEX_BUFFER_VIEW views[2] = {vbv, vbv};
            VertexDeformationView deformation;
            if (VertexDeformation::TryResolveMesh(entity, deformation))
            {
                views[0] = deformation.CurrentView;
                views[1] = deformation.PreviousView;
            }
            commandList->IASetVertexBuffers(0, 2, views);
            commandList->IASetIndexBuffer(nullptr);
            GpuObjectCulling::Begin(commandList, cullView, entity);
            commandList->DrawInstanced(vertexCount, 1, 0, 0);
            GpuObjectCulling::End(commandList);
        };

        for (EntityID entity : World::GetView<SpriteComponent>())
        {
            const auto& sprite = ComponentManager::GetComponentUnchecked<SpriteComponent>(entity);
            if (sprite.Is3D)
            {
                draw(entity, sprite.VertexBufferView, sprite.VertexCount);
            }
        }
        for (EntityID entity : World::GetView<MeshComponent>())
        {
            if (Registry::HasComponent(entity, ComponentType::STATIC_MODEL) ||
                Registry::HasComponent(entity, ComponentType::ANIMATION_MODEL))
            {
                continue;
            }
            if (MeshShaderRenderer::IsActive() && GPUDrivenRenderer::HandlesEntity(entity))
            {
                continue;
            }
            const auto& mesh = ComponentManager::GetComponentUnchecked<MeshComponent>(entity);
            draw(entity, mesh.VertexBufferView, mesh.VertexCount);
        }
        return;
    }

    const bool drawTransparent = renderPass == RenderPass::OverlayScene || renderPass == RenderPass::TransparentScene;
    const bool drawDeferredOpaque = RenderConfiguration::GetRenderMode() == RenderMode::DEFERRED &&
                                    (renderPass == RenderPass::PrimaryScene || occlusionPhaseTwo);
    ID3D12GraphicsCommandList* pCommandList = GraphicsDevice::GetCommandList();
    if (!pCommandList)
    {
        return;
    }

    ID3D12DescriptorHeap* pCbvHeap = FrameConstants::GetCbvHeap();
    if (!pCbvHeap)
    {
        return;
    }

    ScenePasses::BeginSpritePass();
    // 内訳の計測：収集（マテリアル解決・PSO 検索・定数書き込み）と提出（コマンド記録）
    std::optional<RenderProfiler::ScopedEvent> collectProfile;
    collectProfile.emplace(drawDeferredOpaque ? "RenderSystem: Collect (opaque)" : "RenderSystem: Collect",
                           pCommandList);

    UINT8* pCbvDataBegin = FrameConstants::GetConstantBufferPtr();
    if (!pCbvDataBegin)
    {
        return;
    }

    UINT descriptorIncrement = FrameConstants::GetCbvIncrementSize();

    auto spriteEntities = World::GetView<SpriteComponent>();
    const size_t spriteCapacity = Registry::GetActiveEntities(ComponentType::SPRITE).size();
    const size_t modelCapacity = Registry::GetActiveEntities(ComponentType::MESH).size();
    m_SpriteDrawCalls.clear();
    m_ModelDrawCalls.clear();
    m_SpriteDrawCalls.reserve(spriteCapacity);
    m_ModelDrawCalls.reserve(modelCapacity);

    XMMATRIX viewMat = XMMatrixIdentity();
    XMMATRIX projMat = XMMatrixIdentity();
    Camera::GetCameraMatrices(Camera::GetCameraEntity(), viewMat, projMat);
    const XMMATRIX transposedView = XMMatrixTranspose(viewMat);
    const XMMATRIX transposedProj = XMMatrixTranspose(projMat);
    const float sceneAspectRatio = GraphicsDevice::GetSceneAspectRatio();
    const int defaultTextureIndex = TextureManager::GetDefaultTextureIndex();
    XMFLOAT3 cameraPosition = {0.0f, 0.0f, 5.0f};
    if (Registry::HasComponent(Camera::GetCameraEntity(), ComponentType::TRANSFORM))
    {
        cameraPosition =
            ComponentManager::GetComponentUnchecked<TransformComponent>(Camera::GetCameraEntity()).Position;
    }
    const int defaultNormalIndex = TextureManager::GetDefaultTextureIndex();

    for (EntityID i : spriteEntities)
    {
        auto& sprite = ComponentManager::GetComponentUnchecked<SpriteComponent>(i);
        if ((renderPass == RenderPass::TransparentScene && !sprite.Is3D) ||
            (renderPass == RenderPass::OverlayScene && sprite.Is3D))
        {
            continue;
        }
        bool isReceiving = MaterialSystem::IsReceivingPostProcess(i);
        const MaterialComponent* material;
        if (Registry::HasComponent(i, ComponentType::MATERIAL))
        {
            material = &MaterialManager::Resolve(ComponentManager::GetComponentUnchecked<MaterialComponent>(i));
        }
        else
        {
            material = nullptr;
        }
        bool isTransparent = material && MaterialSystem::IsTransparentMaterial(*material);
        if (isTransparent != drawTransparent)
        {
            continue;
        }
        if (!drawTransparent && !drawDeferredOpaque && isReceiving != receivingPostProcessOnly)
        {
            continue;
        }

        if (sprite.VertexBufferView.BufferLocation != 0)
        {
            if (sprite.Is3D)
            {
                if (!Registry::HasComponent(i, ComponentType::TRANSFORM))
                {
                    continue;
                }
                if (InstancingSystem::CanInstance(i))
                {
                    continue;
                }
                // 視錐台・画面サイズの判定は GPU（描画時に SetPredication で破棄）。

                ShaderDescription psoResource{};
                psoResource.vsPath = GetSpriteVsPath(i, true);
                psoResource.psPath = GetSpritePsPath(i, true);
                if (material && material->ShaderClassMode == MaterialMode::Manual &&
                    material->ShaderClass == ShaderClass::PBR)
                {
                    psoResource.psPath = "shader/hlsl/build/modelshaderPS.cso";
                }
                psoResource.isModel = true;
                psoResource.enableAlphaBlend = drawTransparent;
                if (material && material->RuntimeHandle != 0)
                {
                    if (material->AssetCull == MaterialCull::None)
                    {
                        psoResource.cullMode = D3D12_CULL_MODE_NONE;
                    }
                    else if (material->AssetCull == MaterialCull::Front)
                    {
                        psoResource.cullMode = D3D12_CULL_MODE_FRONT;
                    }
                    psoResource.depthWrite = material->AssetDepthWrite;
                }

                ID3D12PipelineState* pso = PsoManager::GetOrCreateGraphicsPso(psoResource);
                if (!pso)
                {
                    continue;
                }

                XMMATRIX world =
                    XMLoadFloat4x4(&ComponentManager::GetComponentUnchecked<TransformComponent>(i).WorldMatrix);

                ConstantBuffer3D cb3D{};
                cb3D.World = XMMatrixTranspose(world);
                cb3D.View = transposedView;
                cb3D.Projection = transposedProj;
                cb3D.UseTexture = 0;

                cb3D.CameraPos = cameraPosition;

                int srvIndex = defaultTextureIndex;
                int normalSrvIndex = defaultNormalIndex;
                if (material)
                {
                    const auto& mat = *material;
                    cb3D.UseTexture = static_cast<int>(mat.UseTexture);
                    cb3D.MaterialMetallic = mat.Metallic;
                    cb3D.MaterialRoughness = mat.Roughness;
                    cb3D.MaterialFresnel = mat.Fresnel;
                    cb3D.MaterialAlpha = mat.Alpha;
                    cb3D.MaterialIsTransparent = static_cast<int>(mat.IsTransparent);
                    cb3D.MaterialMode = static_cast<int>(mat.ShaderClassMode);
                    cb3D.ShaderClass = static_cast<int>(mat.ShaderClass);
                    cb3D.ToonOutlineWidth = mat.ToonOutlineWidth;
                    if (cb3D.UseTexture != 0 && mat.TextureID >= 0)
                    {
                        srvIndex = mat.TextureID;
                    }
                    if (mat.NormalMapID >= 0)
                    {
                        cb3D.UseNormalMap = 1;
                        normalSrvIndex = mat.NormalMapID;
                    }
                }
                if (srvIndex >= FrameConstants::g_kTEXTURE_SRV_START_INDEX + FrameConstants::g_kMAX_SRVS)
                {
                    continue;
                }

                memcpy(pCbvDataBegin + (i * FrameConstants::g_kCB_ALIGNED_SIZE), &cb3D, sizeof(cb3D));
                float cameraDepth;
                if (drawTransparent)
                {
                    cameraDepth = GetCameraDepth(i, viewMat);
                }
                else
                {
                    cameraDepth = 0.0f;
                }
                m_ModelDrawCalls.push_back(DrawCall{i, pso, srvIndex, normalSrvIndex, sprite.VertexBufferView,
                                                    sprite.VertexCount, true, material, cameraDepth});
                continue;
            }

            ShaderDescription psoResource{};
            psoResource.vsPath = GetSpriteVsPath(i, false);
            psoResource.psPath = GetSpritePsPath(i, false);
            psoResource.isModel = false;
            psoResource.enableAlphaBlend = drawTransparent;
            if (material && material->RuntimeHandle != 0)
            {
                if (material->AssetCull == MaterialCull::None)
                {
                    psoResource.cullMode = D3D12_CULL_MODE_NONE;
                }
                else if (material->AssetCull == MaterialCull::Front)
                {
                    psoResource.cullMode = D3D12_CULL_MODE_FRONT;
                }
                psoResource.depthWrite = material->AssetDepthWrite;
            }

            ID3D12PipelineState* pso = PsoManager::GetOrCreateGraphicsPso(psoResource);
            if (!pso)
            {
                continue;
            }

            struct CbData
            {
                float x;
                float y;
                int UseTexture;
                float AspectRatio;
                float ScaleX;
                float ScaleY;
                int UseNormalMap;
                int MaterialMode;
                int ShaderClass;
                float MaterialAlpha;
                float Pad0;
                float Pad1;
            } cbData = {0, 0, 0, 1.0f, 1.0f, 1.0f, 0, 0, 10, 1.0f, 0, 0};

            if (Registry::HasComponent(i, ComponentType::TRANSFORM))
            {
                auto& t = ComponentManager::GetComponentUnchecked<TransformComponent>(i);
                cbData.x = t.Position.x;
                cbData.y = t.Position.y;
                cbData.ScaleX = t.Scale.x;
                cbData.ScaleY = t.Scale.y;
            }
            cbData.AspectRatio = sceneAspectRatio;

            int srvIndex = defaultTextureIndex;
            int normalSrvIndex = defaultNormalIndex;
            if (material)
            {
                const auto& mat = *material;
                cbData.UseTexture = static_cast<int>(mat.UseTexture);
                cbData.MaterialMode = static_cast<int>(mat.ShaderClassMode);
                cbData.ShaderClass = static_cast<int>(mat.ShaderClass);
                cbData.MaterialAlpha = mat.Alpha;
                if (cbData.UseTexture != 0 && mat.TextureID >= 0)
                {
                    srvIndex = mat.TextureID;
                }
                if (mat.NormalMapID >= 0)
                {
                    cbData.UseNormalMap = 1;
                    normalSrvIndex = mat.NormalMapID;
                }
            }
            if (srvIndex >= FrameConstants::g_kTEXTURE_SRV_START_INDEX + FrameConstants::g_kMAX_SRVS)
            {
                continue;
            }

            auto* pCb = reinterpret_cast<CbData*>(pCbvDataBegin + (i * FrameConstants::g_kCB_ALIGNED_SIZE));
            *pCb = cbData;

            float cameraDepth;
            if (drawTransparent)
            {
                cameraDepth = GetCameraDepth(i, viewMat);
            }
            else
            {
                cameraDepth = 0.0f;
            }
            m_SpriteDrawCalls.push_back(DrawCall{i, pso, srvIndex, normalSrvIndex, sprite.VertexBufferView,
                                                 sprite.VertexCount, false, material, cameraDepth});
        }
    }

    for (EntityID i : World::GetView<MeshComponent>())
    {
        if (!drawTransparent && drawDeferredOpaque && GPUDrivenRenderer::HandlesEntity(i))
        {
            continue;
        }
        if (Registry::HasComponent(i, ComponentType::STATIC_MODEL) ||
            Registry::HasComponent(i, ComponentType::ANIMATION_MODEL))
        {
            continue;
        }

        auto& mesh = ComponentManager::GetComponentUnchecked<MeshComponent>(i);
        if (mesh.VertexBufferView.BufferLocation == 0)
        {
            continue;
        }
        if (renderPass == RenderPass::OverlayScene)
        {
            continue;
        }
        if (!Registry::HasComponent(i, ComponentType::TRANSFORM))
        {
            continue;
        }
        if (InstancingSystem::CanInstance(i))
        {
            continue;
        }
        // 視錐台・画面サイズの判定は GPU（描画時に SetPredication で破棄）。

        bool isReceiving = MaterialSystem::IsReceivingPostProcess(i);
        const MaterialComponent* material;
        if (Registry::HasComponent(i, ComponentType::MATERIAL))
        {
            material = &MaterialManager::Resolve(ComponentManager::GetComponentUnchecked<MaterialComponent>(i));
        }
        else
        {
            material = nullptr;
        }
        bool isTransparent = material && MaterialSystem::IsTransparentMaterial(*material);
        if (isTransparent != drawTransparent)
        {
            continue;
        }
        if (!drawTransparent && !drawDeferredOpaque && isReceiving != receivingPostProcessOnly)
        {
            continue;
        }

        ShaderDescription psoResource{};
        psoResource.vsPath = GetMeshVsPath(i);
        psoResource.psPath = GetMeshPsPath(i);
        psoResource.isModel = true;
        psoResource.enableAlphaBlend = drawTransparent;
        if (material && material->RuntimeHandle != 0)
        {
            if (material->AssetCull == MaterialCull::None)
            {
                psoResource.cullMode = D3D12_CULL_MODE_NONE;
            }
            else if (material->AssetCull == MaterialCull::Front)
            {
                psoResource.cullMode = D3D12_CULL_MODE_FRONT;
            }
            psoResource.depthWrite = material->AssetDepthWrite;
        }

        ID3D12PipelineState* pso = PsoManager::GetOrCreateGraphicsPso(psoResource);
        if (!pso)
        {
            continue;
        }

        XMMATRIX world = XMLoadFloat4x4(&ComponentManager::GetComponentUnchecked<TransformComponent>(i).WorldMatrix);

        ConstantBuffer3D cb3D{};
        cb3D.World = XMMatrixTranspose(world);
        cb3D.View = transposedView;
        cb3D.Projection = transposedProj;
        cb3D.UseTexture = 0;

        cb3D.CameraPos = cameraPosition;

        int srvIndex = defaultTextureIndex;
        int normalSrvIndex = defaultNormalIndex;
        if (material)
        {
            const auto& mat = *material;
            cb3D.UseTexture = static_cast<int>(mat.UseTexture);
            cb3D.MaterialMetallic = mat.Metallic;
            cb3D.MaterialRoughness = mat.Roughness;
            cb3D.MaterialFresnel = mat.Fresnel;
            cb3D.MaterialAlpha = mat.Alpha;
            cb3D.MaterialIsTransparent = static_cast<int>(mat.IsTransparent);
            cb3D.MaterialMode = static_cast<int>(mat.ShaderClassMode);
            cb3D.ShaderClass = static_cast<int>(mat.ShaderClass);
            cb3D.ToonOutlineWidth = mat.ToonOutlineWidth;
            if (cb3D.UseTexture != 0 && mat.TextureID >= 0)
            {
                srvIndex = mat.TextureID;
            }
            if (mat.NormalMapID >= 0)
            {
                cb3D.UseNormalMap = 1;
                normalSrvIndex = mat.NormalMapID;
            }
        }
        if (srvIndex >= FrameConstants::g_kTEXTURE_SRV_START_INDEX + FrameConstants::g_kMAX_SRVS)
        {
            continue;
        }

        memcpy(pCbvDataBegin + (i * FrameConstants::g_kCB_ALIGNED_SIZE), &cb3D, sizeof(cb3D));
        float cameraDepth;
        if (drawTransparent)
        {
            cameraDepth = GetCameraDepth(i, viewMat);
        }
        else
        {
            cameraDepth = 0.0f;
        }
        VertexDeformationView deformation;
        const auto vertexView =
            VertexDeformation::TryResolveMesh(i, deformation) ? deformation.CurrentView : mesh.VertexBufferView;
        m_ModelDrawCalls.push_back(
            DrawCall{i, pso, srvIndex, normalSrvIndex, vertexView, mesh.VertexCount, true, material, cameraDepth});
    }

    auto sortDrawCalls = [drawTransparent](vector<DrawCall>& drawCalls) {
        sort(drawCalls.begin(), drawCalls.end(), [drawTransparent](const DrawCall& a, const DrawCall& b) {
            if (drawTransparent)
            {
                if (fabsf(a.cameraDepth - b.cameraDepth) > 0.0001f)
                {
                    return a.cameraDepth > b.cameraDepth;
                }
            }
            else
            {
                if (fabsf(a.cameraDepth - b.cameraDepth) > 0.0001f)
                {
                    return a.cameraDepth < b.cameraDepth;
                }
            }
            if (a.pso != b.pso)
            {
                return a.pso < b.pso;
            }
            if (a.srvIndex != b.srvIndex)
            {
                return a.srvIndex < b.srvIndex;
            }
            return a.normalSrvIndex < b.normalSrvIndex;
        });
    };

    sortDrawCalls(m_SpriteDrawCalls);
    sortDrawCalls(m_ModelDrawCalls);
    collectProfile.reset();
    RenderProfiler::ScopedEvent submitProfile(
        drawDeferredOpaque ? "RenderSystem: Submit (opaque)" : "RenderSystem: Submit", pCommandList);

    auto heapStart = pCbvHeap->GetGPUDescriptorHandleForHeapStart();
    // 可視判定は GPU（GpuObjectCullCS）。PSO を設定する前に取得する。
    const int cullView = (m_SpriteDrawCalls.empty() && m_ModelDrawCalls.empty())
                             ? GpuObjectCulling::kNoView
                             : GpuObjectCulling::CameraView(pCommandList);

    auto submitDrawCalls = [&](const vector<DrawCall>& drawCalls) {
        ID3D12PipelineState* lastPso = nullptr;
        ID3D12PipelineState* outlinePso = PsoManager::GetOrCreateToonOutlinePso(drawTransparent);
        int lastSrvIndex = -1;
        int lastNormalSrvIndex = -1;

        for (const auto& dc : drawCalls)
        {
            if (occlusionPhaseTwo && (!dc.is3D || !ComponentManager::HasComponent<MeshComponent>(dc.EntityID)))
            {
                continue;
            }
            const MaterialComponent* materialSelection = nullptr;
            if (dc.material)
            {
                materialSelection = &(*dc.material);
            }
            else
            {
                materialSelection = &(DefaultMaterial());
            }
            const MaterialComponent& material = *materialSelection;
            MaterialBindings::SetMaterial(dc.EntityID, material);
            if (InstancingSystem::CanInstance(dc.EntityID))
            {
                continue;
            }

            if (dc.pso != lastPso)
            {
                pCommandList->SetPipelineState(dc.pso);
                lastPso = dc.pso;
            }

            pCommandList->SetGraphicsRootDescriptorTable(0, FrameConstants::GetConstantBufferHandle(dc.EntityID));

            if (dc.srvIndex != lastSrvIndex)
            {
                TextureManager::TouchTexture(dc.srvIndex);
                CD3DX12_GPU_DESCRIPTOR_HANDLE srvHandle(heapStart, dc.srvIndex, descriptorIncrement);
                pCommandList->SetGraphicsRootDescriptorTable(1, srvHandle);
                lastSrvIndex = dc.srvIndex;
            }
            if (dc.normalSrvIndex != lastNormalSrvIndex)
            {
                TextureManager::TouchTexture(dc.normalSrvIndex);
                CD3DX12_GPU_DESCRIPTOR_HANDLE normalSrvHandle(heapStart, dc.normalSrvIndex, descriptorIncrement);
                pCommandList->SetGraphicsRootDescriptorTable(6, normalSrvHandle);
                lastNormalSrvIndex = dc.normalSrvIndex;
            }

            pCommandList->IASetVertexBuffers(0, 1, &dc.vbv);
            if (dc.is3D)
            {
                GpuObjectCulling::Begin(pCommandList, cullView, dc.EntityID);
            }
            bool meshShaderDrawn = false;
            if (material.RuntimeHandle == 0 && drawDeferredOpaque && dc.is3D &&
                ComponentManager::HasComponent<MeshComponent>(dc.EntityID))
            {
                const auto& mesh = ComponentManager::GetComponentUnchecked<MeshComponent>(dc.EntityID);
                VertexDeformationView deformation;
                MeshShaderPipeline::Geometry geometry;
                geometry.Vertices = VertexDeformation::TryResolveMesh(dc.EntityID, deformation)
                                        ? deformation.CurrentResource
                                        : mesh.VertexBuffer.Get();
                geometry.VertexState = deformation.CurrentResource ? D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER
                                                                   : D3D12_RESOURCE_STATE_GENERIC_READ;
                geometry.VertexView = dc.vbv;
                geometry.ElementCount = dc.vertexCount;
                XMFLOAT3 center = mesh.LocalBoundsCenter, extents = mesh.LocalBoundsExtents;
                VertexDeformation::TryGetLocalBounds(dc.EntityID, center, extents);
                meshShaderDrawn = MeshShaderPipeline::Draw(pCommandList, geometry, center, extents);
                ++s_MeshShaderDraws;
            }
            if (meshShaderDrawn)
            {
                lastPso = nullptr;
            }
            else if (!occlusionPhaseTwo)
            {
                pCommandList->DrawInstanced(dc.vertexCount, 1, 0, 0);
            }

            if (!occlusionPhaseTwo && dc.is3D && outlinePso && ShouldDrawToonOutline(dc.EntityID))
            {
                pCommandList->SetPipelineState(outlinePso);
                pCommandList->DrawInstanced(dc.vertexCount, 1, 0, 0);
                lastPso = outlinePso;
            }
            GpuObjectCulling::End(pCommandList);
        }
    };

    if (!m_SpriteDrawCalls.empty())
    {
        ScenePasses::BeginSpritePass();
        submitDrawCalls(m_SpriteDrawCalls);
    }

    if (!m_ModelDrawCalls.empty())
    {
        ScenePasses::BeginModelPass();
        submitDrawCalls(m_ModelDrawCalls);
    }
    // 何件がこの CPU 経路に来ているか（GPU 駆動が受け持てていない物の数）。ログに 1 秒ごと
    static auto lastLog = chrono::steady_clock::now();
    if (drawDeferredOpaque && chrono::steady_clock::now() - lastLog > chrono::seconds(1))
    {
        lastLog = chrono::steady_clock::now();
        WriteGraphicsLog("RenderSystem(CPU path): sprites=%zu models=%zu meshShaderDraws=%u gpuDriven=%d\n",
                         m_SpriteDrawCalls.size(), m_ModelDrawCalls.size(), s_MeshShaderDraws,
                         GPUDrivenRenderer::IsActive() ? 1 : 0);
    }
    s_MeshShaderDraws = 0;
}

