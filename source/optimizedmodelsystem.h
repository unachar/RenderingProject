#pragma once

#include "pch.h"
#include "modelsystem.h"
#include "componentmanager.h"
#include "modelmanager.h"
#include "rendererresource.h"
#include "renderercore.h"
#include "renderershader.h"
#include "psomanager.h"
#include "camera.h"
#include "materialsystem.h"
#include "world.h"
#include "gpudrivenindirect.h"
#include <vector>

// GPU-driven model submission. Geometry is culled and emitted through
// ExecuteIndirect for the primary, shadow, and velocity passes; material and
// PSO state is grouped on the CPU because D3D12 indirect signatures cannot
// change pipeline state or descriptor tables.
class OptimizedModelSystem final : public SystemBase
{
private:
    ModelSystem m_Legacy{};
    GpuDrivenIndirectDrawCache m_IndirectDraws{};

    static bool IsSkyEntity(EntityID entity)
    {
        return ComponentManager::HasComponent<NameComponent>(entity) &&
            ComponentManager::GetComponentUnchecked<NameComponent>(entity).Name == "Sky";
    }

    static bool ShouldCastShadow(EntityID entity)
    {
        if (IsSkyEntity(entity) || ComponentManager::HasComponent<LightComponent>(entity))
        {
            return false;
        }
        if (!Registry::HasComponent(entity, ComponentType::MATERIAL))
        {
            return true;
        }

        const auto& material = ComponentManager::GetComponentUnchecked<MaterialComponent>(entity);
        if (MaterialSystem::IsTransparentMaterial(material))
        {
            return false;
        }
        return !(material.ShaderClassMode == MaterialMode::Manual &&
            material.ShaderClass == ShaderClass::Shadow);
    }

    static void SubmitBarriers(
        ID3D12GraphicsCommandList* commandList,
        const std::vector<D3D12_RESOURCE_BARRIER>& barriers)
    {
        if (commandList && !barriers.empty())
        {
            commandList->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
        }
    }

    static void RestoreShadowGraphicsState(
        ID3D12GraphicsCommandList* commandList,
        ID3D12PipelineState* shadowPso)
    {
        commandList->SetGraphicsRootSignature(RendererShader::GetModelRootSignature());
        if (RendererResource::GetShadowCB())
        {
            commandList->SetGraphicsRootConstantBufferView(
                5,
                RendererResource::GetCurrentShadowConstantBufferAddress());
        }
        commandList->SetPipelineState(shadowPso);
        commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    }

    static void RestoreVelocityGraphicsState(
        ID3D12GraphicsCommandList* commandList,
        ID3D12PipelineState* velocityPso)
    {
        commandList->SetGraphicsRootSignature(RendererShader::GetModelRootSignature());
        commandList->SetPipelineState(velocityPso);
        commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    }

    void DrawShadowMap()
    {
        ID3D12GraphicsCommandList* commandList = RendererCore::GetCommandList();
        ID3D12PipelineState* shadowPso = PsoManager::GetOrCreateShadowMapPso();
        UINT8* constantBufferBegin = RendererResource::GetConstantBufferPtr();
        ID3D12DescriptorHeap* cbvHeap = RendererResource::GetCbvHeap();
        if (!commandList || !shadowPso || !constantBufferBegin || !cbvHeap)
        {
            return;
        }

        ID3D12DescriptorHeap* heaps[] = { cbvHeap };
        commandList->SetDescriptorHeaps(_countof(heaps), heaps);
        RestoreShadowGraphicsState(commandList, shadowPso);
        const XMMATRIX lightViewProjection = RendererResource::GetCurrentShadowViewProjection();

        auto writeShadowConstants = [&](EntityID entity)
            {
                const auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
                ConstantBuffer3D constants{};
                constants.World = XMMatrixTranspose(XMLoadFloat4x4(&transform.WorldMatrix));
                constants.UseTexture = 0;
                memcpy(
                    constantBufferBegin + entity * RendererResource::g_kCB_ALIGNED_SIZE,
                    &constants,
                    sizeof(constants));
                commandList->SetGraphicsRootDescriptorTable(
                    0,
                    RendererResource::GetConstantBufferHandle(entity));
            };

        std::vector<D3D12_RESOURCE_BARRIER> barriers;
        for (EntityID entity : World::GetView<AnimationModelComponent, TransformComponent>())
        {
            const auto& component = ComponentManager::GetComponentUnchecked<AnimationModelComponent>(entity);
            if (component.ModelId < 0 || !ShouldCastShadow(entity))
            {
                continue;
            }

            AnimationModelResource* model = ModelManager::GetAnimModel(component.ModelId);
            if (!model)
            {
                continue;
            }

            model->DispatchGpuSkinning(commandList);
            RestoreShadowGraphicsState(commandList, shadowPso);

            barriers.clear();
            barriers.reserve(model->GetMeshCount());
            for (UINT meshIndex = 0; meshIndex < model->GetMeshCount(); ++meshIndex)
            {
                const MeshData& mesh = model->GetMeshData(meshIndex);
                if (mesh.VertexBuffer)
                {
                    barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
                        mesh.VertexBuffer.Get(),
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER));
                }
            }
            SubmitBarriers(commandList, barriers);

