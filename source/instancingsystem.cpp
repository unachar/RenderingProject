#include "pch.h"
#include "instancingsystem.h"
#include "componentmanager.h"
#include "world.h"
#include "camera.h"
#include "renderercore.h"
#include "rendererdraw.h"
#include "rendererresource.h"
#include "renderershader.h"
#include "psomanager.h"
#include "texturemanager.h"
#include "materialsystem.h"
#include "modelmanager.h"
#include "animationmodel.h"
#include <unordered_map>
#include <unordered_set>

namespace
{
    enum class InstanceKind : UINT8
    {
        Mesh,
        Sprite3D,
        AnimatedMesh
    };

    struct InstanceBatch
    {
        InstanceKind Kind = InstanceKind::Mesh;
        std::vector<EntityID> Entities;
        ID3D12PipelineState* Pso = nullptr;
        D3D12_VERTEX_BUFFER_VIEW VertexBuffer{};
        D3D12_INDEX_BUFFER_VIEW IndexBuffer{};
        UINT VertexCount = 0;
        UINT IndexCount = 0;
        int TextureIndex = -1;
        int NormalIndex = -1;
        const MaterialComponent* Material = nullptr;
        XMFLOAT3 BoundsCenter{};
        XMFLOAT3 BoundsExtents{};
        bool HasBounds = false;
        AnimationModelResource* AnimatedModel = nullptr;
        UINT AnimatedMeshIndex = 0;
    };

    const MaterialComponent& DefaultMaterial()
    {
        static const MaterialComponent material{};
        return material;
    }

    uint64_t HashMaterial(const MaterialComponent* material)
    {
        return material ? RendererResource::GetMaterialBatchHash(*material) : 0;
    }

    const char* ResolvePixelShader(EntityID entity, InstanceKind kind)
    {
        if (ComponentManager::HasComponent<ShaderComponent>(entity))
        {
            const auto& shader = ComponentManager::GetComponentUnchecked<ShaderComponent>(entity);
            if (!shader.PsPath.empty())
            {
                return shader.PsPath.c_str();
            }
        }
        return kind == InstanceKind::Sprite3D
            ? "shader/hlsl/build/colorshader3dPS.cso"
            : "shader/hlsl/build/modelshaderPS.cso";
    }

    bool IsBoundsVisible(EntityID entity, const XMMATRIX& viewProjection, const XMFLOAT3& fallbackCenter,
        const XMFLOAT3& fallbackExtents, bool hasFallbackBounds)
    {
        const auto& instancing = ComponentManager::GetComponentUnchecked<InstancingComponent>(entity);
        if (!instancing.EnableFrustumCulling)
        {
            return true;
        }

        if (!ComponentManager::HasComponent<AABBComponent>(entity) && !hasFallbackBounds)
        {
            return true;
        }

        XMFLOAT3 center = fallbackCenter;
        XMFLOAT3 extents = fallbackExtents;
        if (ComponentManager::HasComponent<AABBComponent>(entity))
        {
            const auto& bounds = ComponentManager::GetComponentUnchecked<AABBComponent>(entity);
            center = bounds.Center;
            extents = bounds.Extents;
        }

        const XMMATRIX world = XMLoadFloat4x4(
            &ComponentManager::GetComponentUnchecked<TransformComponent>(entity).WorldMatrix);
        bool outsideLeft = true;
        bool outsideRight = true;
        bool outsideBottom = true;
        bool outsideTop = true;
        bool outsideNear = true;
        bool outsideFar = true;
        for (UINT corner = 0; corner < 8; ++corner)
        {
            const XMFLOAT3 local =
            {
                center.x + extents.x * ((corner & 1) ? 1.0f : -1.0f),
                center.y + extents.y * ((corner & 2) ? 1.0f : -1.0f),
                center.z + extents.z * ((corner & 4) ? 1.0f : -1.0f)
            };
            const XMVECTOR clip = XMVector4Transform(
                XMVectorSet(local.x, local.y, local.z, 1.0f),
                world * viewProjection);
            XMFLOAT4 p{};
            XMStoreFloat4(&p, clip);
            outsideLeft &= p.x < -p.w;
            outsideRight &= p.x > p.w;
            outsideBottom &= p.y < -p.w;
            outsideTop &= p.y > p.w;
            outsideNear &= p.z < 0.0f;
            outsideFar &= p.z > p.w;
        }
        return !(outsideLeft || outsideRight || outsideBottom || outsideTop || outsideNear || outsideFar);
    }

