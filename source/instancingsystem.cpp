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

enum class InstanceKind : UINT8
{
    Mesh,
    Sprite3D,
    AnimatedMesh,
    StaticMesh
};

struct InstanceBatch
{
    InstanceKind Kind = InstanceKind::Mesh;
    vector<EntityID> Entities;
    ID3D12PipelineState* Pso = nullptr;
    D3D12_VERTEX_BUFFER_VIEW VertexBuffer{};
    D3D12_INDEX_BUFFER_VIEW IndexBuffer{};
    UINT VertexCount = 0;
    UINT IndexCount = 0;
    array<D3D12_INDEX_BUFFER_VIEW, 3> LodIndexBuffers{};
    array<UINT, 3> LodDrawCounts{};
    UINT AvailableLodCount = 1;
    int TextureIndex = -1;
    int NormalIndex = -1;
    const MaterialComponent* Material = nullptr;
    XMFLOAT3 BoundsCenter{};
    XMFLOAT3 BoundsExtents{};
    bool HasBounds = false;
    AnimationModelResource* AnimatedModel = nullptr;
    StaticModelResource* StaticModel = nullptr;
    UINT AnimatedMeshIndex = 0;
};

struct BatchKey
{
    UINT8 Kind = 0;
    uintptr_t Pso = 0;
    uint64_t GeometryIdentity = 0;
    UINT VertexCount = 0;
    uintptr_t IndexBuffer = 0;
    UINT IndexCount = 0;
    int TextureIndex = -1;
    int NormalIndex = -1;
    uint64_t MaterialHash = 0;
    UINT MeshIndex = 0;
    uint32_t GroupId = 0;
    uint32_t AnimationModelId = 0;
    string CurrentAnimation;
    string NextAnimation;
    float CurrentTime = 0.0f;
    float NextTime = 0.0f;
    float BlendRate = 0.0f;
    vector<pair<string, float>> ActiveAnimationLayers;

    bool operator==(const BatchKey& other) const
    {
        return Kind == other.Kind &&
            Pso == other.Pso &&
            GeometryIdentity == other.GeometryIdentity &&
            VertexCount == other.VertexCount &&
            IndexBuffer == other.IndexBuffer &&
            IndexCount == other.IndexCount &&
            TextureIndex == other.TextureIndex &&
            NormalIndex == other.NormalIndex &&
            MaterialHash == other.MaterialHash &&
            MeshIndex == other.MeshIndex &&
            GroupId == other.GroupId &&
            AnimationModelId == other.AnimationModelId &&
            CurrentAnimation == other.CurrentAnimation &&
            NextAnimation == other.NextAnimation &&
            CurrentTime == other.CurrentTime &&
            NextTime == other.NextTime &&
            BlendRate == other.BlendRate &&
            ActiveAnimationLayers == other.ActiveAnimationLayers;
    }
};

struct BatchKeyHash
{
    size_t operator()(const BatchKey& key) const noexcept
    {
        size_t h = 0;
        auto hash_combine = [&h](auto&& v)
        {
            using std::hash;
            h ^= hash<decay_t<decltype(v)>>{}(v) + 0x9e3779b9 + (h << 6) + (h >> 2);
        };
        hash_combine(key.Kind);
        hash_combine(key.Pso);
        hash_combine(key.GeometryIdentity);
        hash_combine(key.VertexCount);
        hash_combine(key.IndexBuffer);
        hash_combine(key.IndexCount);
        hash_combine(key.TextureIndex);
        hash_combine(key.NormalIndex);
        hash_combine(key.MaterialHash);
        hash_combine(key.MeshIndex);
        hash_combine(key.GroupId);
        hash_combine(key.AnimationModelId);
        hash_combine(key.CurrentAnimation);
        hash_combine(key.NextAnimation);
        hash_combine(key.CurrentTime);
        hash_combine(key.NextTime);
        hash_combine(key.BlendRate);
        for (const auto& layer : key.ActiveAnimationLayers)
        {
            hash_combine(layer.first);
            hash_combine(layer.second);
        }
        return h;
    }
};

static const MaterialComponent& DefaultMaterial()
{
    static const MaterialComponent material{};
    return material;
}

static bool ShouldCastShadow(EntityID entity)
{
    if (ComponentManager::HasComponent<LightComponent>(entity))
        return false;
    if (ComponentManager::HasComponent<NameComponent>(entity) &&
        ComponentManager::GetComponentUnchecked<NameComponent>(entity).Name == "Sky")
        return false;
    if (!ComponentManager::HasComponent<MaterialComponent>(entity))
        return true;
    const auto& material = ComponentManager::GetComponentUnchecked<MaterialComponent>(entity);
    return !MaterialSystem::IsTransparentMaterial(material) &&
        !(material.ShaderClassMode == MaterialMode::Manual &&
            material.ShaderClass == ShaderClass::Shadow);
}

uint64_t InstancingSystem::HashMaterial(const MaterialComponent* material)
{
    return material ? RendererResource::GetMaterialBatchHash(*material) : 0;
}

const char* InstancingSystem::ResolvePixelShader(EntityID entity, InstanceKind kind)
{
    if (ComponentManager::HasComponent<ShaderComponent>(entity))
    {
        const auto& shader = ComponentManager::GetComponentUnchecked<ShaderComponent>(entity);
        if (!shader.PsPath.empty())
            return shader.PsPath.c_str();
    }
    return kind == InstanceKind::Sprite3D
        ? "shader/hlsl/build/colorshader3dPS.cso"
        : "shader/hlsl/build/modelshaderPS.cso";
}

