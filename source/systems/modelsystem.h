#pragma once

#include "pch.h"
#include "systembase.h"
#include "ecs.h"
#include "componentmanager.h"
#include "modelmanager.h"
#include "graphicsdevice.h"
#include "shaderpipelines.h"
#include "psomanager.h"
#include "camera.h"
#include "materialsystem.h"
#include "instancingsystem.h"
#include "world.h"
#include "gpudrivenindirect.h"
#include <unordered_set>
#include <vector>
#include "frameconstants.h"
#include "lightingresources.h"

struct MaterialComponent;

class ModelDrawBackend final : public SystemBase
{
private:
    struct AnimDrawCall
    {
        EntityID EntityID;
        ID3D12PipelineState* pso;
        int srvIndex;
        int normalSrvIndex;
        class AnimationModelResource* model;
        const MaterialComponent* material;
        float cameraDepth;
    };
    struct StaticDrawCall
    {
        EntityID EntityID;
        ID3D12PipelineState* pso;
        int srvIndex;
        int normalSrvIndex;
        class StaticModelResource* model;
        const MaterialComponent* material;
        float cameraDepth;
    };
    vector<AnimDrawCall> m_AnimDrawCalls;
    vector<StaticDrawCall> m_StaticDrawCalls;
    GpuDrivenIndirectDrawCache* m_IndirectDraws = nullptr;

public:
    void SetIndirectDrawCache(GpuDrivenIndirectDrawCache* cache) { m_IndirectDraws = cache; }
    void Draw(RenderPass renderPass, bool receivingPostProcessOnly) override;
};

class ModelSystem final : public SystemBase
{
private:
    ModelDrawBackend m_Backend{};
    GpuDrivenIndirectDrawCache m_IndirectDraws{};

    struct ModelDrawContext
    {
        ID3D12GraphicsCommandList* commandList = nullptr;
        ID3D12PipelineState* pso = nullptr;
        XMMATRIX viewProjection{};
        bool cpuCulledVirtualPage = false;
    };

    using WriteConstantsFn = void(*)(EntityID, void*);
    using ExecuteIndirectFn = bool(*)(ID3D12GraphicsCommandList*, const void*, EntityID, const XMMATRIX&, const XMMATRIX&, ID3D12PipelineState*, bool);
    using FallbackDrawFn = void(*)(ID3D12GraphicsCommandList*, const void*, EntityID, const XMMATRIX&, ID3D12PipelineState*);
    using ShouldTransitionFn = bool(*)(const void*, UINT);

    struct ModelCallbacks
    {
        void(*restoreGraphicsState)(ID3D12GraphicsCommandList*, ID3D12PipelineState*);
        WriteConstantsFn writeConstants;
        ExecuteIndirectFn executeIndirect;
        FallbackDrawFn fallbackDraw;
        ShouldTransitionFn shouldTransitionMesh;
    };

    static bool IsSkyEntity(EntityID entity)
    {
        return ComponentManager::HasComponent<NameComponent>(entity) &&
            ComponentManager::GetComponentUnchecked<NameComponent>(entity).Name == "Sky";
    }

    static bool ShouldCastShadow(EntityID entity)
    {
        if (IsSkyEntity(entity) || ComponentManager::HasComponent<LightComponent>(entity))
            return false;
        if (!Registry::HasComponent(entity, ComponentType::MATERIAL))
            return true;
        const auto& material = ComponentManager::GetComponentUnchecked<MaterialComponent>(entity);
        if (MaterialSystem::IsTransparentMaterial(material))
            return false;
        return !(material.ShaderClassMode == MaterialMode::Manual &&
            material.ShaderClass == ShaderClass::Shadow);
    }