    string MakeKey(
        EntityID entity,
        InstanceKind kind,
        ID3D12PipelineState* pso,
        D3D12_GPU_VIRTUAL_ADDRESS vertexBuffer,
        uint64_t geometryHash,
        UINT vertexCount,
        D3D12_GPU_VIRTUAL_ADDRESS indexBuffer,
        UINT indexCount,
        int textureIndex,
        int normalIndex,
        const MaterialComponent* material,
        UINT meshIndex = 0,
        const AnimationModelComponent* animation = nullptr)
    {
        const auto& instancing = ComponentManager::GetComponentUnchecked<InstancingComponent>(entity);
        const uint64_t geometryIdentity = instancing.GroupId != 0
            ? static_cast<uint64_t>(instancing.GroupId)
            : (geometryHash != 0 ? geometryHash : vertexBuffer);
        string key = to_string(static_cast<UINT>(kind)) + "|" +
            to_string(reinterpret_cast<uintptr_t>(pso)) + "|" +
            to_string(geometryIdentity) + "|" + to_string(vertexCount) + "|" +
            to_string(indexBuffer) + "|" + to_string(indexCount) + "|" +
            to_string(textureIndex) + "|" + to_string(normalIndex) + "|" +
            to_string(HashMaterial(material)) + "|" + to_string(meshIndex) + "|" +
            to_string(instancing.GroupId);
        if (animation)
        {
            key += "|" + to_string(animation->ModelId) + "|" + animation->CurrentAnimation + "|" +
                animation->NextAnimation + "|" + to_string(animation->CurrentTime) + "|" +
                to_string(animation->NextTime) + "|" + to_string(animation->BlendRate);
            for (const auto& layer : animation->ActiveAnimationLayers)
            {
                key += "|" + layer.AnimationName + "|" + to_string(layer.CurrentTime);
            }
        }
        return key;
    }
}

bool InstancingSystem::CanInstance(EntityID entity)
{
    if (!s_Available ||
        !ComponentManager::HasComponent<InstancingComponent>(entity) ||
        !ComponentManager::GetComponentUnchecked<InstancingComponent>(entity).UseInstancing ||
        !ComponentManager::HasComponent<TransformComponent>(entity))
    {
        return false;
    }

    if (ComponentManager::HasComponent<AnimationModelComponent>(entity))
    {
        return true;
    }
    if (ComponentManager::HasComponent<SpriteComponent>(entity))
    {
        return ComponentManager::GetComponentUnchecked<SpriteComponent>(entity).Is3D;
    }
    return ComponentManager::HasComponent<MeshComponent>(entity) &&
        !ComponentManager::HasComponent<StaticModelComponent>(entity);
}