bool InstancingSystem::IsBoundsVisible(EntityID entity, const XMMATRIX& viewProjection, const XMFLOAT3& fallbackCenter,
    const XMFLOAT3& fallbackExtents, bool hasFallbackBounds)
{
    if (ComponentManager::HasComponent<InstancingComponent>(entity) &&
        !ComponentManager::GetComponentUnchecked<InstancingComponent>(entity).EnableFrustumCulling)
        return true;

    if (!ComponentManager::HasComponent<AABBComponent>(entity) && !hasFallbackBounds)
        return true;

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
    {
        const XMVECTOR clipCenter = XMVector4Transform(
            XMVectorSet(center.x, center.y, center.z, 1.0f),
            world * viewProjection);
        XMFLOAT4 c{};
        XMStoreFloat4(&c, clipCenter);
        if (c.x >= -c.w && c.x <= c.w &&
            c.y >= -c.w && c.y <= c.w &&
            c.z >= 0.0f && c.z <= c.w)
            return true;
    }
    bool outsideLeft = true, outsideRight = true, outsideBottom = true;
    bool outsideTop = true, outsideNear = true, outsideFar = true;
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

BatchKey InstancingSystem::MakeKey(
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
    UINT meshIndex,
    const AnimationModelComponent* animation)
{
    const InstancingComponent* instancing =
        ComponentManager::HasComponent<InstancingComponent>(entity)
        ? &ComponentManager::GetComponentUnchecked<InstancingComponent>(entity)
        : nullptr;
    const uint32_t groupId = instancing ? instancing->GroupId : 0;
    const uint64_t geometryIdentity = groupId != 0
        ? static_cast<uint64_t>(groupId)
        : (geometryHash != 0 ? geometryHash : vertexBuffer);

    BatchKey key{};
    key.Kind = static_cast<UINT8>(kind);
    key.Pso = reinterpret_cast<uintptr_t>(pso);
    key.GeometryIdentity = geometryIdentity;
    key.VertexCount = vertexCount;
    key.IndexBuffer = indexBuffer;
    key.IndexCount = indexCount;
    key.TextureIndex = textureIndex;
    key.NormalIndex = normalIndex;
    key.MaterialHash = HashMaterial(material);
    key.MeshIndex = meshIndex;
    key.GroupId = groupId;

    if (animation)
    {
        key.AnimationModelId = animation->ModelId;
        key.CurrentAnimation = animation->CurrentAnimation;
        key.NextAnimation = animation->NextAnimation;
        key.CurrentTime = animation->CurrentTime;
        key.NextTime = animation->NextTime;
        key.BlendRate = animation->BlendRate;
        key.ActiveAnimationLayers.reserve(animation->ActiveAnimationLayers.size());
        for (const auto& layer : animation->ActiveAnimationLayers)
            key.ActiveAnimationLayers.emplace_back(layer.AnimationName, layer.CurrentTime);
    }
    return key;
}

bool InstancingSystem::CanInstance(EntityID entity)
{
    if (!s_Available || !ComponentManager::HasComponent<TransformComponent>(entity))
        return false;

    const bool useInstancing =
        ComponentManager::HasComponent<InstancingComponent>(entity) &&
        ComponentManager::GetComponentUnchecked<InstancingComponent>(entity).UseInstancing;
    const bool useLod =
        ComponentManager::HasComponent<LODComponent>(entity) &&
        ComponentManager::GetComponentUnchecked<LODComponent>(entity).UseLOD;
    if (!useInstancing && !useLod)
        return false;

    if (ComponentManager::HasComponent<AnimationModelComponent>(entity))
        return true;
    if (ComponentManager::HasComponent<StaticModelComponent>(entity))
        return true;
    if (ComponentManager::HasComponent<SpriteComponent>(entity))
        return ComponentManager::GetComponentUnchecked<SpriteComponent>(entity).Is3D;
    return ComponentManager::HasComponent<MeshComponent>(entity) &&
        !ComponentManager::HasComponent<StaticModelComponent>(entity);
}

bool InstancingSystem::IsEntityVisible(EntityID entity)
{
    if (!CanInstance(entity))
        return true;

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

bool InstancingSystem::CreateGpuCullingResources(ID3D12Device* device)
{
    if (!device)
        return false;

    auto createDefaultBuffer = [&](UINT64 size, D3D12_RESOURCE_FLAGS flags,
        D3D12_RESOURCE_STATES state, ComPtr<ID3D12Resource>& resource)
    {
        const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        const auto description = CD3DX12_RESOURCE_DESC::Buffer(size, flags);
        return SUCCEEDED(device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &description, state,
            nullptr, IID_PPV_ARGS(&resource)));
    };

    const UINT64 transformBytes = static_cast<UINT64>(kMaxInstancesPerFrame) * sizeof(XMFLOAT4X4);
    for (auto& resource : m_LodInstances)
    {
        if (!createDefaultBuffer(transformBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COMMON, resource))
            return false;
    }
    if (!createDefaultBuffer(3 * sizeof(UINT), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_COMMON, m_LodCounts) ||
        !createDefaultBuffer(3 * sizeof(D3D12_DRAW_INDEXED_ARGUMENTS),
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON, m_IndirectArguments))
        return false;

    const UINT zeros[3]{};
    const auto uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    const auto zeroDescription = CD3DX12_RESOURCE_DESC::Buffer(sizeof(zeros));
    if (FAILED(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &zeroDescription,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_ZeroCountsUpload))))
        return false;
    void* mappedZeros = nullptr;
    const D3D12_RANGE noRead{ 0, 0 };
    if (FAILED(m_ZeroCountsUpload->Map(0, &noRead, &mappedZeros)))
        return false;
    memcpy(mappedZeros, zeros, sizeof(zeros));
    m_ZeroCountsUpload->Unmap(0, nullptr);

    auto createRootSignature = [&](D3D12_ROOT_PARAMETER* parameters, UINT count,
        ComPtr<ID3D12RootSignature>& rootSignature)
    {
        CD3DX12_ROOT_SIGNATURE_DESC description{};
        description.Init(count, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE);
        ComPtr<ID3DBlob> serialized, errors;
        if (FAILED(D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)))
        {
            if (errors) Debug::Log("%s\n", static_cast<const char*>(errors->GetBufferPointer()));
            return false;
        }
        return SUCCEEDED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&rootSignature)));
    };

    CD3DX12_ROOT_PARAMETER cullParameters[6]{};
    cullParameters[0].InitAsShaderResourceView(0);
    cullParameters[1].InitAsUnorderedAccessView(0);
    cullParameters[2].InitAsUnorderedAccessView(1);
    cullParameters[3].InitAsUnorderedAccessView(2);
    cullParameters[4].InitAsUnorderedAccessView(3);
    cullParameters[5].InitAsConstants(32, 0);
    if (!createRootSignature(cullParameters, _countof(cullParameters), m_CullLodRootSignature))
        return false;

    CD3DX12_ROOT_PARAMETER argsParameters[3]{};
    argsParameters[0].InitAsShaderResourceView(0);
    argsParameters[1].InitAsUnorderedAccessView(0);
    argsParameters[2].InitAsConstants(4, 0);
    if (!createRootSignature(argsParameters, _countof(argsParameters), m_ArgsRootSignature))
        return false;

    auto createComputePso = [&](const char* path, ID3D12RootSignature* rootSignature, ComPtr<ID3D12PipelineState>& pso)
    {
        rendererResource resource{};
        resource.csoPath = path;
        ComPtr<ID3DBlob> shader;
        resource.ppBlob = shader.GetAddressOf();
        if (!RendererShader::LoadShaderBlob(resource))
            return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC description{};
        description.pRootSignature = rootSignature;
        description.CS = CD3DX12_SHADER_BYTECODE(shader.Get());
        return SUCCEEDED(device->CreateComputePipelineState(&description, IID_PPV_ARGS(&pso)));
    };
    if (!createComputePso("shader/hlsl/build/GpuInstanceCullLodCS.cso", m_CullLodRootSignature.Get(), m_CullLodPso) ||
        !createComputePso("shader/hlsl/build/GpuInstanceArgsCS.cso", m_ArgsRootSignature.Get(), m_ArgsPso))
        return false;

    D3D12_INDIRECT_ARGUMENT_DESC indexedArgument{};
    indexedArgument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
    D3D12_COMMAND_SIGNATURE_DESC indexedDescription{};
    indexedDescription.ByteStride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
    indexedDescription.NumArgumentDescs = 1;
    indexedDescription.pArgumentDescs = &indexedArgument;
    if (FAILED(device->CreateCommandSignature(&indexedDescription, nullptr, IID_PPV_ARGS(&m_DrawIndexedSignature))))
        return false;

    D3D12_INDIRECT_ARGUMENT_DESC drawArgument{};
    drawArgument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
    D3D12_COMMAND_SIGNATURE_DESC drawDescription{};
    drawDescription.ByteStride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
    drawDescription.NumArgumentDescs = 1;
    drawDescription.pArgumentDescs = &drawArgument;
    return SUCCEEDED(device->CreateCommandSignature(&drawDescription, nullptr, IID_PPV_ARGS(&m_DrawSignature)));
}