    static void SubmitBarriers(
        ID3D12GraphicsCommandList* commandList,
        const vector<D3D12_RESOURCE_BARRIER>& barriers)
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
        commandList->SetGraphicsRootSignature(ShaderPipelines::GetModelRootSignature());
        if (LightingResources::GetShadowCB())
        {
            commandList->SetGraphicsRootConstantBufferView(
                5, LightingResources::GetCurrentShadowConstantBufferAddress());
        }
        commandList->SetPipelineState(shadowPso);
        commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    }

    static void RestoreVelocityGraphicsState(
        ID3D12GraphicsCommandList* commandList,
        ID3D12PipelineState* velocityPso)
    {
        commandList->SetGraphicsRootSignature(ShaderPipelines::GetModelRootSignature());
        commandList->SetPipelineState(velocityPso);
        commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    }

    template<typename ModelComponent, typename ModelResource, typename MeshDataType>
    void DrawModels(
        const ModelDrawContext& ctx,
        const ModelCallbacks& callbacks)
    {
        if (!ctx.commandList || !ctx.pso)
            return;

        callbacks.restoreGraphicsState(ctx.commandList, ctx.pso);

        vector<D3D12_RESOURCE_BARRIER> barriers;
        unordered_set<AnimationModelResource*> skinnedModels;

        for (EntityID entity : World::GetView<ModelComponent, TransformComponent>())
        {
            const auto& component = ComponentManager::GetComponentUnchecked<ModelComponent>(entity);
            if (component.ModelId < 0)
                continue;

            bool isAnimated = std::is_same_v<ModelComponent, AnimationModelComponent>;
            if (isAnimated && (!ShouldCastShadow(entity) || !LightingResources::ShouldDrawEntityInCurrentShadowPass(entity)))
                continue;
            if (InstancingSystem::CanInstance(entity) || (InstancingSystem::CanInstance(entity) && !InstancingSystem::IsEntityVisible(entity)))
                continue;

            ModelResource* model;
            if (isAnimated)
            {
                model = ModelManager::GetAnimModel(component.ModelId);
            }
            else
            {
                model = ModelManager::GetStaticModel(component.ModelId);
            }
            if (!model)
                continue;

            if (skinnedModels.insert(model).second)
            {
                model->DispatchGpuSkinning(ctx.commandList);
                callbacks.restoreGraphicsState(ctx.commandList, ctx.pso);
            }

            barriers.clear();
            barriers.reserve(model->GetMeshCount());
            for (UINT meshIndex = 0; meshIndex < model->GetMeshCount(); ++meshIndex)
            {
                const MeshDataType& mesh = model->GetMeshData(meshIndex);
                if (callbacks.shouldTransitionMesh(model, meshIndex, mesh))
                {
                    barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
                        mesh.VertexBuffer.Get(),
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER));
                }
            }
            SubmitBarriers(ctx.commandList, barriers);

            const auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
            const XMMATRIX world = XMLoadFloat4x4(&transform.WorldMatrix);
            callbacks.writeConstants(entity, ctx.commandList);
            if (!callbacks.executeIndirect(ctx.commandList, model, entity, world, ctx.viewProjection, ctx.pso, ctx.cpuCulledVirtualPage))
            {
                callbacks.restoreGraphicsState(ctx.commandList, ctx.pso);
                callbacks.fallbackDraw(ctx.commandList, model, entity, world, ctx.pso);
            }

            barriers.clear();
            for (UINT meshIndex = 0; meshIndex < model->GetMeshCount(); ++meshIndex)
            {
                const MeshDataType& mesh = model->GetMeshData(meshIndex);
                if (callbacks.shouldTransitionMesh(model, meshIndex, mesh))
                {
                    barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
                        mesh.VertexBuffer.Get(),
                        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
                }
            }
            SubmitBarriers(ctx.commandList, barriers);
        }
    }

    void DrawShadowMap()
    {
        ID3D12GraphicsCommandList* commandList = GraphicsDevice::GetCommandList();
        ID3D12PipelineState* shadowPso = PsoManager::GetOrCreateShadowMapPso();
        UINT8* constantBufferBegin = FrameConstants::GetConstantBufferPtr();
        ID3D12DescriptorHeap* cbvHeap = FrameConstants::GetCbvHeap();
        if (!commandList || !shadowPso || !constantBufferBegin || !cbvHeap)
            return;

        ID3D12DescriptorHeap* heaps[] = { cbvHeap };
        commandList->SetDescriptorHeaps(_countof(heaps), heaps);
        const XMMATRIX lightViewProjection = LightingResources::GetCurrentShadowViewProjection();
        const bool cpuCulledVirtualPage = LightingResources::IsCurrentShadowPassVirtualPage();

        auto writeShadowConstants = [](EntityID entity, void* cmdList)
        {
            auto* cl = static_cast<ID3D12GraphicsCommandList*>(cmdList);
            auto* cbBegin = FrameConstants::GetConstantBufferPtr();
            const auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
            ConstantBuffer3D constants{};
            constants.World = XMMatrixTranspose(XMLoadFloat4x4(&transform.WorldMatrix));
            constants.UseTexture = 0;
            memcpy(cbBegin + entity * FrameConstants::g_kCB_ALIGNED_SIZE, &constants, sizeof(constants));
            cl->SetGraphicsRootDescriptorTable(0, FrameConstants::GetConstantBufferHandle(entity));
        };

        ModelDrawContext ctx{ commandList, shadowPso, lightViewProjection, cpuCulledVirtualPage };
        ModelCallbacks animCallbacks{
            [](auto cl, auto ps) { RestoreShadowGraphicsState(cl, ps); },
            writeShadowConstants,
            [this](auto cl, auto model, auto entity, auto world, auto vp, auto ps, auto cpu) 
            { return m_IndirectDraws.ExecuteShadow(cl, model, entity, world, vp, ps, cpu); },
            [this](auto cl, auto model, auto entity, auto world, auto ps) { DrawAnimFallback(cl, model, entity, world, ps); },
            [](auto model, auto idx, auto mesh) { return mesh.VertexBuffer != nullptr; }
        };
        ModelCallbacks staticCallbacks{
            [](auto cl, auto ps) { RestoreShadowGraphicsState(cl, ps); },
            writeShadowConstants,
            [this](auto cl, auto model, auto entity, auto world, auto vp, auto ps, auto cpu)
            { return m_IndirectDraws.ExecuteShadow(cl, model, entity, world, vp, ps, cpu); },
            [this](auto cl, auto model, auto entity, auto world, auto ps) { DrawStaticFallback(cl, model, entity, world, ps); },
            [](auto model, auto idx, auto mesh) { return mesh.VertexBuffer != nullptr; }
        };

        DrawModels<AnimationModelComponent, AnimationModelResource, MeshData>(ctx, animCallbacks);
        DrawModels<StaticModelComponent, StaticModelResource, StaticMeshData>(ctx, staticCallbacks);
    }

    void DrawVelocity()
    {
        ID3D12GraphicsCommandList* commandList = GraphicsDevice::GetCommandList();
        ID3D12PipelineState* velocityPso = PsoManager::GetVelocityGeometryPso();
        if (!commandList || !velocityPso)
            return;

        XMMATRIX view = XMMatrixIdentity();
        XMMATRIX projection = XMMatrixIdentity();
        Camera::GetCameraMatrices(Camera::GetCameraEntity(), view, projection);
        const XMMATRIX viewProjection = view * projection;
        const XMMATRIX previousViewProjection =
            XMLoadFloat4x4(&GraphicsDevice::GetPreviousViewMatrix()) *
            XMLoadFloat4x4(&GraphicsDevice::GetPreviousProjectionMatrix());

        auto setConstants = [=](EntityID entity, void* cmdList)
        {
            auto* cl = static_cast<ID3D12GraphicsCommandList*>(cmdList);
            const auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
            ConstantBuffer3D constants{};
            constants.World = XMMatrixTranspose(XMLoadFloat4x4(&transform.WorldMatrix));
            constants.View = XMMatrixTranspose(view);
            constants.Projection = XMMatrixTranspose(projection);
            constants.PreviousWorld = XMMatrixTranspose(XMLoadFloat4x4(&transform.PreviousWorldMatrix));
            constants.PreviousViewProjection = XMMatrixTranspose(previousViewProjection);
            cl->SetGraphicsRootDescriptorTable(0, FrameConstants::AllocateTransientConstantBuffer(constants));
        };

        ModelDrawContext ctx{ commandList, velocityPso, viewProjection, false };
        ModelCallbacks animCallbacks{
            [](auto cl, auto ps) { RestoreVelocityGraphicsState(cl, ps); },
            setConstants,
            [this](auto cl, auto model, auto entity, auto world, auto vp, auto ps, auto)
            { return m_IndirectDraws.ExecuteVelocity(cl, model, entity, world, vp, ps); },
            [this](auto cl, auto model, auto entity, auto world, auto ps) { DrawAnimVelocityFallback(cl, model, entity, world, ps); },
            [](auto model, auto idx, auto mesh) { return mesh.VertexBuffer && mesh.PreviousVertexValid; }
        };
        ModelCallbacks staticCallbacks{
            [](auto cl, auto ps) { RestoreVelocityGraphicsState(cl, ps); },
            setConstants,
            [this](auto cl, auto model, auto entity, auto world, auto vp, auto ps, auto)
            { return m_IndirectDraws.ExecuteVelocity(cl, model, entity, world, vp, ps); },
            [this](auto cl, auto model, auto entity, auto world, auto ps) { DrawStaticVelocityFallback(cl, model, entity, world, ps); },
            [](auto model, auto idx, auto mesh) { return mesh.VertexBuffer && mesh.IndexBuffer && mesh.IndexCount > 0; }
        };

        DrawModels<AnimationModelComponent, AnimationModelResource, MeshData>(ctx, animCallbacks);
        DrawModels<StaticModelComponent, StaticModelResource, StaticMeshData>(ctx, staticCallbacks);
    }

    static void DrawAnimFallback(ID3D12GraphicsCommandList* cmdList, const AnimationModelResource* model, EntityID, const XMMATRIX&, ID3D12PipelineState* pso)
    {
        for (UINT meshIndex = 0; meshIndex < model->GetMeshCount(); ++meshIndex)
        {
            const MeshData& mesh = model->GetMeshData(meshIndex);
            if (!mesh.VertexBuffer || !mesh.IndexBuffer || mesh.IndexCount == 0)
                continue;
            cmdList->IASetVertexBuffers(0, 1, &mesh.VertexBufferView);
            cmdList->IASetIndexBuffer(&mesh.IndexBufferView);
            cmdList->DrawIndexedInstanced(mesh.IndexCount, 1, 0, 0, 0);
        }
    }

    static void DrawStaticFallback(ID3D12GraphicsCommandList* cmdList, const StaticModelResource* model, EntityID, const XMMATRIX&, ID3D12PipelineState* pso)
    {
        for (UINT meshIndex = 0; meshIndex < model->GetMeshCount(); ++meshIndex)
        {
            const StaticMeshData& mesh = model->GetMeshData(meshIndex);
            if (!mesh.VertexBuffer || !mesh.IndexBuffer || mesh.IndexCount == 0)
                continue;
            cmdList->IASetVertexBuffers(0, 1, &mesh.VertexBufferView);
            cmdList->IASetIndexBuffer(&mesh.IndexBufferView);
            cmdList->DrawIndexedInstanced(mesh.IndexCount, 1, 0, 0, 0);
        }
    }

    static void DrawAnimVelocityFallback(ID3D12GraphicsCommandList* cmdList, const AnimationModelResource* model, EntityID, const XMMATRIX&, ID3D12PipelineState* pso)
    {
        for (UINT meshIndex = 0; meshIndex < model->GetMeshCount(); ++meshIndex)
        {
            const MeshData& mesh = model->GetMeshData(meshIndex);
            if (!mesh.VertexBuffer || !mesh.IndexBuffer || !mesh.PreviousVertexValid || mesh.IndexCount == 0)
                continue;
            D3D12_VERTEX_BUFFER_VIEW views[2] = { mesh.VertexBufferView, mesh.PreviousVertexBufferView };
            cmdList->IASetVertexBuffers(0, _countof(views), views);
            cmdList->IASetIndexBuffer(&mesh.IndexBufferView);
            cmdList->DrawIndexedInstanced(mesh.IndexCount, 1, 0, 0, 0);
        }
    }

    static void DrawStaticVelocityFallback(ID3D12GraphicsCommandList* cmdList, const StaticModelResource* model, EntityID, const XMMATRIX&, ID3D12PipelineState* pso)
    {
        for (UINT meshIndex = 0; meshIndex < model->GetMeshCount(); ++meshIndex)
        {
            const StaticMeshData& mesh = model->GetMeshData(meshIndex);
            if (!mesh.VertexBuffer || !mesh.IndexBuffer || mesh.IndexCount == 0)
                continue;
            D3D12_VERTEX_BUFFER_VIEW views[2] = { mesh.VertexBufferView, mesh.VertexBufferView };
            cmdList->IASetVertexBuffers(0, _countof(views), views);
            cmdList->IASetIndexBuffer(&mesh.IndexBufferView);
            cmdList->DrawIndexedInstanced(mesh.IndexCount, 1, 0, 0, 0);
        }
    }

public:
    void Init() override { m_Backend.Init(); }
    void Uninit() override { m_IndirectDraws.Reset(); m_Backend.Uninit(); }
    void Update() override { m_Backend.Update(); }
    void Draw(RenderPass renderPass, bool receivingPostProcessOnly) override
    {
        switch (renderPass)
        {
        case RenderPass::ShadowMap: DrawShadowMap(); break;
        case RenderPass::Velocity: DrawVelocity(); break;
        default:
            m_Backend.SetIndirectDrawCache(&m_IndirectDraws);
            m_Backend.Draw(renderPass, receivingPostProcessOnly);
            break;
        }
    }
};