bool InstancingSystem::IsEntityVisible(EntityID entity)
{
    if (!CanInstance(entity))
    {
        return true;
    }

    XMFLOAT3 center{};
    XMFLOAT3 extents{};
    bool hasBounds = false;
    if (ComponentManager::HasComponent<AABBComponent>(entity))
    {
        const auto& bounds = ComponentManager::GetComponentUnchecked<AABBComponent>(entity);
        center = bounds.Center;
        extents = bounds.Extents;
        hasBounds = true;
    }
    else if (ComponentManager::HasComponent<AnimationModelComponent>(entity))
    {
        const auto& animation = ComponentManager::GetComponentUnchecked<AnimationModelComponent>(entity);
        if (AnimationModelResource* model = ModelManager::GetAnimModel(animation.ModelId))
        {
            center = model->GetAabbCenter();
            extents = model->GetAabbExtents();
            hasBounds = true;
        }
    }
    else if (ComponentManager::HasComponent<SpriteComponent>(entity))
    {
        const auto& sprite = ComponentManager::GetComponentUnchecked<SpriteComponent>(entity);
        center = sprite.LocalBoundsCenter;
        extents = sprite.LocalBoundsExtents;
        hasBounds = sprite.HasLocalBounds;
    }
    else if (ComponentManager::HasComponent<MeshComponent>(entity))
    {
        const auto& mesh = ComponentManager::GetComponentUnchecked<MeshComponent>(entity);
        center = mesh.LocalBoundsCenter;
        extents = mesh.LocalBoundsExtents;
        hasBounds = mesh.HasLocalBounds;
    }

    XMMATRIX view = XMMatrixIdentity();
    XMMATRIX projection = XMMatrixIdentity();
    Camera::GetCameraMatrices(Camera::GetCameraEntity(), view, projection);
    return IsBoundsVisible(entity, view * projection, center, extents, hasBounds);
}

void InstancingSystem::Init()
{
    ID3D12Device* device = RendererCore::GetDevice();
    if (!device)
    {
        return;
    }
    const UINT64 count = static_cast<UINT64>(RendererState::g_kFRAME_COUNT) * kMaxInstancesPerFrame;
    const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    const auto desc = CD3DX12_RESOURCE_DESC::Buffer(count * sizeof(InstanceTransform));
    if (FAILED(device->CreateCommittedResource(
        &heap,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_InstanceUpload))))
    {
        return;
    }
    const D3D12_RANGE noRead{ 0, 0 };
    if (FAILED(m_InstanceUpload->Map(0, &noRead, reinterpret_cast<void**>(&m_MappedInstances))))
    {
        m_InstanceUpload.Reset();
        return;
    }
    s_Available = true;
}

void InstancingSystem::Uninit()
{
    s_Available = false;
    if (m_InstanceUpload && m_MappedInstances)
    {
        m_InstanceUpload->Unmap(0, nullptr);
    }
    m_MappedInstances = nullptr;
    m_InstanceUpload.Reset();
}