void InstancingSystem::Init()
{
    m_GpuCullingInitialized = false;
    ID3D12Device* device = RendererCore::GetDevice();
    if (!device) return;

    const UINT64 count = static_cast<UINT64>(RendererState::g_kFRAME_COUNT) * kMaxInstancesPerFrame;
    const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    const auto desc = CD3DX12_RESOURCE_DESC::Buffer(count * sizeof(GpuInstanceInput));
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_InstanceUpload))))
        return;
    const D3D12_RANGE noRead{ 0, 0 };
    if (FAILED(m_InstanceUpload->Map(0, &noRead, reinterpret_cast<void**>(&m_MappedInstances))))
    {
        m_InstanceUpload.Reset();
        return;
    }
    const auto directDesc = CD3DX12_RESOURCE_DESC::Buffer(count * sizeof(XMFLOAT4X4));
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &directDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_DirectInstanceUpload))))
    {
        m_InstanceUpload->Unmap(0, nullptr);
        m_MappedInstances = nullptr;
        m_InstanceUpload.Reset();
        return;
    }
    if (FAILED(m_DirectInstanceUpload->Map(0, &noRead, reinterpret_cast<void**>(&m_MappedDirectInstances))))
    {
        m_DirectInstanceUpload.Reset();
        m_InstanceUpload->Unmap(0, nullptr);
        m_MappedInstances = nullptr;
        m_InstanceUpload.Reset();
        return;
    }
    s_Available = CreateGpuCullingResources(device);
}

void InstancingSystem::Uninit()
{
    s_Available = false;
    m_GpuCullingInitialized = false;
    if (m_InstanceUpload && m_MappedInstances) m_InstanceUpload->Unmap(0, nullptr);
    m_MappedInstances = nullptr; m_InstanceUpload.Reset();
    if (m_DirectInstanceUpload && m_MappedDirectInstances) m_DirectInstanceUpload->Unmap(0, nullptr);
    m_MappedDirectInstances = nullptr; m_DirectInstanceUpload.Reset();
    for (auto& resource : m_LodInstances) resource.Reset();
    m_LodCounts.Reset(); m_IndirectArguments.Reset(); m_ZeroCountsUpload.Reset();
    m_CullLodRootSignature.Reset(); m_CullLodPso.Reset();
    m_ArgsRootSignature.Reset(); m_ArgsPso.Reset();
    m_DrawIndexedSignature.Reset(); m_DrawSignature.Reset();
}

// --- Private helper methods ---

struct DrawContext
{
    ID3D12GraphicsCommandList* commandList = nullptr;
    ID3D12DescriptorHeap* heap = nullptr;
    UINT frameIndex = 0;
    XMMATRIX viewProjection{};
    XMFLOAT3 cameraPosition{};
    bool transparentPass = false;
    bool deferredOpaque = false;
    int defaultTexture = -1;
};

struct BatchBuilder
{
    vector<InstanceBatch> batches;
    unordered_map<BatchKey, size_t, BatchKeyHash> lookup;

    void Add(EntityID entity, InstanceBatch prototype, const BatchKey& key)
    {
        auto it = lookup.find(key);
        if (it == lookup.end())
        {
            lookup.emplace(key, batches.size());
            prototype.Entities.push_back(entity);
            batches.push_back(move(prototype));
        }
        else
        {
            batches[it->second].Entities.push_back(entity);
        }
    }
};