            const auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
            const XMMATRIX world = XMLoadFloat4x4(&transform.WorldMatrix);
            writeShadowConstants(entity);
            if (!m_IndirectDraws.ExecuteShadow(commandList, model, entity, world, lightViewProjection, shadowPso))
            {
                RestoreShadowGraphicsState(commandList, shadowPso);
                for (UINT meshIndex = 0; meshIndex < model->GetMeshCount(); ++meshIndex)
                {
                    const MeshData& mesh = model->GetMeshData(meshIndex);
                    if (!mesh.VertexBuffer || !mesh.IndexBuffer || mesh.IndexCount == 0)
                    {
                        continue;
                    }
                    commandList->IASetVertexBuffers(0, 1, &mesh.VertexBufferView);
                    commandList->IASetIndexBuffer(&mesh.IndexBufferView);
                    commandList->DrawIndexedInstanced(mesh.IndexCount, 1, 0, 0, 0);
                }
            }

            barriers.clear();
            for (UINT meshIndex = 0; meshIndex < model->GetMeshCount(); ++meshIndex)
            {
                const MeshData& mesh = model->GetMeshData(meshIndex);
                if (mesh.VertexBuffer)
                {
                    barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
                        mesh.VertexBuffer.Get(),
                        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
                }
            }
            SubmitBarriers(commandList, barriers);
        }

        for (EntityID entity : World::GetView<StaticModelComponent, TransformComponent>())
        {
            const auto& component = ComponentManager::GetComponentUnchecked<StaticModelComponent>(entity);
            if (component.ModelId < 0 || !ShouldCastShadow(entity))
            {
                continue;
            }

            StaticModelResource* model = ModelManager::GetStaticModel(component.ModelId);
            if (!model)
            {
                continue;
            }

            const auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
            const XMMATRIX world = XMLoadFloat4x4(&transform.WorldMatrix);
            writeShadowConstants(entity);
            if (!m_IndirectDraws.ExecuteShadow(commandList, model, entity, world, lightViewProjection, shadowPso))
            {
                RestoreShadowGraphicsState(commandList, shadowPso);
                for (UINT meshIndex = 0; meshIndex < model->GetMeshCount(); ++meshIndex)
                {
                    const StaticMeshData& mesh = model->GetMeshData(meshIndex);
                    if (!mesh.VertexBuffer || !mesh.IndexBuffer || mesh.IndexCount == 0)
                    {
                        continue;
                    }
                    commandList->IASetVertexBuffers(0, 1, &mesh.VertexBufferView);
                    commandList->IASetIndexBuffer(&mesh.IndexBufferView);
                    commandList->DrawIndexedInstanced(mesh.IndexCount, 1, 0, 0, 0);
                }
            }
        }
    }

    void DrawVelocity()
    {
        ID3D12GraphicsCommandList* commandList = RendererCore::GetCommandList();
        ID3D12PipelineState* velocityPso = PsoManager::GetVelocityGeometryPso();
        if (!commandList || !velocityPso)
        {
            return;
        }

        XMMATRIX view = XMMatrixIdentity();
        XMMATRIX projection = XMMatrixIdentity();
        Camera::GetCameraMatrices(Camera::GetCameraEntity(), view, projection);
        const XMMATRIX viewProjection = view * projection;
        const XMMATRIX previousViewProjection =
            XMLoadFloat4x4(&RendererCore::GetPreviousViewMatrix()) *
            XMLoadFloat4x4(&RendererCore::GetPreviousProjectionMatrix());
        RestoreVelocityGraphicsState(commandList, velocityPso);

        auto setConstants = [&](EntityID entity)
            {
                const auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
                ConstantBuffer3D constants{};
                constants.World = XMMatrixTranspose(XMLoadFloat4x4(&transform.WorldMatrix));
                constants.View = XMMatrixTranspose(view);
                constants.Projection = XMMatrixTranspose(projection);
                constants.PreviousWorld = XMMatrixTranspose(XMLoadFloat4x4(&transform.PreviousWorldMatrix));
                constants.PreviousViewProjection = XMMatrixTranspose(previousViewProjection);
                commandList->SetGraphicsRootDescriptorTable(
                    0,
                    RendererResource::AllocateTransientConstantBuffer(constants));
            };

        std::vector<D3D12_RESOURCE_BARRIER> barriers;
        for (EntityID entity : World::GetView<AnimationModelComponent, TransformComponent>())
        {
            const auto& component = ComponentManager::GetComponentUnchecked<AnimationModelComponent>(entity);
            AnimationModelResource* model = ModelManager::GetAnimModel(component.ModelId);
            if (!model)
            {
                continue;
            }

            model->DispatchGpuSkinning(commandList);
            RestoreVelocityGraphicsState(commandList, velocityPso);

            barriers.clear();
            barriers.reserve(model->GetMeshCount());
            for (UINT meshIndex = 0; meshIndex < model->GetMeshCount(); ++meshIndex)
            {
                const MeshData& mesh = model->GetMeshData(meshIndex);
                if (mesh.VertexBuffer && mesh.PreviousVertexValid)
                {
                    barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
                        mesh.VertexBuffer.Get(),
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER));
                }
            }
            SubmitBarriers(commandList, barriers);

            const auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
            const XMMATRIX world = XMLoadFloat4x4(&transform.WorldMatrix);
            setConstants(entity);
            if (!m_IndirectDraws.ExecuteVelocity(
                commandList,
                model,
                entity,
                world,
                viewProjection,
                velocityPso))
            {
                RestoreVelocityGraphicsState(commandList, velocityPso);
                for (UINT meshIndex = 0; meshIndex < model->GetMeshCount(); ++meshIndex)
                {
                    const MeshData& mesh = model->GetMeshData(meshIndex);
                    if (!mesh.VertexBuffer || !mesh.IndexBuffer ||
                        !mesh.PreviousVertexValid || mesh.IndexCount == 0)
                    {
                        continue;
                    }
                    D3D12_VERTEX_BUFFER_VIEW views[2] =
                    {
                        mesh.VertexBufferView,
                        mesh.PreviousVertexBufferView
                    };
                    commandList->IASetVertexBuffers(0, _countof(views), views);
                    commandList->IASetIndexBuffer(&mesh.IndexBufferView);
                    commandList->DrawIndexedInstanced(mesh.IndexCount, 1, 0, 0, 0);
                }
            }

            barriers.clear();
            for (UINT meshIndex = 0; meshIndex < model->GetMeshCount(); ++meshIndex)
            {
                const MeshData& mesh = model->GetMeshData(meshIndex);
                if (mesh.VertexBuffer && mesh.PreviousVertexValid)
                {
                    barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
                        mesh.VertexBuffer.Get(),
                        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
                }
            }
            SubmitBarriers(commandList, barriers);
        }

        for (EntityID entity : World::GetView<StaticModelComponent, TransformComponent>())
        {
            const auto& component = ComponentManager::GetComponentUnchecked<StaticModelComponent>(entity);
            StaticModelResource* model = ModelManager::GetStaticModel(component.ModelId);
            if (!model)
            {
                continue;
            }

            const auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
            const XMMATRIX world = XMLoadFloat4x4(&transform.WorldMatrix);
            setConstants(entity);
            if (!m_IndirectDraws.ExecuteVelocity(
                commandList,
                model,
                entity,
                world,
                viewProjection,
                velocityPso))
            {
                RestoreVelocityGraphicsState(commandList, velocityPso);
                for (UINT meshIndex = 0; meshIndex < model->GetMeshCount(); ++meshIndex)
                {
                    const StaticMeshData& mesh = model->GetMeshData(meshIndex);
                    if (!mesh.VertexBuffer || !mesh.IndexBuffer || mesh.IndexCount == 0)
                    {
                        continue;
                    }
                    D3D12_VERTEX_BUFFER_VIEW views[2] =
                    {
                        mesh.VertexBufferView,
                        mesh.VertexBufferView
                    };
                    commandList->IASetVertexBuffers(0, _countof(views), views);
                    commandList->IASetIndexBuffer(&mesh.IndexBufferView);
                    commandList->DrawIndexedInstanced(mesh.IndexCount, 1, 0, 0, 0);
                }
            }
        }
    }

public:
    void Init() override
    {
        m_Legacy.Init();
    }

    void Uninit() override
    {
        m_IndirectDraws.Reset();
        m_Legacy.Uninit();
    }

    void Update() override
    {
        m_Legacy.Update();
    }

    void Draw(RenderPass renderPass, bool receivingPostProcessOnly) override
    {
        switch (renderPass)
        {
        case RenderPass::ShadowMap:
            DrawShadowMap();
            break;
        case RenderPass::Velocity:
            DrawVelocity();
            break;
        default:
            m_Legacy.SetIndirectDrawCache(&m_IndirectDraws);
            m_Legacy.Draw(renderPass, receivingPostProcessOnly);
            break;
        }
    }
};