void InstancingSystem::Draw(RenderPass renderPass, bool receivingPostProcessOnly)
{
    if (!s_Available || !m_MappedInstances ||
        (renderPass != RenderPass::PrimaryScene && renderPass != RenderPass::OverlayScene))
    {
        return;
    }

    ID3D12GraphicsCommandList* commandList = RendererCore::GetCommandList();
    ID3D12DescriptorHeap* heap = RendererResource::GetCbvHeap();
    if (!commandList || !heap)
    {
        return;
    }

    const UINT frameIndex = RendererCore::GetFrameIndex() % RendererState::g_kFRAME_COUNT;
    if (m_FrameIndex != frameIndex)
    {
        m_FrameIndex = frameIndex;
        m_FrameCursor = 0;
    }

    XMMATRIX view = XMMatrixIdentity();
    XMMATRIX projection = XMMatrixIdentity();
    Camera::GetCameraMatrices(Camera::GetCameraEntity(), view, projection);
    const XMMATRIX viewProjection = view * projection;
    const bool transparentPass = renderPass == RenderPass::OverlayScene;
    const bool deferredOpaque = RendererCore::GetRenderMode() == RenderMode::DEFERRED && renderPass == RenderPass::PrimaryScene;
    const int defaultTexture = TextureManager::GetDefaultTextureIndex();

    std::vector<InstanceBatch> batches;
    std::unordered_map<string, size_t> batchLookup;
    auto add = [&](EntityID entity, InstanceBatch prototype, const string& key)
    {
        auto it = batchLookup.find(key);
        if (it == batchLookup.end())
        {
            const size_t index = batches.size();
            batchLookup.emplace(key, index);
            prototype.Entities.push_back(entity);
            batches.push_back(std::move(prototype));
        }
        else
        {
            batches[it->second].Entities.push_back(entity);
        }
    };

    auto acceptsPass = [&](EntityID entity, const MaterialComponent* material)
    {
        const bool transparent = material && MaterialSystem::IsTransparentMaterial(*material);
        if (transparent != transparentPass)
        {
            return false;
        }
        return transparentPass || deferredOpaque ||
            MaterialSystem::IsReceivingPostProcess(entity) == receivingPostProcessOnly;
    };

    for (EntityID entity : World::GetView<InstancingComponent, SpriteComponent, TransformComponent>())
    {
        if (!CanInstance(entity)) continue;
        const auto& sprite = ComponentManager::GetComponentUnchecked<SpriteComponent>(entity);
        if (!sprite.Is3D || sprite.VertexBufferView.BufferLocation == 0 || sprite.VertexCount == 0) continue;
        if (!IsBoundsVisible(entity, viewProjection, sprite.LocalBoundsCenter,
            sprite.LocalBoundsExtents, sprite.HasLocalBounds)) continue;
        const MaterialComponent* material = ComponentManager::HasComponent<MaterialComponent>(entity)
            ? &ComponentManager::GetComponentUnchecked<MaterialComponent>(entity) : nullptr;
        if (!acceptsPass(entity, material)) continue;
        const int texture = material && material->UseTexture && material->TextureID >= 0 ? material->TextureID : defaultTexture;
        const int normal = material && material->NormalMapID >= 0 ? material->NormalMapID : defaultTexture;
        rendererResource resource{};
        resource.vsPath = "shader/hlsl/build/colorshader3dInstancedVS.cso";
        resource.psPath = ResolvePixelShader(entity, InstanceKind::Sprite3D);
        resource.isModel = true;
        resource.enableAlphaBlend = transparentPass;
        ID3D12PipelineState* pso = PsoManager::GetOrCreateGraphicsPso(resource);
        if (!pso) continue;
        InstanceBatch batch{};
        batch.Kind = InstanceKind::Sprite3D;
        batch.Pso = pso;
        batch.VertexBuffer = sprite.VertexBufferView;
        batch.VertexCount = sprite.VertexCount;
        batch.TextureIndex = texture;
        batch.NormalIndex = normal;
        batch.Material = material;
        batch.BoundsCenter = sprite.LocalBoundsCenter;
        batch.BoundsExtents = sprite.LocalBoundsExtents;
        batch.HasBounds = sprite.HasLocalBounds;
        add(entity, batch, MakeKey(entity, batch.Kind, pso, batch.VertexBuffer.BufferLocation, sprite.GeometryHash, batch.VertexCount, 0, 0, texture, normal, material));
    }

    for (EntityID entity : World::GetView<InstancingComponent, MeshComponent, TransformComponent>())
    {
        if (!CanInstance(entity) || ComponentManager::HasComponent<AnimationModelComponent>(entity)) continue;
        const auto& mesh = ComponentManager::GetComponentUnchecked<MeshComponent>(entity);
        if (mesh.VertexBufferView.BufferLocation == 0 || mesh.VertexCount == 0) continue;
        if (!IsBoundsVisible(entity, viewProjection, mesh.LocalBoundsCenter,
            mesh.LocalBoundsExtents, mesh.HasLocalBounds)) continue;
        const MaterialComponent* material = ComponentManager::HasComponent<MaterialComponent>(entity)
            ? &ComponentManager::GetComponentUnchecked<MaterialComponent>(entity) : nullptr;
        if (!acceptsPass(entity, material)) continue;
        const int texture = material && material->UseTexture && material->TextureID >= 0 ? material->TextureID : defaultTexture;
        const int normal = material && material->NormalMapID >= 0 ? material->NormalMapID : defaultTexture;
        rendererResource resource{};
        resource.vsPath = "shader/hlsl/build/modelshaderInstancedVS.cso";
        resource.psPath = ResolvePixelShader(entity, InstanceKind::Mesh);
        resource.isModel = true;
        resource.enableAlphaBlend = transparentPass;
        ID3D12PipelineState* pso = PsoManager::GetOrCreateGraphicsPso(resource);
        if (!pso) continue;
        InstanceBatch batch{};
        batch.Kind = InstanceKind::Mesh;
        batch.Pso = pso;
        batch.VertexBuffer = mesh.VertexBufferView;
        batch.VertexCount = mesh.VertexCount;
        batch.TextureIndex = texture;
        batch.NormalIndex = normal;
        batch.Material = material;
        batch.BoundsCenter = mesh.LocalBoundsCenter;
        batch.BoundsExtents = mesh.LocalBoundsExtents;
        batch.HasBounds = mesh.HasLocalBounds;
        add(entity, batch, MakeKey(entity, batch.Kind, pso, batch.VertexBuffer.BufferLocation, mesh.GeometryHash, batch.VertexCount, 0, 0, texture, normal, material));
    }

    for (EntityID entity : World::GetView<InstancingComponent, AnimationModelComponent, TransformComponent>())
    {
        if (!CanInstance(entity)) continue;
        const auto& animation = ComponentManager::GetComponentUnchecked<AnimationModelComponent>(entity);
        AnimationModelResource* model = ModelManager::GetAnimModel(animation.ModelId);
        if (!model) continue;
        if (!IsBoundsVisible(entity, viewProjection, model->GetAabbCenter(),
            model->GetAabbExtents(), true)) continue;
        const MaterialComponent* material = ComponentManager::HasComponent<MaterialComponent>(entity)
            ? &ComponentManager::GetComponentUnchecked<MaterialComponent>(entity) : nullptr;
        if (!acceptsPass(entity, material)) continue;
        const int entityTexture = material && material->UseTexture && material->TextureID >= 0 ? material->TextureID : defaultTexture;
        const int normal = material && material->NormalMapID >= 0 ? material->NormalMapID : defaultTexture;
        for (UINT meshIndex = 0; meshIndex < model->GetMeshCount(); ++meshIndex)
        {
            const MeshData& mesh = model->GetMeshData(meshIndex);
            if (!mesh.VertexBuffer || mesh.VertexBufferView.BufferLocation == 0 ||
                !mesh.IndexBuffer || mesh.IndexBufferView.BufferLocation == 0 || mesh.IndexCount == 0) continue;
            const int texture = mesh.TextureIndex >= 0 ? mesh.TextureIndex : entityTexture;
            rendererResource resource{};
            resource.vsPath = "shader/hlsl/build/modelshaderInstancedVS.cso";
            resource.psPath = ResolvePixelShader(entity, InstanceKind::AnimatedMesh);
            resource.isModel = true;
            resource.enableAlphaBlend = transparentPass;
            ID3D12PipelineState* pso = PsoManager::GetOrCreateGraphicsPso(resource);
            if (!pso) continue;
            InstanceBatch batch{};
            batch.Kind = InstanceKind::AnimatedMesh;
            batch.Pso = pso;
            batch.VertexBuffer = mesh.VertexBufferView;
            batch.VertexCount = mesh.VertexCount;
            batch.IndexBuffer = mesh.IndexBufferView;
            batch.IndexCount = mesh.IndexCount;
            batch.TextureIndex = texture;
            batch.NormalIndex = normal;
            batch.Material = material;
            batch.BoundsCenter = model->GetAabbCenter();
            batch.BoundsExtents = model->GetAabbExtents();
            batch.HasBounds = true;
            batch.AnimatedModel = model;
            batch.AnimatedMeshIndex = meshIndex;
            add(entity, batch, MakeKey(entity, batch.Kind, pso, batch.VertexBuffer.BufferLocation, 0, batch.VertexCount,
                batch.IndexBuffer.BufferLocation, batch.IndexCount, texture, normal, material, meshIndex, &animation));
        }
    }

    std::unordered_set<AnimationModelResource*> skinned;
    const UINT descriptorIncrement = RendererResource::GetCbvIncrementSize();
    const auto descriptorStart = heap->GetGPUDescriptorHandleForHeapStart();
    std::unordered_map<string, UINT> transformRanges;
    RendererDraw::BeginModelPass();
    for (auto& batch : batches)
    {
        std::vector<EntityID> visible;
        visible.reserve(batch.Entities.size());
        for (EntityID entity : batch.Entities)
        {
            if (IsBoundsVisible(entity, viewProjection, batch.BoundsCenter, batch.BoundsExtents, batch.HasBounds))
            {
                visible.push_back(entity);
            }
        }
        if (visible.empty())
        {
            continue;
        }

        string transformKey;
        transformKey.reserve(visible.size() * 6);
        for (EntityID entity : visible)
        {
            transformKey += to_string(entity) + ",";
        }

        UINT firstInstance = 0;
        const auto existingRange = transformRanges.find(transformKey);
        if (existingRange != transformRanges.end())
        {
            firstInstance = existingRange->second;
        }
        else
        {
            if (m_FrameCursor + visible.size() > kMaxInstancesPerFrame)
            {
                continue;
            }
            firstInstance = m_FrameCursor;
            InstanceTransform* destination = m_MappedInstances +
                static_cast<UINT64>(frameIndex) * kMaxInstancesPerFrame + firstInstance;
            for (EntityID entity : visible)
            {
                destination->World = ComponentManager::GetComponentUnchecked<TransformComponent>(entity).WorldMatrix;
                ++destination;
            }
            m_FrameCursor += static_cast<UINT>(visible.size());
            transformRanges.emplace(std::move(transformKey), firstInstance);
        }

        // Do not dispatch skinning, transition vertex buffers, or issue any GBuffer
        // draw when every instance in this model batch was culled.
        if (batch.AnimatedModel && skinned.insert(batch.AnimatedModel).second)
        {
            batch.AnimatedModel->DispatchGpuSkinning(commandList);
        }

        D3D12_RESOURCE_BARRIER animatedReady{};
        if (batch.AnimatedModel)
        {
            const MeshData& mesh = batch.AnimatedModel->GetMeshData(batch.AnimatedMeshIndex);
            animatedReady = CD3DX12_RESOURCE_BARRIER::Transition(
                mesh.VertexBuffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
            commandList->ResourceBarrier(1, &animatedReady);
        }

        const EntityID representative = visible.front();
        commandList->SetPipelineState(batch.Pso);
        commandList->SetGraphicsRootDescriptorTable(0, RendererResource::GetConstantBufferHandle(representative));
        commandList->SetGraphicsRootDescriptorTable(1,
            CD3DX12_GPU_DESCRIPTOR_HANDLE(descriptorStart, batch.TextureIndex, descriptorIncrement));
        commandList->SetGraphicsRootDescriptorTable(6,
            CD3DX12_GPU_DESCRIPTOR_HANDLE(descriptorStart, batch.NormalIndex, descriptorIncrement));
        RendererResource::SetMaterial(representative, batch.Material ? *batch.Material : DefaultMaterial());
        const UINT64 frameBase = static_cast<UINT64>(frameIndex) * kMaxInstancesPerFrame;
        commandList->SetGraphicsRootShaderResourceView(
            9,
            m_InstanceUpload->GetGPUVirtualAddress() +
            (frameBase + firstInstance) * sizeof(InstanceTransform));
        commandList->IASetVertexBuffers(0, 1, &batch.VertexBuffer);
        commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        if (batch.IndexCount != 0)
        {
            commandList->IASetIndexBuffer(&batch.IndexBuffer);
            commandList->DrawIndexedInstanced(batch.IndexCount, static_cast<UINT>(visible.size()), 0, 0, 0);
        }
        else
        {
            commandList->IASetIndexBuffer(nullptr);
            commandList->DrawInstanced(batch.VertexCount, static_cast<UINT>(visible.size()), 0, 0);
        }

        if (batch.AnimatedModel)
        {
            const MeshData& mesh = batch.AnimatedModel->GetMeshData(batch.AnimatedMeshIndex);
            const auto back = CD3DX12_RESOURCE_BARRIER::Transition(
                mesh.VertexBuffer.Get(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            commandList->ResourceBarrier(1, &back);
        }
    }
}