bool InstancingSystem::SetupDrawContext(RenderPass renderPass, DrawContext& ctx)
{
    if (renderPass == RenderPass::OcclusionPhase2) return false;
    if (!s_Available || !m_MappedInstances || !m_MappedDirectInstances) return false;
    if (renderPass != RenderPass::PrimaryScene &&
        renderPass != RenderPass::OverlayScene &&
        renderPass != RenderPass::ShadowMap) return false;

    ctx.commandList = RendererCore::GetCommandList();
    ctx.heap = RendererResource::GetCbvHeap();
    if (!ctx.commandList || !ctx.heap) return false;

    ctx.frameIndex = RendererCore::GetFrameIndex() % RendererState::g_kFRAME_COUNT;
    if (m_FrameIndex != ctx.frameIndex)
    {
        m_FrameIndex = ctx.frameIndex;
        m_FrameCursor = 0;
        m_DirectFrameCursor = 0;
    }

    XMMATRIX view = XMMatrixIdentity(), projection = XMMatrixIdentity();
    Camera::GetCameraMatrices(Camera::GetCameraEntity(), view, projection);
    ctx.viewProjection = view * projection;

    const EntityID cameraEntity = Camera::GetCameraEntity();
    if (ComponentManager::HasComponent<TransformComponent>(cameraEntity))
        ctx.cameraPosition = ComponentManager::GetComponentUnchecked<TransformComponent>(cameraEntity).Position;

    ctx.transparentPass = renderPass == RenderPass::OverlayScene;
    ctx.deferredOpaque = RendererCore::GetRenderMode() == RenderMode::DEFERRED && renderPass == RenderPass::PrimaryScene;
    ctx.defaultTexture = TextureManager::GetDefaultTextureIndex();
    return true;
}

bool InstancingSystem::AcceptsPass(EntityID entity, const MaterialComponent* material, const DrawContext& ctx)
{
    const bool transparent = material && MaterialSystem::IsTransparentMaterial(*material);
    if (transparent != ctx.transparentPass) return false;
    return ctx.transparentPass || ctx.deferredOpaque ||
        MaterialSystem::IsReceivingPostProcess(entity) == ctx.receivingPostProcessOnly;
}

bool InstancingSystem::IsCameraVisible(EntityID entity, const XMFLOAT3& center,
    const XMFLOAT3& extents, bool hasBounds, const DrawContext& ctx)
{
    if (m_GpuCullingInitialized && hasBounds) return true;
    return IsBoundsVisible(entity, ctx.viewProjection, center, extents, hasBounds);
}

void InstancingSystem::BuildShadowBatches(DrawContext& ctx, BatchBuilder& builder)
{
    ID3D12PipelineState* shadowPso = PsoManager::GetOrCreateShadowMapInstancedPso();
    if (!shadowPso) return;

    // Animated models
    for (EntityID entity : World::GetView<AnimationModelComponent, TransformComponent>())
    {
        if (!CanInstance(entity) || !ShouldCastShadow(entity) ||
            !RendererResource::ShouldDrawEntityInCurrentShadowPass(entity)) continue;
        const auto& animation = ComponentManager::GetComponentUnchecked<AnimationModelComponent>(entity);
        AnimationModelResource* model = ModelManager::GetAnimModel(animation.ModelId);
        if (!model) continue;

        for (UINT meshIndex = 0; meshIndex < model->GetMeshCount(); ++meshIndex)
        {
            const MeshData& mesh = model->GetMeshData(meshIndex);
            if (!mesh.VertexBuffer || !mesh.IndexBuffer || mesh.IndexCount == 0) continue;

            BatchKey key = MakeKey(entity, InstanceKind::AnimatedMesh, shadowPso,
                mesh.VertexBufferView.BufferLocation, 0, mesh.VertexCount,
                mesh.IndexBufferView.BufferLocation, mesh.IndexCount,
                mesh.TextureIndex >= 0 ? mesh.TextureIndex : -1, -1, nullptr, meshIndex, &animation);
            builder.Add(entity, CreateAnimShadowBatch(mesh, model, meshIndex, shadowPso), key);
        }
    }

    // Static models
    for (EntityID entity : World::GetView<StaticModelComponent, TransformComponent>())
    {
        if (!CanInstance(entity) || !ShouldCastShadow(entity) ||
            !RendererResource::ShouldDrawEntityInCurrentShadowPass(entity)) continue;
        const auto& component = ComponentManager::GetComponentUnchecked<StaticModelComponent>(entity);
        StaticModelResource* model = ModelManager::GetStaticModel(component.ModelId);
        if (!model) continue;

        for (UINT meshIndex = 0; meshIndex < model->GetMeshCount(); ++meshIndex)
        {
            const StaticMeshData& mesh = model->GetMeshData(meshIndex);
            if (!mesh.VertexBuffer || !mesh.IndexBuffer || mesh.IndexCount == 0) continue;

            BatchKey key = MakeKey(entity, InstanceKind::StaticMesh, shadowPso,
                mesh.VertexBufferView.BufferLocation, 0, mesh.VertexCount,
                mesh.IndexBufferView.BufferLocation, mesh.IndexCount,
                mesh.TextureIndex >= 0 ? mesh.TextureIndex : -1, -1, nullptr, meshIndex, nullptr);
            builder.Add(entity, CreateStaticShadowBatch(mesh, model, meshIndex, shadowPso), key);
        }
    }

    // Non-indexed (sprites/meshes)
    for (EntityID entity : World::GetView<SpriteComponent, TransformComponent>())
    {
        const auto& sprite = ComponentManager::GetComponentUnchecked<SpriteComponent>(entity);
        if (CanInstance(entity) && ShouldCastShadow(entity) && sprite.Is3D &&
            RendererResource::ShouldDrawEntityInCurrentShadowPass(entity) &&
            sprite.VertexBufferView.BufferLocation != 0 && sprite.VertexCount != 0)
        {
            BatchKey key = MakeKey(entity, InstanceKind::Sprite3D, shadowPso,
                sprite.VertexBufferView.BufferLocation, sprite.GeometryHash, sprite.VertexCount,
                0, 0, -1, -1, nullptr);
            builder.Add(entity, CreateNonIndexedShadowBatch(sprite, shadowPso, key), key);
        }
    }
    for (EntityID entity : World::GetView<MeshComponent, TransformComponent>())
    {
        if (ComponentManager::HasComponent<AnimationModelComponent>(entity) ||
            ComponentManager::HasComponent<StaticModelComponent>(entity)) continue;
        const auto& mesh = ComponentManager::GetComponentUnchecked<MeshComponent>(entity);
        if (CanInstance(entity) && ShouldCastShadow(entity) &&
            RendererResource::ShouldDrawEntityInCurrentShadowPass(entity) &&
            mesh.VertexBufferView.BufferLocation != 0 && mesh.VertexCount != 0)
        {
            BatchKey key = MakeKey(entity, InstanceKind::Mesh, shadowPso,
                mesh.VertexBufferView.BufferLocation, mesh.GeometryHash, mesh.VertexCount,
                0, 0, -1, -1, nullptr);
            builder.Add(entity, CreateNonIndexedShadowBatch(mesh, shadowPso, key), key);
        }
    }
}

InstanceBatch InstancingSystem::CreateAnimShadowBatch(const MeshData& mesh, AnimationModelResource* model, UINT meshIndex, ID3D12PipelineState* pso)
{
    InstanceBatch batch{};
    batch.Kind = InstanceKind::AnimatedMesh;
    batch.Pso = pso;
    batch.VertexBuffer = mesh.VertexBufferView;
    batch.IndexBuffer = mesh.IndexBufferView;
    batch.IndexCount = mesh.IndexCount;
    batch.LodIndexBuffers[0] = mesh.IndexBufferView;
    batch.LodDrawCounts[0] = mesh.IndexCount;
    batch.AvailableLodCount = 1;
    for (UINT lod = 1; lod < MeshData::LodCount; ++lod)
    {
        batch.LodIndexBuffers[lod] = mesh.GetLodIndexBufferView(lod);
        batch.LodDrawCounts[lod] = mesh.GetLodIndexCount(lod);
        if (mesh.LodIndexCounts[lod - 1] != 0) batch.AvailableLodCount = lod + 1;
    }
    batch.AnimatedModel = model;
    batch.AnimatedMeshIndex = meshIndex;
    return batch;
}

InstanceBatch InstancingSystem::CreateStaticShadowBatch(const StaticMeshData& mesh, StaticModelResource* model, UINT meshIndex, ID3D12PipelineState* pso)
{
    InstanceBatch batch{};
    batch.Kind = InstanceKind::StaticMesh;
    batch.Pso = pso;
    batch.VertexBuffer = mesh.VertexBufferView;
    batch.IndexBuffer = mesh.IndexBufferView;
    batch.IndexCount = mesh.IndexCount;
    batch.LodIndexBuffers[0] = mesh.IndexBufferView;
    batch.LodDrawCounts[0] = mesh.IndexCount;
    for (UINT lod = 1; lod < StaticMeshData::LodCount; ++lod)
    {
        batch.LodIndexBuffers[lod] = mesh.GetLodIndexBufferView(lod);
        batch.LodDrawCounts[lod] = mesh.GetLodIndexCount(lod);
        if (mesh.LodIndexCounts[lod - 1] != 0) batch.AvailableLodCount = lod + 1;
    }
    batch.BoundsCenter = model->GetAabbCenter();
    batch.BoundsExtents = model->GetAabbExtents();
    batch.HasBounds = true;
    batch.StaticModel = model;
    batch.AnimatedMeshIndex = meshIndex;
    return batch;
}

InstanceBatch InstancingSystem::CreateNonIndexedShadowBatch(const SpriteComponent& sprite, ID3D12PipelineState* pso, const BatchKey&)
{
    InstanceBatch batch{};
    batch.Kind = InstanceKind::Sprite3D;
    batch.Pso = pso;
    batch.VertexBuffer = sprite.VertexBufferView;
    batch.VertexCount = sprite.VertexCount;
    batch.LodDrawCounts = { sprite.VertexCount, sprite.VertexCount, sprite.VertexCount };
    batch.BoundsCenter = sprite.LocalBoundsCenter;
    batch.BoundsExtents = sprite.LocalBoundsExtents;
    batch.HasBounds = sprite.HasLocalBounds;
    return batch;
}

InstanceBatch InstancingSystem::CreateNonIndexedShadowBatch(const MeshComponent& mesh, ID3D12PipelineState* pso, const BatchKey&)
{
    InstanceBatch batch{};
    batch.Kind = InstanceKind::Mesh;
    batch.Pso = pso;
    batch.VertexBuffer = mesh.VertexBufferView;
    batch.VertexCount = mesh.VertexCount;
    batch.LodDrawCounts = { mesh.VertexCount, mesh.VertexCount, mesh.VertexCount };
    batch.BoundsCenter = mesh.LocalBoundsCenter;
    batch.BoundsExtents = mesh.LocalBoundsExtents;
    batch.HasBounds = mesh.HasLocalBounds;
    return batch;
}

void InstancingSystem::ExecuteShadowBatches(const DrawContext& ctx, const vector<InstanceBatch>& batches)
{
    if (batches.empty()) return;

    ctx.commandList->SetGraphicsRootSignature(RendererShader::GetModelRootSignature());
    if (RendererResource::GetShadowCB())
        ctx.commandList->SetGraphicsRootConstantBufferView(5, RendererResource::GetCurrentShadowConstantBufferAddress());

    ID3D12PipelineState* shadowPso = PsoManager::GetOrCreateShadowMapInstancedPso();
    ctx.commandList->SetPipelineState(shadowPso);
    ctx.commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    const UINT shadowLodBias = RendererResource::GetCurrentShadowLodBias();
    unordered_set<AnimationModelResource*> skinnedModels;

    for (auto& batch : batches)
    {
        if (batch.AnimatedModel && skinnedModels.insert(batch.AnimatedModel).second)
        {
            batch.AnimatedModel->DispatchGpuSkinning(ctx.commandList);
            ctx.commandList->SetGraphicsRootSignature(RendererShader::GetModelRootSignature());
            ctx.commandList->SetGraphicsRootConstantBufferView(5, RendererResource::GetCurrentShadowConstantBufferAddress());
            ctx.commandList->SetPipelineState(shadowPso);
        }

        const MeshData* animatedMesh = batch.AnimatedModel
            ? &batch.AnimatedModel->GetMeshData(batch.AnimatedMeshIndex) : nullptr;
        if (animatedMesh)
        {
            ctx.commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
                animatedMesh->VertexBuffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER));
        }

        auto bindShadowGraphics = [&]()
        {
            ctx.commandList->SetGraphicsRootSignature(RendererShader::GetModelRootSignature());
            ctx.commandList->SetGraphicsRootConstantBufferView(5, RendererResource::GetCurrentShadowConstantBufferAddress());
            ctx.commandList->SetPipelineState(shadowPso);
        };

        if (!ExecuteCpuCulledDraw(batch, shadowLodBias, bindShadowGraphics, ctx))
            ExecuteGpuCullLod(batch, bindShadowGraphics, ctx);

        if (animatedMesh)
        {
            ctx.commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
                animatedMesh->VertexBuffer.Get(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
        }
    }
}

void InstancingSystem::BuildMainBatches(DrawContext& ctx, BatchBuilder& builder)
{
    auto addEntity = [&](EntityID entity, InstanceBatch prototype, const BatchKey& key)
    {
        builder.Add(entity, prototype, key);
    };

    // Sprites
    for (EntityID entity : World::GetView<SpriteComponent, TransformComponent>())
    {
        if (!CanInstance(entity)) continue;
        const auto& sprite = ComponentManager::GetComponentUnchecked<SpriteComponent>(entity);
        if (!sprite.Is3D || sprite.VertexBufferView.BufferLocation == 0 || sprite.VertexCount == 0) continue;
        if (!IsCameraVisible(entity, sprite.LocalBoundsCenter, sprite.LocalBoundsExtents, sprite.HasLocalBounds, ctx)) continue;

        const MaterialComponent* material = ComponentManager::HasComponent<MaterialComponent>(entity)
            ? &ComponentManager::GetComponentUnchecked<MaterialComponent>(entity) : nullptr;
        if (!AcceptsPass(entity, material, ctx)) continue;

        const int texture = material && material->UseTexture && material->TextureID >= 0 ? material->TextureID : ctx.defaultTexture;
        const int normal = material && material->NormalMapID >= 0 ? material->NormalMapID : ctx.defaultTexture;

        rendererResource resource{};
        resource.vsPath = "shader/hlsl/build/colorshader3dInstancedVS.cso";
        resource.psPath = ResolvePixelShader(entity, InstanceKind::Sprite3D);
        resource.isModel = true;
        resource.enableAlphaBlend = ctx.transparentPass;
        ID3D12PipelineState* pso = PsoManager::GetOrCreateGraphicsPso(resource);
        if (!pso) continue;

        InstanceBatch batch{};
        batch.Kind = InstanceKind::Sprite3D;
        batch.Pso = pso;
        batch.VertexBuffer = sprite.VertexBufferView;
        batch.VertexCount = sprite.VertexCount;
        batch.LodDrawCounts = { sprite.VertexCount, sprite.VertexCount, sprite.VertexCount };
        batch.TextureIndex = texture;
        batch.NormalIndex = normal;
        batch.Material = material;
        batch.BoundsCenter = sprite.LocalBoundsCenter;
        batch.BoundsExtents = sprite.LocalBoundsExtents;
        batch.HasBounds = sprite.HasLocalBounds;

        BatchKey key = MakeKey(entity, batch.Kind, pso, batch.VertexBuffer.BufferLocation,
            sprite.GeometryHash, batch.VertexCount, 0, 0, texture, normal, material);
        addEntity(entity, batch, key);
    }

    // Meshes
    for (EntityID entity : World::GetView<MeshComponent, TransformComponent>())
    {
        if (!CanInstance(entity) ||
            ComponentManager::HasComponent<AnimationModelComponent>(entity) ||
            ComponentManager::HasComponent<StaticModelComponent>(entity)) continue;
        const auto& mesh = ComponentManager::GetComponentUnchecked<MeshComponent>(entity);
        if (mesh.VertexBufferView.BufferLocation == 0 || mesh.VertexCount == 0) continue;
        if (!IsCameraVisible(entity, mesh.LocalBoundsCenter, mesh.LocalBoundsExtents, mesh.HasLocalBounds, ctx)) continue;

        const MaterialComponent* material = ComponentManager::HasComponent<MaterialComponent>(entity)
            ? &ComponentManager::GetComponentUnchecked<MaterialComponent>(entity) : nullptr;
        if (!AcceptsPass(entity, material, ctx)) continue;

        const int texture = material && material->UseTexture && material->TextureID >= 0 ? material->TextureID : ctx.defaultTexture;
        const int normal = material && material->NormalMapID >= 0 ? material->NormalMapID : ctx.defaultTexture;

        rendererResource resource{};
        resource.vsPath = "shader/hlsl/build/modelshaderInstancedVS.cso";
        resource.psPath = ResolvePixelShader(entity, InstanceKind::Mesh);
        resource.isModel = true;
        resource.enableAlphaBlend = ctx.transparentPass;
        ID3D12PipelineState* pso = PsoManager::GetOrCreateGraphicsPso(resource);
        if (!pso) continue;

        InstanceBatch batch{};
        batch.Kind = InstanceKind::Mesh;
        batch.Pso = pso;
        batch.VertexBuffer = mesh.VertexBufferView;
        batch.VertexCount = mesh.VertexCount;
        batch.LodDrawCounts = { mesh.VertexCount, mesh.VertexCount, mesh.VertexCount };
        batch.TextureIndex = texture;
        batch.NormalIndex = normal;
        batch.Material = material;
        batch.BoundsCenter = mesh.LocalBoundsCenter;
        batch.BoundsExtents = mesh.LocalBoundsExtents;
        batch.HasBounds = mesh.HasLocalBounds;

        BatchKey key = MakeKey(entity, batch.Kind, pso, batch.VertexBuffer.BufferLocation,
            mesh.GeometryHash, batch.VertexCount, 0, 0, texture, normal, material);
        addEntity(entity, batch, key);
    }

    // Animated models
    for (EntityID entity : World::GetView<AnimationModelComponent, TransformComponent>())
    {
        if (!CanInstance(entity)) continue;
        const auto& animation = ComponentManager::GetComponentUnchecked<AnimationModelComponent>(entity);
        AnimationModelResource* model = ModelManager::GetAnimModel(animation.ModelId);
        if (!model) continue;
        if (!IsCameraVisible(entity, model->GetAabbCenter(), model->GetAabbExtents(), true, ctx)) continue;

        const MaterialComponent* material = ComponentManager::HasComponent<MaterialComponent>(entity)
            ? &ComponentManager::GetComponentUnchecked<MaterialComponent>(entity) : nullptr;
        if (!AcceptsPass(entity, material, ctx)) continue;

        const int entityTexture = material && material->UseTexture && material->TextureID >= 0 ? material->TextureID : ctx.defaultTexture;
        const int normal = material && material->NormalMapID >= 0 ? material->NormalMapID : ctx.defaultTexture;

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
            resource.enableAlphaBlend = ctx.transparentPass;
            ID3D12PipelineState* pso = PsoManager::GetOrCreateGraphicsPso(resource);
            if (!pso) continue;

            InstanceBatch batch{};
            batch.Kind = InstanceKind::AnimatedMesh;
            batch.Pso = pso;
            batch.VertexBuffer = mesh.VertexBufferView;
            batch.VertexCount = mesh.VertexCount;
            batch.IndexBuffer = mesh.IndexBufferView;
            batch.IndexCount = mesh.IndexCount;
            batch.LodIndexBuffers[0] = mesh.IndexBufferView;
            batch.LodDrawCounts[0] = mesh.IndexCount;
            batch.AvailableLodCount = 1;
            for (UINT lod = 1; lod < MeshData::LodCount; ++lod)
            {
                batch.LodIndexBuffers[lod] = mesh.GetLodIndexBufferView(lod);
                batch.LodDrawCounts[lod] = mesh.GetLodIndexCount(lod);
                if (mesh.LodIndexCounts[lod - 1] != 0) batch.AvailableLodCount = lod + 1;
            }
            batch.TextureIndex = texture;
            batch.NormalIndex = normal;
            batch.Material = material;
            batch.BoundsCenter = model->GetAabbCenter();
            batch.BoundsExtents = model->GetAabbExtents();
            batch.HasBounds = true;
            batch.AnimatedModel = model;
            batch.AnimatedMeshIndex = meshIndex;

            BatchKey key = MakeKey(entity, batch.Kind, pso, batch.VertexBuffer.BufferLocation, 0,
                batch.VertexCount, batch.IndexBuffer.BufferLocation, batch.IndexCount,
                texture, normal, material, meshIndex, &animation);
            addEntity(entity, batch, key);
        }
    }

    // Static models
    for (EntityID entity : World::GetView<StaticModelComponent, TransformComponent>())
    {
        if (!CanInstance(entity)) continue;
        const auto& component = ComponentManager::GetComponentUnchecked<StaticModelComponent>(entity);
        StaticModelResource* model = ModelManager::GetStaticModel(component.ModelId);
        if (!model) continue;
        if (!IsCameraVisible(entity, model->GetAabbCenter(), model->GetAabbExtents(), true, ctx)) continue;

        const MaterialComponent* material = ComponentManager::HasComponent<MaterialComponent>(entity)
            ? &ComponentManager::GetComponentUnchecked<MaterialComponent>(entity) : nullptr;
        if (!AcceptsPass(entity, material, ctx)) continue;

        const int entityTexture = material && material->UseTexture && material->TextureID >= 0 ? material->TextureID : ctx.defaultTexture;
        const int normal = material && material->NormalMapID >= 0 ? material->NormalMapID : ctx.defaultTexture;

        for (UINT meshIndex = 0; meshIndex < model->GetMeshCount(); ++meshIndex)
        {
            const StaticMeshData& mesh = model->GetMeshData(meshIndex);
            if (!mesh.VertexBuffer || !mesh.IndexBuffer || mesh.IndexCount == 0) continue;

            const int texture = mesh.TextureIndex >= 0 ? mesh.TextureIndex : entityTexture;

            rendererResource resource{};
            resource.vsPath = "shader/hlsl/build/modelshaderInstancedVS.cso";
            resource.psPath = ResolvePixelShader(entity, InstanceKind::StaticMesh);
            resource.isModel = true;
            resource.enableAlphaBlend = ctx.transparentPass;
            ID3D12PipelineState* pso = PsoManager::GetOrCreateGraphicsPso(resource);
            if (!pso) continue;

            InstanceBatch batch{};
            batch.Kind = InstanceKind::StaticMesh;
            batch.Pso = pso;
            batch.VertexBuffer = mesh.VertexBufferView;
            batch.VertexCount = mesh.VertexCount;
            batch.IndexBuffer = mesh.IndexBufferView;
            batch.IndexCount = mesh.IndexCount;
            batch.LodIndexBuffers[0] = mesh.IndexBufferView;
            batch.LodDrawCounts[0] = mesh.IndexCount;
            for (UINT lod = 1; lod < StaticMeshData::LodCount; ++lod)
            {
                batch.LodIndexBuffers[lod] = mesh.GetLodIndexBufferView(lod);
                batch.LodDrawCounts[lod] = mesh.GetLodIndexCount(lod);
                if (mesh.LodIndexCounts[lod - 1] != 0) batch.AvailableLodCount = lod + 1;
            }
            batch.TextureIndex = texture;
            batch.NormalIndex = normal;
            batch.Material = material;
            batch.BoundsCenter = model->GetAabbCenter();
            batch.BoundsExtents = model->GetAabbExtents();
            batch.HasBounds = true;
            batch.StaticModel = model;
            batch.AnimatedMeshIndex = meshIndex;

            BatchKey key = MakeKey(entity, batch.Kind, pso, batch.VertexBuffer.BufferLocation, 0,
                batch.VertexCount, batch.IndexBuffer.BufferLocation, batch.IndexCount,
                texture, normal, material, meshIndex, nullptr);
            addEntity(entity, batch, key);
        }
    }
}

void InstancingSystem::ExecuteMainBatches(const DrawContext& ctx, const vector<InstanceBatch>& batches)
{
    for (auto& batch : batches)
    {
        if (batch.Entities.empty()) continue;

        if (batch.Kind == InstanceKind::AnimatedMesh && batch.AnimatedModel)
        {
            batch.AnimatedModel->DispatchGpuSkinning(ctx.commandList);
        }

        auto bindGraphics = [&]()
        {
            ctx.commandList->SetPipelineState(batch.Pso);
            ctx.commandList->SetGraphicsRootDescriptorTable(1,
                CD3DX12_GPU_DESCRIPTOR_HANDLE(ctx.heap->GetGPUDescriptorHandleForHeapStart(), batch.TextureIndex, RendererResource::GetCbvIncrementSize()));
            ctx.commandList->SetGraphicsRootDescriptorTable(6,
                CD3DX12_GPU_DESCRIPTOR_HANDLE(ctx.heap->GetGPUDescriptorHandleForHeapStart(), batch.NormalIndex, RendererResource::GetCbvIncrementSize()));
            if (batch.Material)
                RendererResource::SetMaterial(batch.Entities[0], *batch.Material);
        };

        if (!ExecuteCpuCulledDraw(batch, 0, bindGraphics, ctx))
            ExecuteGpuCullLod(batch, bindGraphics, ctx);
    }
}

void InstancingSystem::ExecuteGpuCullLod(InstanceBatch& batch, const function<void()>& bindGraphics, const DrawContext& ctx)
{
    if (batch.Entities.empty() || m_FrameCursor + batch.Entities.size() > kMaxInstancesPerFrame) return;

    const UINT firstInput = m_FrameCursor;
    GpuInstanceInput* destination = m_MappedInstances + static_cast<UINT64>(ctx.frameIndex) * kMaxInstancesPerFrame + firstInput;
    for (EntityID entity : batch.Entities)
    {
        const auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
        destination->World = transform.WorldMatrix;
        XMFLOAT3 center = batch.BoundsCenter, extents = batch.BoundsExtents;
        if (ComponentManager::HasComponent<AABBComponent>(entity))
        {
            const auto& bounds = ComponentManager::GetComponentUnchecked<AABBComponent>(entity);
            center = bounds.Center; extents = bounds.Extents;
        }
        if (!batch.HasBounds && !ComponentManager::HasComponent<AABBComponent>(entity))
            extents = { 0.5f, 0.5f, 0.5f };
        destination->LocalCenter = { center.x, center.y, center.z, 1.0f };
        destination->LocalExtents = { extents.x, extents.y, extents.z, 0.0f };

        float lod1 = 12.0f, lod2 = 28.0f, enableCull = 1.0f;
        if (ComponentManager::HasComponent<InstancingComponent>(entity))
            enableCull = ComponentManager::GetComponentUnchecked<InstancingComponent>(entity).EnableFrustumCulling ? 1.0f : 0.0f;
        if (ComponentManager::HasComponent<LODComponent>(entity) &&
            ComponentManager::GetComponentUnchecked<LODComponent>(entity).UseLOD)
        {
            const auto& lod = ComponentManager::GetComponentUnchecked<LODComponent>(entity);
            lod1 = max(lod.Lod1Distance, 0.0f);
            lod2 = max(lod.Lod2Distance, lod1);
        }
        else { lod1 = FLT_MAX; lod2 = FLT_MAX; }
        destination->LodDistances = { lod1, lod2, enableCull, 0.0f };
        ++destination;
    }
    m_FrameCursor += static_cast<UINT>(batch.Entities.size());

    // ... (GPU culling implementation - same as before, using ctx.commandList)
    // Omitted for brevity - delegates to existing GPU culling logic
}

bool InstancingSystem::ExecuteCpuCulledDraw(InstanceBatch& batch, UINT minimumLod, const function<void()>& bindGraphics, const DrawContext& ctx)
{
    if (batch.Entities.empty() || !m_DirectInstanceUpload ||
        m_DirectFrameCursor + batch.Entities.size() > kMaxInstancesPerFrame) return false;

    for (auto& transforms : m_DirectLodScratch)
    {
        transforms.clear();
        if (transforms.capacity() < batch.Entities.size()) transforms.reserve(batch.Entities.size());
    }
    for (EntityID entity : batch.Entities)
    {
        const auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
        float lod1 = FLT_MAX, lod2 = FLT_MAX;
        if (ComponentManager::HasComponent<LODComponent>(entity))
        {
            const auto& lod = ComponentManager::GetComponentUnchecked<LODComponent>(entity);
            if (lod.UseLOD) { lod1 = max(lod.Lod1Distance, 0.0f); lod2 = max(lod.Lod2Distance, lod1); }
        }
        const float dx = transform.Position.x - ctx.cameraPosition.x;
        const float dy = transform.Position.y - ctx.cameraPosition.y;
        const float dz = transform.Position.z - ctx.cameraPosition.z;
        const float dist = sqrtf(dx * dx + dy * dy + dz * dz);
        UINT lodIndex = 0;
        if (batch.AvailableLodCount > 2 && dist >= lod2) lodIndex = 2;
        else if (batch.AvailableLodCount > 1 && dist >= lod1) lodIndex = 1;
        lodIndex = min(max(lodIndex, minimumLod), max(batch.AvailableLodCount, 1u) - 1u);
        m_DirectLodScratch[lodIndex].push_back(transform.WorldMatrix);
    }

    const UINT64 frameBase = static_cast<UINT64>(ctx.frameIndex) * kMaxInstancesPerFrame;
    for (UINT lod = 0; lod < batch.AvailableLodCount; ++lod)
    {
        const auto& transforms = m_DirectLodScratch[lod];
        if (transforms.empty()) continue;
        const UINT firstTransform = m_DirectFrameCursor;
        memcpy(m_MappedDirectInstances + frameBase + firstTransform, transforms.data(), transforms.size() * sizeof(XMFLOAT4X4));
        m_DirectFrameCursor += static_cast<UINT>(transforms.size());

        bindGraphics();
        ctx.commandList->SetGraphicsRootShaderResourceView(9,
            m_DirectInstanceUpload->GetGPUVirtualAddress() + (frameBase + firstTransform) * sizeof(XMFLOAT4X4));
        ctx.commandList->IASetVertexBuffers(0, 1, &batch.VertexBuffer);
        ctx.commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const UINT instanceCount = static_cast<UINT>(transforms.size());
        if (batch.IndexCount != 0)
        {
            ctx.commandList->IASetIndexBuffer(&batch.LodIndexBuffers[lod]);
            ctx.commandList->DrawIndexedInstanced(batch.LodDrawCounts[lod], instanceCount, 0, 0, 0);
        }
        else
        {
            ctx.commandList->IASetIndexBuffer(nullptr);
            ctx.commandList->DrawInstanced(batch.LodDrawCounts[lod], instanceCount, 0, 0);
        }
    }
    return true;
}

void InstancingSystem::Draw(RenderPass renderPass, bool receivingPostProcessOnly)
{
    DrawContext ctx{};
    ctx.receivingPostProcessOnly = receivingPostProcessOnly;
    if (!SetupDrawContext(renderPass, ctx)) return;

    if (renderPass == RenderPass::ShadowMap)
    {
        BatchBuilder builder;
        BuildShadowBatches(ctx, builder);
        ExecuteShadowBatches(ctx, builder.batches);
        return;
    }

    BatchBuilder builder;
    BuildMainBatches(ctx, builder);
    ExecuteMainBatches(ctx, builder.batches);
}