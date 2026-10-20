#include "pch.h"
#include "materialmanager.h"
#include "vertexdeformation.h"
#include "instancingsystem.h"
#include "gpudriven/gpudrivenrenderer.h"
#include "gpuscene/gpuscene.h"
#include "componentmanager.h"
#include "world.h"
#include "camera.h"
#include "graphicsdevice.h"
#include "shaderpipelines.h"
#include "psomanager.h"
#include "texturemanager.h"
#include "materialsystem.h"
#include "modelmanager.h"
#include "animationmodel.h"
#include <unordered_map>
#include <unordered_set>
#include "shaderdescription.h"
#include "materialbindings.h"
#include "frameconstants.h"
#include "lightingresources.h"
#include "renderconfiguration.h"
#include "impostorrenderer.h"
#include "gpuobjectculling.h"
#include "graphicslog.h"
#include <memory>

namespace
{
    std::unordered_map<ID3D12PipelineState*, std::unique_ptr<WorkGraph::MeshGraph>> g_InstanceRasterGraphs;
    ComPtr<ID3DBlob> g_InstanceRasterLibrary;
    ComPtr<ID3DBlob> g_InstanceShadowLibrary;
}

bool InstancingSystem::RegisterMeshPipeline(ID3D12Device* device, ID3D12PipelineState* pipeline,
                                            const D3D12_GRAPHICS_PIPELINE_STATE_DESC& description, bool shadowPass)
{
    ComPtr<ID3DBlob>& library = shadowPass ? g_InstanceShadowLibrary : g_InstanceRasterLibrary;
    if (!library)
    {
        const wchar_t* shaderFile = L"shader/hlsl/InstanceRasterWorkGraph.hlsl";
        if (shadowPass)
        {
            shaderFile = L"shader/hlsl/InstanceShadowWorkGraph.hlsl";
        }
        if (FAILED(WorkGraph::CompileFile(shaderFile, nullptr, L"lib_6_9", library)))
        {
            WriteGraphicsLog("Instancing Mesh Graph: shader compilation failed.\n");
            return false;
        }
    }
    WorkGraph::MeshGraphDescription graphDescription{};
    graphDescription.RootSignature = description.pRootSignature;
    graphDescription.Library = CD3DX12_SHADER_BYTECODE(library.Get());
    graphDescription.PixelShader = description.PS;
    graphDescription.PixelShaderEntry = L"main";
    graphDescription.ProgramName = L"InstanceRasterGraph";
    graphDescription.EntryNode = L"InstanceRasterEntry";
    graphDescription.MeshNode = L"InstanceMesh";
    graphDescription.MeshShader = L"InstanceMeshMain";
    graphDescription.Blend = description.BlendState;
    graphDescription.Rasterizer = description.RasterizerState;
    graphDescription.DepthStencil = description.DepthStencilState;
    graphDescription.DepthFormat = description.DSVFormat;
    graphDescription.RenderTargets.NumRenderTargets = description.NumRenderTargets;
    for (UINT index = 0; index < description.NumRenderTargets; ++index)
    {
        graphDescription.RenderTargets.RTFormats[index] = description.RTVFormats[index];
    }
    auto graph = std::make_unique<WorkGraph::MeshGraph>();
    if (!graph->Initialize(device, graphDescription))
    {
        WriteGraphicsLog("Instancing Mesh Graph: pipeline registration failed.\n");
        return false;
    }
    g_InstanceRasterGraphs[pipeline] = std::move(graph);
    return true;
}

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
    ID3D12Resource* VertexResource = nullptr;
    D3D12_INDEX_BUFFER_VIEW IndexBuffer{};
    UINT VertexCount = 0;
    UINT IndexCount = 0;
    array<D3D12_INDEX_BUFFER_VIEW, 3> LodIndexBuffers{};
    array<ID3D12Resource*, 3> LodIndexResources{};
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
        return Kind == other.Kind && Pso == other.Pso && GeometryIdentity == other.GeometryIdentity &&
               VertexCount == other.VertexCount && IndexBuffer == other.IndexBuffer && IndexCount == other.IndexCount &&
               TextureIndex == other.TextureIndex && NormalIndex == other.NormalIndex &&
               MaterialHash == other.MaterialHash && MeshIndex == other.MeshIndex && GroupId == other.GroupId &&
               AnimationModelId == other.AnimationModelId && CurrentAnimation == other.CurrentAnimation &&
               NextAnimation == other.NextAnimation && CurrentTime == other.CurrentTime && NextTime == other.NextTime &&
               BlendRate == other.BlendRate && ActiveAnimationLayers == other.ActiveAnimationLayers;
    }
};

struct BatchKeyHash
{
    size_t operator()(const BatchKey& key) const noexcept
    {
        size_t h = 0;
        auto hash_combine = [&h](auto&& v) {
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
    // 環境メッシュは背景放射輝度を供給するため、太陽を遮蔽させない。
    if (ComponentManager::HasComponent<MeshComponent>(entity) &&
        ComponentManager::GetComponentUnchecked<MeshComponent>(entity).IsEnvironment)
    {
        return false;
    }

    if (ComponentManager::HasComponent<LightComponent>(entity))
    {
        return false;
    }
    if (ComponentManager::HasComponent<NameComponent>(entity) &&
        ComponentManager::GetComponentUnchecked<NameComponent>(entity).Name == "Sky")
    {
        return false;
    }
    if (!ComponentManager::HasComponent<MaterialComponent>(entity))
    {
        return true;
    }
    const auto& material = ComponentManager::GetComponentUnchecked<MaterialComponent>(entity);
    return !MaterialSystem::IsTransparentMaterial(material) &&
           !(material.ShaderClassMode == MaterialMode::Manual && material.ShaderClass == ShaderClass::Shadow);
}

uint64_t InstancingSystem::HashMaterial(const MaterialComponent* material)
{
    if (material)
    {
        return MaterialBindings::GetMaterialBatchHash(*material);
    }
    return 0;
}

const char* InstancingSystem::ResolvePixelShader(EntityID entity, InstanceKind kind)
{
    if (ComponentManager::HasComponent<ShaderComponent>(entity))
    {
        const auto& shader = ComponentManager::GetComponentUnchecked<ShaderComponent>(entity);
        if (!shader.PsPath.empty())
        {
            return shader.PsPath.c_str();
        }
    }
    if (kind == InstanceKind::Sprite3D)
    {
        return "shader/hlsl/build/colorshader3dPS.cso";
    }
    return "shader/hlsl/build/modelshaderPS.cso";
}

bool InstancingSystem::IsBoundsVisible(EntityID entity, const XMMATRIX& viewProjection, const XMFLOAT3& fallbackCenter,
                                       const XMFLOAT3& fallbackExtents, bool hasFallbackBounds)
{
    if (ComponentManager::HasComponent<InstancingComponent>(entity) &&
        !ComponentManager::GetComponentUnchecked<InstancingComponent>(entity).EnableFrustumCulling)
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

    const XMMATRIX world =
        XMLoadFloat4x4(&ComponentManager::GetComponentUnchecked<TransformComponent>(entity).WorldMatrix);
    {
        const XMVECTOR clipCenter =
            XMVector4Transform(XMVectorSet(center.x, center.y, center.z, 1.0f), world * viewProjection);
        XMFLOAT4 c{};
        XMStoreFloat4(&c, clipCenter);
        if (c.x >= -c.w && c.x <= c.w && c.y >= -c.w && c.y <= c.w && c.z >= 0.0f && c.z <= c.w)
        {
            return true;
        }
    }
    bool outsideLeft = true, outsideRight = true, outsideBottom = true;
    bool outsideTop = true, outsideNear = true, outsideFar = true;
    for (UINT corner = 0; corner < 8; ++corner)
    {
        float shaderValue;
        if ((corner & 1))
        {
            shaderValue = 1.0f;
        }
        else
        {
            shaderValue = -1.0f;
        }
        float shader3;
        if ((corner & 2))
        {
            shader3 = 1.0f;
        }
        else
        {
            shader3 = -1.0f;
        }
        float shader4;
        if ((corner & 4))
        {
            shader4 = 1.0f;
        }
        else
        {
            shader4 = -1.0f;
        }
        const XMFLOAT3 local = {center.x + extents.x * (shaderValue), center.y + extents.y * (shader3),
                                center.z + extents.z * (shader4)};
        const XMVECTOR clip = XMVector4Transform(XMVectorSet(local.x, local.y, local.z, 1.0f), world * viewProjection);
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

BatchKey InstancingSystem::MakeKey(EntityID entity, InstanceKind kind, ID3D12PipelineState* pso,
                                   D3D12_GPU_VIRTUAL_ADDRESS vertexBuffer, uint64_t geometryHash, UINT vertexCount,
                                   D3D12_GPU_VIRTUAL_ADDRESS indexBuffer, UINT indexCount, int textureIndex,
                                   int normalIndex, const MaterialComponent* material, UINT meshIndex,
                                   const AnimationModelComponent* animation)
{
    const InstancingComponent* instancing;
    if (ComponentManager::HasComponent<InstancingComponent>(entity))
    {
        instancing = &ComponentManager::GetComponentUnchecked<InstancingComponent>(entity);
    }
    else
    {
        instancing = nullptr;
    }
    uint32_t groupId;
    if (instancing)
    {
        groupId = instancing->GroupId;
    }
    else
    {
        groupId = 0;
    }
    uint64_t geometryIdentity;
    if (groupId != 0)
    {
        geometryIdentity = static_cast<uint64_t>(groupId);
    }
    else
    {
        uint64_t geometryKey;
        if (geometryHash != 0)
        {
            geometryKey = geometryHash;
        }
        else
        {
            geometryKey = vertexBuffer;
        }
        geometryIdentity = (geometryKey);
    }

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
        {
            key.ActiveAnimationLayers.emplace_back(layer.AnimationName, layer.CurrentTime);
        }
    }
    return key;
}

bool InstancingSystem::CanInstance(EntityID entity)
{
    if (ComponentManager::HasComponent<MaterialComponent>(entity))
    {
        const auto& material =
            MaterialManager::Resolve(ComponentManager::GetComponentUnchecked<MaterialComponent>(entity));
        // マテリアルを考慮する影のピクセルは、通常の描画経路を使う。
        if (MaterialSystem::IsTransparentMaterial(material) ||
            MaterialSystem::ResolveShadowMode(material) != MaterialShadowMode::Opaque ||
            material.AssetSurface == MaterialSurface::Masked || material.RuntimeHandle != 0)
        {
            return false;
        }
    }
    if (ComponentManager::HasComponent<MaterialComponent>(entity) &&
        !ComponentManager::GetComponentUnchecked<MaterialComponent>(entity).MaterialSlots.empty())
    {
        return false;
    }
    // エンティティごとの頂点場が異なる場合、同じインスタンス描画用ジオメトリは共有できない。
    if (VertexDeformation::IsRequested(entity))
    {
        return false;
    }
    if (ComponentManager::HasComponent<MeshComponent>(entity) &&
        ComponentManager::GetComponentUnchecked<MeshComponent>(entity).IsEnvironment)
    {
        return false;
    }
    if (!s_Available || !ComponentManager::HasComponent<TransformComponent>(entity))
    {
        return false;
    }

    const bool useInstancing = ComponentManager::HasComponent<InstancingComponent>(entity) &&
                               ComponentManager::GetComponentUnchecked<InstancingComponent>(entity).UseInstancing;
    const bool useLod = ComponentManager::HasComponent<LODComponent>(entity) &&
                        ComponentManager::GetComponentUnchecked<LODComponent>(entity).UseLOD;
    // インポスターの置き換えはこの経路で行う。
    const bool useImpostor = ImpostorRenderer::WantsImpostor(entity);
    if (!useInstancing && !useLod && !useImpostor)
    {
        return false;
    }

    if (ComponentManager::HasComponent<AnimationModelComponent>(entity))
    {
        return true;
    }
    if (ComponentManager::HasComponent<StaticModelComponent>(entity))
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
    // CPU では判定しない。GPU（GpuObjectCullCS）のメイン カメラの結果を読み戻した値（数フレーム遅れ）。
    return GpuObjectCulling::WasVisibleOnGpu(entity);
}

bool InstancingSystem::CreateGpuCullingResources(ID3D12Device* device)
{
    if (!device)
    {
        return false;
    }

    auto createDefaultBuffer = [&](UINT64 size, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state,
                                   ComPtr<ID3D12Resource>& resource) {
        const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        const auto description = CD3DX12_RESOURCE_DESC::Buffer(size, flags);
        return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description, state, nullptr,
                                                         IID_PPV_ARGS(&resource)));
    };

    const UINT64 transformBytes = static_cast<UINT64>(kMaxInstancesPerFrame) * sizeof(XMFLOAT4X4);
    for (auto& resource : m_LodInstances)
    {
        if (!createDefaultBuffer(transformBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                 D3D12_RESOURCE_STATE_COMMON, resource))
        {
            return false;
        }
    }
    if (!createDefaultBuffer(static_cast<UINT64>(kMaxBatchesPerFrame) * 3 * sizeof(UINT),
                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON, m_LodCounts) ||
        !createDefaultBuffer(static_cast<UINT64>(kMaxBatchesPerFrame) * 3 * sizeof(D3D12_DRAW_INDEXED_ARGUMENTS),
                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON,
                             m_IndirectArguments))
    {
        return false;
    }

    const vector<UINT> zeros(static_cast<size_t>(kMaxBatchesPerFrame) * 3, 0u);
    const auto uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    const auto zeroDescription = CD3DX12_RESOURCE_DESC::Buffer(zeros.size() * sizeof(UINT));
    if (FAILED(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &zeroDescription,
                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               IID_PPV_ARGS(&m_ZeroCountsUpload))))
    {
        return false;
    }
    void* mappedZeros = nullptr;
    const D3D12_RANGE noRead{0, 0};
    if (FAILED(m_ZeroCountsUpload->Map(0, &noRead, &mappedZeros)))
    {
        return false;
    }
    memcpy(mappedZeros, zeros.data(), zeros.size() * sizeof(UINT));
    m_ZeroCountsUpload->Unmap(0, nullptr);

    auto createRootSignature = [&](D3D12_ROOT_PARAMETER* parameters, UINT count,
                                   ComPtr<ID3D12RootSignature>& rootSignature) {
        CD3DX12_ROOT_SIGNATURE_DESC description{};
        description.Init(count, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE);
        ComPtr<ID3DBlob> serialized, errors;
        if (FAILED(D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)))
        {
            if (errors)
            {
                Debug::Log("%s\n", static_cast<const char*>(errors->GetBufferPointer()));
            }
            return false;
        }
        return SUCCEEDED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                                     IID_PPV_ARGS(&rootSignature)));
    };

    // t0 候補, u0-u2 LOD 別出力, u3 件数, u4 インポスター出力, u5 インポスター引数, b0 定数（36）。
    CD3DX12_ROOT_PARAMETER cullParameters[8]{};
    cullParameters[0].InitAsShaderResourceView(0);
    cullParameters[1].InitAsUnorderedAccessView(0);
    cullParameters[2].InitAsUnorderedAccessView(1);
    cullParameters[3].InitAsUnorderedAccessView(2);
    cullParameters[4].InitAsUnorderedAccessView(3);
    cullParameters[5].InitAsUnorderedAccessView(4);
    cullParameters[6].InitAsUnorderedAccessView(5);
    cullParameters[7].InitAsConstants(36, 0);
    if (!createRootSignature(cullParameters, _countof(cullParameters), m_CullLodRootSignature))
    {
        return false;
    }

    CD3DX12_ROOT_PARAMETER argsParameters[3]{};
    argsParameters[0].InitAsShaderResourceView(0);
    argsParameters[1].InitAsUnorderedAccessView(0);
    argsParameters[2].InitAsConstants(6, 0);
    if (!createRootSignature(argsParameters, _countof(argsParameters), m_ArgsRootSignature))
    {
        return false;
    }

    if (!m_CullLodGraph.Initialize(device, m_CullLodRootSignature.Get(),
                                   L"shader/hlsl/InstanceCullWorkGraph.hlsl", L"InstanceCullEntry") ||
        !m_ArgsGraph.Initialize(device, m_ArgsRootSignature.Get(),
                                L"shader/hlsl/InstanceArgsWorkGraph.hlsl", L"InstanceArgsEntry"))
    {
        return false;
    }

    const UINT64 rasterInputBytes = static_cast<UINT64>(WorkGraph::FrameSlots) * kMaxBatchesPerFrame * 3 *
                                      sizeof(D3D12_NODE_GPU_INPUT);
    const auto rasterInputDescription = CD3DX12_RESOURCE_DESC::Buffer(rasterInputBytes);
    if (FAILED(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &rasterInputDescription,
                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               IID_PPV_ARGS(&m_RasterGraphInputs))))
    {
        return false;
    }
    return SUCCEEDED(m_RasterGraphInputs->Map(0, &noRead, reinterpret_cast<void**>(&m_MappedRasterGraphInputs)));
}

void InstancingSystem::Init()
{
    m_GpuCullingInitialized = false;
    ID3D12Device* device = GraphicsDevice::GetDevice();
    if (!device)
    {
        return;
    }

    const UINT64 count = static_cast<UINT64>(RendererState::g_kFRAME_COUNT) * kMaxInstancesPerFrame;
    const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    const auto desc = CD3DX12_RESOURCE_DESC::Buffer(count * sizeof(GpuInstanceInput));
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                               nullptr, IID_PPV_ARGS(&m_InstanceUpload))))
    {
        return;
    }
    const D3D12_RANGE noRead{0, 0};
    if (FAILED(m_InstanceUpload->Map(0, &noRead, reinterpret_cast<void**>(&m_MappedInstances))))
    {
        m_InstanceUpload.Reset();
        return;
    }
    const auto directDesc = CD3DX12_RESOURCE_DESC::Buffer(count * sizeof(XMFLOAT4X4));
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &directDesc,
                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               IID_PPV_ARGS(&m_DirectInstanceUpload))))
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
    // 可視判定はすべて GPU（CPU の視錐台判定は行わない）。
    m_GpuCullingInitialized = s_Available;
}

void InstancingSystem::Uninit()
{
    ImpostorRenderer::Shutdown();
    s_Available = false;
    m_GpuCullingInitialized = false;
    if (m_InstanceUpload && m_MappedInstances)
    {
        m_InstanceUpload->Unmap(0, nullptr);
    }
    m_MappedInstances = nullptr;
    m_InstanceUpload.Reset();
    if (m_DirectInstanceUpload && m_MappedDirectInstances)
    {
        m_DirectInstanceUpload->Unmap(0, nullptr);
    }
    m_MappedDirectInstances = nullptr;
    m_DirectInstanceUpload.Reset();
    for (auto& resource : m_LodInstances)
    {
        resource.Reset();
    }
    m_LodCounts.Reset();
    m_IndirectArguments.Reset();
    m_ZeroCountsUpload.Reset();
    m_CullLodRootSignature.Reset();
    m_CullLodGraph.Reset();
    m_ArgsRootSignature.Reset();
    m_ArgsGraph.Reset();
    if (m_RasterGraphInputs && m_MappedRasterGraphInputs)
    {
        m_RasterGraphInputs->Unmap(0, nullptr);
    }
    m_MappedRasterGraphInputs = nullptr;
    m_RasterGraphInputs.Reset();
    g_InstanceRasterGraphs.clear();
    g_InstanceRasterLibrary.Reset();
    g_InstanceShadowLibrary.Reset();
    GpuObjectCulling::Shutdown();
}

// 内部補助メソッド。

struct DrawContext
{
    ID3D12GraphicsCommandList* commandList = nullptr;
    ID3D12DescriptorHeap* heap = nullptr;
    UINT frameIndex = 0;
    XMMATRIX viewProjection{};
    XMMATRIX view{};
    XMMATRIX projection{};
    XMMATRIX previousViewProjection{};
    XMFLOAT3 cameraPosition{};
    XMFLOAT3 eyePosition{}; // ビュー行列の逆から求めた視点（ConstantBuffer3D.CameraPos 用）
    bool transparentPass = false;
    bool deferredOpaque = false;
    bool receivingPostProcessOnly = false;
    bool impostorPass = false;
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
    if (renderPass == RenderPass::OcclusionPhase2)
    {
        return false;
    }
    if (!s_Available || !m_MappedInstances || !m_MappedDirectInstances)
    {
        return false;
    }
    if (renderPass != RenderPass::PrimaryScene && renderPass != RenderPass::OverlayScene &&
        renderPass != RenderPass::ShadowMap)
    {
        return false;
    }

    ctx.commandList = GraphicsDevice::GetCommandList();
    ctx.heap = FrameConstants::GetCbvHeap();
    if (!ctx.commandList || !ctx.heap)
    {
        return false;
    }

    ctx.frameIndex = GraphicsDevice::GetFrameIndex() % RendererState::g_kFRAME_COUNT;
    if (m_FrameIndex != ctx.frameIndex)
    {
        m_FrameIndex = ctx.frameIndex;
        m_FrameCursor = 0;
        m_DirectFrameCursor = 0;
        m_BatchCursor = 0;
    }

    XMMATRIX view = XMMatrixIdentity(), projection = XMMatrixIdentity();
    Camera::GetCameraMatrices(Camera::GetCameraEntity(), view, projection);
    ctx.viewProjection = view * projection;
    ctx.view = view;
    ctx.projection = projection;
    ctx.previousViewProjection = XMLoadFloat4x4(&GraphicsDevice::GetPreviousViewMatrix()) *
                                 XMLoadFloat4x4(&GraphicsDevice::GetPreviousProjectionMatrix());
    {
        XMVECTOR determinant{};
        XMStoreFloat3(&ctx.eyePosition, XMMatrixInverse(&determinant, view).r[3]);
    }

    const EntityID cameraEntity = Camera::GetCameraEntity();
    if (ComponentManager::HasComponent<TransformComponent>(cameraEntity))
    {
        ctx.cameraPosition = ComponentManager::GetComponentUnchecked<TransformComponent>(cameraEntity).Position;
    }

    ctx.transparentPass = renderPass == RenderPass::OverlayScene;
    ctx.deferredOpaque =
        RenderConfiguration::GetRenderMode() == RenderMode::DEFERRED && renderPass == RenderPass::PrimaryScene;
    ctx.defaultTexture = TextureManager::GetDefaultTextureIndex();
    return true;
}

bool InstancingSystem::AcceptsPass(EntityID entity, const MaterialComponent* material, const DrawContext& ctx)
{
    const bool transparent = material && MaterialSystem::IsTransparentMaterial(*material);
    if (transparent != ctx.transparentPass)
    {
        return false;
    }
    return ctx.transparentPass || ctx.deferredOpaque ||
           MaterialSystem::IsReceivingPostProcess(entity) == ctx.receivingPostProcessOnly;
}

bool InstancingSystem::IsCameraVisible(EntityID entity, const XMFLOAT3& center, const XMFLOAT3& extents, bool hasBounds,
                                       const DrawContext& ctx)
{
    // 視錐台の判定は GpuInstanceCullLodCS が行う。CPU では候補から外さない。
    (void)entity;
    (void)center;
    (void)extents;
    (void)hasBounds;
    (void)ctx;
    return true;
}

void InstancingSystem::BuildShadowBatches(DrawContext& ctx, BatchBuilder& builder)
{
    ID3D12PipelineState* shadowPso = PsoManager::GetOrCreateShadowMapInstancedPso();
    if (!shadowPso)
    {
        return;
    }

    // アニメーションモデル。
    for (EntityID entity : World::GetView<AnimationModelComponent, TransformComponent>())
    {
        if (!CanInstance(entity) || !ShouldCastShadow(entity) ||
            !LightingResources::ShouldDrawEntityInCurrentShadowSubset(entity))
        {
            continue;
        }
        const auto& animation = ComponentManager::GetComponentUnchecked<AnimationModelComponent>(entity);
        AnimationModelResource* model = ModelManager::GetAnimModel(animation.ModelId);
        if (!model)
        {
            continue;
        }

        for (UINT meshIndex = 0; meshIndex < model->GetMeshCount(); ++meshIndex)
        {
            const MeshData& mesh = model->GetMeshData(meshIndex);
            if (!mesh.VertexBuffer || !mesh.IndexBuffer || mesh.IndexCount == 0)
            {
                continue;
            }

            int textureIndexValue;
            if (mesh.TextureIndex >= 0)
            {
                textureIndexValue = mesh.TextureIndex;
            }
            else
            {
                textureIndexValue = -1;
            }
            BatchKey key = MakeKey(entity, InstanceKind::AnimatedMesh, shadowPso, mesh.VertexBufferView.BufferLocation,
                                   0, mesh.VertexCount, mesh.IndexBufferView.BufferLocation, mesh.IndexCount,
                                   textureIndexValue, -1, nullptr, meshIndex, &animation);
            builder.Add(entity, CreateAnimShadowBatch(mesh, model, meshIndex, shadowPso), key);
        }
    }

    // 静的モデル。
    for (EntityID entity : World::GetView<StaticModelComponent, TransformComponent>())
    {
        if (!CanInstance(entity) || !ShouldCastShadow(entity) ||
            !LightingResources::ShouldDrawEntityInCurrentShadowSubset(entity))
        {
            continue;
        }
        const auto& component = ComponentManager::GetComponentUnchecked<StaticModelComponent>(entity);
        StaticModelResource* model = ModelManager::GetStaticModel(component.ModelId);
        if (!model)
        {
            continue;
        }

        for (UINT meshIndex = 0; meshIndex < model->GetMeshCount(); ++meshIndex)
        {
            const StaticMeshData& mesh = model->GetMeshData(meshIndex);
            if (!mesh.VertexBuffer || !mesh.IndexBuffer || mesh.IndexCount == 0)
            {
                continue;
            }

            int textureIndex3;
            if (mesh.TextureIndex >= 0)
            {
                textureIndex3 = mesh.TextureIndex;
            }
            else
            {
                textureIndex3 = -1;
            }
            BatchKey key = MakeKey(entity, InstanceKind::StaticMesh, shadowPso, mesh.VertexBufferView.BufferLocation, 0,
                                   mesh.VertexCount, mesh.IndexBufferView.BufferLocation, mesh.IndexCount,
                                   textureIndex3, -1, nullptr, meshIndex, nullptr);
            builder.Add(entity, CreateStaticShadowBatch(mesh, model, meshIndex, shadowPso), key);
        }
    }

    // インデックスを使わない描画（スプライト・メッシュ）。
    for (EntityID entity : World::GetView<SpriteComponent, TransformComponent>())
    {
        const auto& sprite = ComponentManager::GetComponentUnchecked<SpriteComponent>(entity);
        if (CanInstance(entity) && ShouldCastShadow(entity) && sprite.Is3D &&
            LightingResources::ShouldDrawEntityInCurrentShadowSubset(entity) &&
            sprite.VertexBufferView.BufferLocation != 0 && sprite.VertexCount != 0)
        {
            BatchKey key = MakeKey(entity, InstanceKind::Sprite3D, shadowPso, sprite.VertexBufferView.BufferLocation,
                                   sprite.GeometryHash, sprite.VertexCount, 0, 0, -1, -1, nullptr);
            builder.Add(entity, CreateNonIndexedShadowBatch(sprite, shadowPso, key), key);
        }
    }
    for (EntityID entity : World::GetView<MeshComponent, TransformComponent>())
    {
        if (ComponentManager::HasComponent<AnimationModelComponent>(entity) ||
            ComponentManager::HasComponent<StaticModelComponent>(entity))
        {
            continue;
        }
        const auto& mesh = ComponentManager::GetComponentUnchecked<MeshComponent>(entity);
        if (CanInstance(entity) && ShouldCastShadow(entity) &&
            LightingResources::ShouldDrawEntityInCurrentShadowSubset(entity) &&
            mesh.VertexBufferView.BufferLocation != 0 && mesh.VertexCount != 0)
        {
            BatchKey key = MakeKey(entity, InstanceKind::Mesh, shadowPso, mesh.VertexBufferView.BufferLocation,
                                   mesh.GeometryHash, mesh.VertexCount, 0, 0, -1, -1, nullptr);
            builder.Add(entity, CreateNonIndexedShadowBatch(mesh, shadowPso, key), key);
        }
    }
}

InstanceBatch InstancingSystem::CreateAnimShadowBatch(const MeshData& mesh, AnimationModelResource* model,
                                                      UINT meshIndex, ID3D12PipelineState* pso)
{
    InstanceBatch batch{};
    batch.Kind = InstanceKind::AnimatedMesh;
    batch.Pso = pso;
    batch.VertexBuffer = mesh.VertexBufferView;
    batch.VertexResource = mesh.VertexBuffer.Get();
    batch.IndexBuffer = mesh.IndexBufferView;
    batch.IndexCount = mesh.IndexCount;
    batch.LodIndexBuffers[0] = mesh.IndexBufferView;
    batch.LodIndexResources[0] = mesh.IndexBuffer.Get();
    batch.LodDrawCounts[0] = mesh.IndexCount;
    batch.AvailableLodCount = 1;
    for (UINT lod = 1; lod < MeshData::LodCount; ++lod)
    {
        batch.LodIndexBuffers[lod] = mesh.GetLodIndexBufferView(lod);
        batch.LodIndexResources[lod] = mesh.IndexBuffer.Get();
        if (mesh.LodIndexCounts[lod - 1] != 0)
        {
            batch.LodIndexResources[lod] = mesh.LodIndexBuffers[lod - 1].Get();
        }
        batch.LodDrawCounts[lod] = mesh.GetLodIndexCount(lod);
        if (mesh.LodIndexCounts[lod - 1] != 0)
        {
            batch.AvailableLodCount = lod + 1;
        }
    }
    batch.AnimatedModel = model;
    batch.AnimatedMeshIndex = meshIndex;
    return batch;
}

InstanceBatch InstancingSystem::CreateStaticShadowBatch(const StaticMeshData& mesh, StaticModelResource* model,
                                                        UINT meshIndex, ID3D12PipelineState* pso)
{
    InstanceBatch batch{};
    batch.Kind = InstanceKind::StaticMesh;
    batch.Pso = pso;
    batch.VertexBuffer = mesh.VertexBufferView;
    batch.VertexResource = mesh.VertexBuffer.Get();
    batch.IndexBuffer = mesh.IndexBufferView;
    batch.IndexCount = mesh.IndexCount;
    batch.LodIndexBuffers[0] = mesh.IndexBufferView;
    batch.LodIndexResources[0] = mesh.IndexBuffer.Get();
    batch.LodDrawCounts[0] = mesh.IndexCount;
    for (UINT lod = 1; lod < min(StaticMeshData::LodCount, static_cast<UINT>(batch.LodIndexBuffers.size())); ++lod)
    {
        batch.LodIndexBuffers[lod] = mesh.GetLodIndexBufferView(lod);
        batch.LodIndexResources[lod] = mesh.IndexBuffer.Get();
        if (mesh.LodIndexCounts[lod - 1] != 0)
        {
            batch.LodIndexResources[lod] = mesh.LodIndexBuffers[lod - 1].Get();
        }
        batch.LodDrawCounts[lod] = mesh.GetLodIndexCount(lod);
        if (mesh.LodIndexCounts[lod - 1] != 0)
        {
            batch.AvailableLodCount = lod + 1;
        }
    }
    batch.BoundsCenter = model->GetAabbCenter();
    batch.BoundsExtents = model->GetAabbExtents();
    batch.HasBounds = true;
    batch.StaticModel = model;
    batch.AnimatedMeshIndex = meshIndex;
    return batch;
}

InstanceBatch InstancingSystem::CreateNonIndexedShadowBatch(const SpriteComponent& sprite, ID3D12PipelineState* pso,
                                                            const BatchKey&)
{
    InstanceBatch batch{};
    batch.Kind = InstanceKind::Sprite3D;
    batch.Pso = pso;
    batch.VertexBuffer = sprite.VertexBufferView;
    batch.VertexResource = sprite.VertexBuffer.Get();
    batch.VertexCount = sprite.VertexCount;
    batch.LodDrawCounts = {sprite.VertexCount, sprite.VertexCount, sprite.VertexCount};
    batch.BoundsCenter = sprite.LocalBoundsCenter;
    batch.BoundsExtents = sprite.LocalBoundsExtents;
    batch.HasBounds = sprite.HasLocalBounds;
    return batch;
}

InstanceBatch InstancingSystem::CreateNonIndexedShadowBatch(const MeshComponent& mesh, ID3D12PipelineState* pso,
                                                            const BatchKey&)
{
    InstanceBatch batch{};
    batch.Kind = InstanceKind::Mesh;
    batch.Pso = pso;
    batch.VertexBuffer = mesh.VertexBufferView;
    batch.VertexResource = mesh.VertexBuffer.Get();
    batch.VertexCount = mesh.VertexCount;
    batch.LodDrawCounts = {mesh.VertexCount, mesh.VertexCount, mesh.VertexCount};
    batch.BoundsCenter = mesh.LocalBoundsCenter;
    batch.BoundsExtents = mesh.LocalBoundsExtents;
    batch.HasBounds = mesh.HasLocalBounds;
    return batch;
}

void InstancingSystem::ExecuteShadowBatches(const DrawContext& ctx, vector<InstanceBatch>& batches)
{
    if (batches.empty())
    {
        return;
    }

    ctx.commandList->SetGraphicsRootSignature(ShaderPipelines::GetModelRootSignature());
    if (LightingResources::GetShadowCB())
    {
        ctx.commandList->SetGraphicsRootConstantBufferView(5,
                                                           LightingResources::GetCurrentShadowConstantBufferAddress());
    }

    ID3D12PipelineState* shadowPso = PsoManager::GetOrCreateShadowMapInstancedPso();
    ctx.commandList->SetPipelineState(shadowPso);
    ctx.commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    const UINT shadowLodBias = LightingResources::GetCurrentShadowLodBias();
    unordered_set<AnimationModelResource*> skinnedModels;

    // 影パスの視錐台判定・LOD 選択は GPU。行列が無い（パス外）ときは視錐台判定なし。
    GpuCullView view{};
    view.MinimumLod = shadowLodBias;
    XMMATRIX transposedViewProjection{};
    if (LightingResources::GetCurrentShadowCullMatrix(transposedViewProjection))
    {
        const XMVECTOR c0 = transposedViewProjection.r[0];
        const XMVECTOR c1 = transposedViewProjection.r[1];
        const XMVECTOR c2 = transposedViewProjection.r[2];
        const XMVECTOR c3 = transposedViewProjection.r[3];
        const XMVECTOR planes[6] = {XMVectorAdd(c3, c0), XMVectorSubtract(c3, c0), XMVectorAdd(c3, c1),
                                    XMVectorSubtract(c3, c1), c2, XMVectorSubtract(c3, c2)};
        for (int i = 0; i < 6; ++i)
        {
            XMStoreFloat4(&view.Planes[i], XMPlaneNormalize(planes[i]));
        }
        view.EnableFrustum = true;
    }
    vector<GpuBatchRecord> records;
    if (!CullBatchesOnGpu(ctx, batches, view, records))
    {
        return;
    }
    ctx.commandList->SetGraphicsRootSignature(ShaderPipelines::GetModelRootSignature());
    if (LightingResources::GetShadowCB())
    {
        ctx.commandList->SetGraphicsRootConstantBufferView(5, LightingResources::GetCurrentShadowConstantBufferAddress());
    }
    ctx.commandList->SetPipelineState(shadowPso);
    size_t batchIndex = 0;

    for (auto& batch : batches)
    {
        if (batch.AnimatedModel && skinnedModels.insert(batch.AnimatedModel).second)
        {
            batch.AnimatedModel->DispatchGpuSkinning(ctx.commandList, false);
            ctx.commandList->SetGraphicsRootSignature(ShaderPipelines::GetModelRootSignature());
            ctx.commandList->SetGraphicsRootConstantBufferView(
                5, LightingResources::GetCurrentShadowConstantBufferAddress());
            ctx.commandList->SetPipelineState(shadowPso);
        }

        const MeshData* animatedMesh;
        if (batch.AnimatedModel)
        {
            animatedMesh = &batch.AnimatedModel->GetMeshData(batch.AnimatedMeshIndex);
        }
        else
        {
            animatedMesh = nullptr;
        }
        if (animatedMesh)
        {
            const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(animatedMesh->VertexBuffer.Get(),
                                                                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                                      D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
            ctx.commandList->ResourceBarrier(1, &barrier);
        }

        auto bindShadowGraphics = [&]() {
            ctx.commandList->SetGraphicsRootSignature(ShaderPipelines::GetModelRootSignature());
            ctx.commandList->SetGraphicsRootConstantBufferView(
                5, LightingResources::GetCurrentShadowConstantBufferAddress());
            ctx.commandList->SetPipelineState(shadowPso);
        };

        if (batchIndex < records.size() && records[batchIndex].Valid)
        {
            DrawCulledBatch(ctx, batch, records[batchIndex], bindShadowGraphics);
        }
        ++batchIndex;

        if (animatedMesh)
        {
            const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(animatedMesh->VertexBuffer.Get(),
                                                                      D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
                                                                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ctx.commandList->ResourceBarrier(1, &barrier);
        }
    }
    FinishGpuCulling(ctx);
}

void InstancingSystem::BuildMainBatches(DrawContext& ctx, BatchBuilder& builder)
{
    auto addEntity = [&](EntityID entity, InstanceBatch prototype, const BatchKey& key) {
        builder.Add(entity, prototype, key);
    };

    // スプライト。
    for (EntityID entity : World::GetView<SpriteComponent, TransformComponent>())
    {
        if (!CanInstance(entity))
        {
            continue;
        }
        const auto& sprite = ComponentManager::GetComponentUnchecked<SpriteComponent>(entity);
        if (!sprite.Is3D || sprite.VertexBufferView.BufferLocation == 0 || sprite.VertexCount == 0)
        {
            continue;
        }
        if (!IsCameraVisible(entity, sprite.LocalBoundsCenter, sprite.LocalBoundsExtents, sprite.HasLocalBounds, ctx))
        {
            continue;
        }

        const MaterialComponent* material;
        if (ComponentManager::HasComponent<MaterialComponent>(entity))
        {
            material = &ComponentManager::GetComponentUnchecked<MaterialComponent>(entity);
        }
        else
        {
            material = nullptr;
        }
        if (!AcceptsPass(entity, material, ctx))
        {
            continue;
        }

        int texture;
        if (material && material->UseTexture && material->TextureID >= 0)
        {
            texture = material->TextureID;
        }
        else
        {
            texture = ctx.defaultTexture;
        }
        int normal;
        if (material && material->NormalMapID >= 0)
        {
            normal = material->NormalMapID;
        }
        else
        {
            normal = ctx.defaultTexture;
        }

        ShaderDescription resource{};
        resource.vsPath = "shader/hlsl/build/colorshader3dInstancedVS.cso";
        resource.psPath = ResolvePixelShader(entity, InstanceKind::Sprite3D);
        resource.isModel = true;
        resource.enableAlphaBlend = ctx.transparentPass;
        ID3D12PipelineState* pso = PsoManager::GetOrCreateGraphicsPso(resource);
        if (!pso)
        {
            continue;
        }

        InstanceBatch batch{};
        batch.Kind = InstanceKind::Sprite3D;
        batch.Pso = pso;
        batch.VertexBuffer = sprite.VertexBufferView;
    batch.VertexResource = sprite.VertexBuffer.Get();
        batch.VertexCount = sprite.VertexCount;
        batch.LodDrawCounts = {sprite.VertexCount, sprite.VertexCount, sprite.VertexCount};
        batch.TextureIndex = texture;
        batch.NormalIndex = normal;
        batch.Material = material;
        batch.BoundsCenter = sprite.LocalBoundsCenter;
        batch.BoundsExtents = sprite.LocalBoundsExtents;
        batch.HasBounds = sprite.HasLocalBounds;

        BatchKey key = MakeKey(entity, batch.Kind, pso, batch.VertexBuffer.BufferLocation, sprite.GeometryHash,
                               batch.VertexCount, 0, 0, texture, normal, material);
        addEntity(entity, batch, key);
    }

    // メッシュ。
    for (EntityID entity : World::GetView<MeshComponent, TransformComponent>())
    {
        if (!ctx.transparentPass && GPUDrivenRenderer::HandlesEntity(entity))
        {
            continue;
        }
        if (!CanInstance(entity) || ComponentManager::HasComponent<AnimationModelComponent>(entity) ||
            ComponentManager::HasComponent<StaticModelComponent>(entity))
        {
            continue;
        }
        const auto& mesh = ComponentManager::GetComponentUnchecked<MeshComponent>(entity);
        if (mesh.VertexBufferView.BufferLocation == 0 || mesh.VertexCount == 0)
        {
            continue;
        }
        if (!IsCameraVisible(entity, mesh.LocalBoundsCenter, mesh.LocalBoundsExtents, mesh.HasLocalBounds, ctx))
        {
            continue;
        }

        const MaterialComponent* material;
        if (ComponentManager::HasComponent<MaterialComponent>(entity))
        {
            material = &ComponentManager::GetComponentUnchecked<MaterialComponent>(entity);
        }
        else
        {
            material = nullptr;
        }
        if (!AcceptsPass(entity, material, ctx))
        {
            continue;
        }

        int texture;
        if (material && material->UseTexture && material->TextureID >= 0)
        {
            texture = material->TextureID;
        }
        else
        {
            texture = ctx.defaultTexture;
        }
        int normal;
        if (material && material->NormalMapID >= 0)
        {
            normal = material->NormalMapID;
        }
        else
        {
            normal = ctx.defaultTexture;
        }

        ShaderDescription resource{};
        resource.vsPath = "shader/hlsl/build/modelshaderInstancedVS.cso";
        resource.psPath = ResolvePixelShader(entity, InstanceKind::Mesh);
        resource.isModel = true;
        resource.enableAlphaBlend = ctx.transparentPass;
        ID3D12PipelineState* pso = PsoManager::GetOrCreateGraphicsPso(resource);
        if (!pso)
        {
            continue;
        }

        InstanceBatch batch{};
        batch.Kind = InstanceKind::Mesh;
        batch.Pso = pso;
        batch.VertexBuffer = mesh.VertexBufferView;
    batch.VertexResource = mesh.VertexBuffer.Get();
        batch.VertexCount = mesh.VertexCount;
        batch.LodDrawCounts = {mesh.VertexCount, mesh.VertexCount, mesh.VertexCount};
        batch.TextureIndex = texture;
        batch.NormalIndex = normal;
        batch.Material = material;
        batch.BoundsCenter = mesh.LocalBoundsCenter;
        batch.BoundsExtents = mesh.LocalBoundsExtents;
        batch.HasBounds = mesh.HasLocalBounds;

        BatchKey key = MakeKey(entity, batch.Kind, pso, batch.VertexBuffer.BufferLocation, mesh.GeometryHash,
                               batch.VertexCount, 0, 0, texture, normal, material);
        addEntity(entity, batch, key);
    }

    // アニメーションモデル。
    for (EntityID entity : World::GetView<AnimationModelComponent, TransformComponent>())
    {
        if (!CanInstance(entity))
        {
            continue;
        }
        const auto& animation = ComponentManager::GetComponentUnchecked<AnimationModelComponent>(entity);
        AnimationModelResource* model = ModelManager::GetAnimModel(animation.ModelId);
        if (!model)
        {
            continue;
        }
        if (!IsCameraVisible(entity, model->GetAabbCenter(), model->GetAabbExtents(), true, ctx))
        {
            continue;
        }

        const MaterialComponent* material;
        if (ComponentManager::HasComponent<MaterialComponent>(entity))
        {
            material = &ComponentManager::GetComponentUnchecked<MaterialComponent>(entity);
        }
        else
        {
            material = nullptr;
        }
        if (!AcceptsPass(entity, material, ctx))
        {
            continue;
        }

        int entityTexture;
        if (material && material->UseTexture && material->TextureID >= 0)
        {
            entityTexture = material->TextureID;
        }
        else
        {
            entityTexture = ctx.defaultTexture;
        }
        int normal;
        if (material && material->NormalMapID >= 0)
        {
            normal = material->NormalMapID;
        }
        else
        {
            normal = ctx.defaultTexture;
        }

        for (UINT meshIndex = 0; meshIndex < model->GetMeshCount(); ++meshIndex)
        {
            const MeshData& mesh = model->GetMeshData(meshIndex);
            if (!mesh.VertexBuffer || mesh.VertexBufferView.BufferLocation == 0 || !mesh.IndexBuffer ||
                mesh.IndexBufferView.BufferLocation == 0 || mesh.IndexCount == 0)
            {
                continue;
            }

            int texture;
            if (mesh.TextureIndex >= 0)
            {
                texture = mesh.TextureIndex;
            }
            else
            {
                texture = entityTexture;
            }

            ShaderDescription resource{};
            resource.vsPath = "shader/hlsl/build/modelshaderInstancedVS.cso";
            resource.psPath = ResolvePixelShader(entity, InstanceKind::AnimatedMesh);
            resource.isModel = true;
            resource.enableAlphaBlend = ctx.transparentPass;
            ID3D12PipelineState* pso = PsoManager::GetOrCreateGraphicsPso(resource);
            if (!pso)
            {
                continue;
            }

            InstanceBatch batch{};
            batch.Kind = InstanceKind::AnimatedMesh;
            batch.Pso = pso;
            batch.VertexBuffer = mesh.VertexBufferView;
    batch.VertexResource = mesh.VertexBuffer.Get();
            batch.VertexCount = mesh.VertexCount;
            batch.IndexBuffer = mesh.IndexBufferView;
            batch.IndexCount = mesh.IndexCount;
            batch.LodIndexBuffers[0] = mesh.IndexBufferView;
    batch.LodIndexResources[0] = mesh.IndexBuffer.Get();
            batch.LodDrawCounts[0] = mesh.IndexCount;
            batch.AvailableLodCount = 1;
            for (UINT lod = 1; lod < MeshData::LodCount; ++lod)
            {
                batch.LodIndexBuffers[lod] = mesh.GetLodIndexBufferView(lod);
        batch.LodIndexResources[lod] = mesh.IndexBuffer.Get();
        if (mesh.LodIndexCounts[lod - 1] != 0)
        {
            batch.LodIndexResources[lod] = mesh.LodIndexBuffers[lod - 1].Get();
        }
                batch.LodDrawCounts[lod] = mesh.GetLodIndexCount(lod);
                if (mesh.LodIndexCounts[lod - 1] != 0)
                {
                    batch.AvailableLodCount = lod + 1;
                }
            }
            batch.TextureIndex = texture;
            batch.NormalIndex = normal;
            batch.Material = material;
            batch.BoundsCenter = model->GetAabbCenter();
            batch.BoundsExtents = model->GetAabbExtents();
            batch.HasBounds = true;
            batch.AnimatedModel = model;
            batch.AnimatedMeshIndex = meshIndex;

            BatchKey key = MakeKey(entity, batch.Kind, pso, batch.VertexBuffer.BufferLocation, 0, batch.VertexCount,
                                   batch.IndexBuffer.BufferLocation, batch.IndexCount, texture, normal, material,
                                   meshIndex, &animation);
            addEntity(entity, batch, key);
        }
    }

    // GPU 側の対象インスタンスは、この差分更新用 CPU フォールバック一覧には含まれない。
    const auto staticEntities = !ctx.transparentPass && GPUDrivenRenderer::IsActive()
                                    ? EntityView(&GpuScene::CpuOpaqueStaticEntities())
                                    : World::GetView<StaticModelComponent, TransformComponent>();
    for (EntityID entity : staticEntities)
    {
        if (!CanInstance(entity))
        {
            continue;
        }
        if (!ctx.transparentPass && GPUDrivenRenderer::HandlesEntity(entity))
        {
            continue;
        }
        const auto& component = ComponentManager::GetComponentUnchecked<StaticModelComponent>(entity);
        StaticModelResource* model = ModelManager::GetStaticModel(component.ModelId);
        if (!model)
        {
            continue;
        }
        if (!IsCameraVisible(entity, model->GetAabbCenter(), model->GetAabbExtents(), true, ctx))
        {
            continue;
        }

        const MaterialComponent* material;
        if (ComponentManager::HasComponent<MaterialComponent>(entity))
        {
            material = &ComponentManager::GetComponentUnchecked<MaterialComponent>(entity);
        }
        else
        {
            material = nullptr;
        }
        if (!AcceptsPass(entity, material, ctx))
        {
            continue;
        }

        int entityTexture;
        if (material && material->UseTexture && material->TextureID >= 0)
        {
            entityTexture = material->TextureID;
        }
        else
        {
            entityTexture = ctx.defaultTexture;
        }
        int normal;
        if (material && material->NormalMapID >= 0)
        {
            normal = material->NormalMapID;
        }
        else
        {
            normal = ctx.defaultTexture;
        }

        // インポスター：アトラスが準備済みなら候補として登録するだけ。切替距離の判定は
        // GpuInstanceCullLodCS が行い、遠ければメッシュの代わりにインポスターを出力する。
        if (ctx.impostorPass)
        {
            ImpostorRenderer::RegisterInstanced(entity, model, material, entityTexture);
        }

        for (UINT meshIndex = 0; meshIndex < model->GetMeshCount(); ++meshIndex)
        {
            const StaticMeshData& mesh = model->GetMeshData(meshIndex);
            if (!mesh.VertexBuffer || !mesh.IndexBuffer || mesh.IndexCount == 0)
            {
                continue;
            }

            int texture;
            if (mesh.TextureIndex >= 0)
            {
                texture = mesh.TextureIndex;
            }
            else
            {
                texture = entityTexture;
            }

            ShaderDescription resource{};
            resource.vsPath = "shader/hlsl/build/modelshaderInstancedVS.cso";
            resource.psPath = ResolvePixelShader(entity, InstanceKind::StaticMesh);
            resource.isModel = true;
            resource.enableAlphaBlend = ctx.transparentPass;
            ID3D12PipelineState* pso = PsoManager::GetOrCreateGraphicsPso(resource);
            if (!pso)
            {
                continue;
            }

            InstanceBatch batch{};
            batch.Kind = InstanceKind::StaticMesh;
            batch.Pso = pso;
            batch.VertexBuffer = mesh.VertexBufferView;
    batch.VertexResource = mesh.VertexBuffer.Get();
            batch.VertexCount = mesh.VertexCount;
            batch.IndexBuffer = mesh.IndexBufferView;
            batch.IndexCount = mesh.IndexCount;
            batch.LodIndexBuffers[0] = mesh.IndexBufferView;
    batch.LodIndexResources[0] = mesh.IndexBuffer.Get();
            batch.LodDrawCounts[0] = mesh.IndexCount;
            for (UINT lod = 1; lod < min(StaticMeshData::LodCount, static_cast<UINT>(batch.LodIndexBuffers.size()));
                 ++lod)
            {
                batch.LodIndexBuffers[lod] = mesh.GetLodIndexBufferView(lod);
        batch.LodIndexResources[lod] = mesh.IndexBuffer.Get();
        if (mesh.LodIndexCounts[lod - 1] != 0)
        {
            batch.LodIndexResources[lod] = mesh.LodIndexBuffers[lod - 1].Get();
        }
                batch.LodDrawCounts[lod] = mesh.GetLodIndexCount(lod);
                if (mesh.LodIndexCounts[lod - 1] != 0)
                {
                    batch.AvailableLodCount = lod + 1;
                }
            }
            batch.TextureIndex = texture;
            batch.NormalIndex = normal;
            batch.Material = material;
            batch.BoundsCenter = model->GetAabbCenter();
            batch.BoundsExtents = model->GetAabbExtents();
            batch.HasBounds = true;
            batch.StaticModel = model;
            batch.AnimatedMeshIndex = meshIndex;

            BatchKey key = MakeKey(entity, batch.Kind, pso, batch.VertexBuffer.BufferLocation, 0, batch.VertexCount,
                                   batch.IndexBuffer.BufferLocation, batch.IndexCount, texture, normal, material,
                                   meshIndex, nullptr);
            addEntity(entity, batch, key);
        }
    }
}

namespace
{
    // インスタンス描画のバッチ用 b0（ConstantBuffer3D）。
    // 以前は b0 を一度も設定しておらず、直前に描いた別エンティティの定数（MaterialMode / ShaderClass /
    // Metallic / Roughness / UseTexture / UseNormalMap / FlipNormal など）をそのまま使っていた。
    // インポスターを有効にするとモデルが GPU Driven からこの経路へ移るため、近距離のシェーディングが変わっていた。
    // ModelSystem（通常の CPU 描画）と同じ規則で、バッチ先頭エンティティのマテリアルから組み立てる。
    ConstantBuffer3D BuildInstancedBatchConstants(const DrawContext& ctx, EntityID entity,
                                                  const MaterialComponent* material)
    {
        ConstantBuffer3D cb{};
        XMMATRIX world = XMMatrixIdentity();
        XMMATRIX previousWorld = XMMatrixIdentity();
        if (ComponentManager::HasComponent<TransformComponent>(entity))
        {
            const auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
            world = XMLoadFloat4x4(&transform.WorldMatrix);
            previousWorld = XMLoadFloat4x4(&transform.PreviousWorldMatrix);
        }
        // インスタンス VS は位置に InstanceTransforms を使う。World は PS 側（法線マップの接線基底など）が
        // 参照するため、通常描画と同じく先頭エンティティの行列を入れる。
        cb.World = XMMatrixTranspose(world);
        cb.PreviousWorld = XMMatrixTranspose(previousWorld);
        cb.View = XMMatrixTranspose(ctx.view);
        cb.Projection = XMMatrixTranspose(ctx.projection);
        cb.PreviousViewProjection = XMMatrixTranspose(ctx.previousViewProjection);
        cb.CameraPos = ctx.eyePosition;
        cb.UseTexture = 0;
        cb.UseNormalMap = 0;
        cb.FlipNormal = 0;
        cb.ViewportSize = {max(static_cast<float>(GraphicsDevice::GetSceneWidth()), 1.0f),
                           max(static_cast<float>(GraphicsDevice::GetSceneHeight()), 1.0f)};
        if (material)
        {
            const MaterialComponent& mat = MaterialManager::Resolve(*material);
            cb.UseTexture = static_cast<int>(mat.UseTexture);
            cb.MaterialMetallic = mat.Metallic;
            cb.MaterialRoughness = mat.Roughness;
            cb.MaterialFresnel = mat.Fresnel;
            cb.MaterialAlpha = mat.Alpha;
            cb.MaterialIsTransparent = static_cast<int>(mat.IsTransparent);
            cb.MaterialMode = static_cast<int>(mat.ShaderClassMode);
            cb.ShaderClass = static_cast<int>(mat.ShaderClass);
            cb.ToonOutlineWidth = mat.ToonOutlineWidth;
            cb.ToonOutlineScreenWidth = mat.ToonOutlineScreenWidth;
            cb.ToonOutlineUseScreenSpace =
                static_cast<int>(mat.ToonOutlineWidthModeSetting == ToonOutlineWidthMode::ScreenPixels);
            if (mat.NormalMapID >= 0)
            {
                cb.UseNormalMap = 1;
            }
        }
        return cb;
    }

    D3D12_GPU_DESCRIPTOR_HANDLE AllocateInstancedBatchConstants(const DrawContext& ctx, EntityID entity,
                                                                const MaterialComponent* material)
    {
        const ConstantBuffer3D cb = BuildInstancedBatchConstants(ctx, entity, material);
        D3D12_GPU_DESCRIPTOR_HANDLE handle = FrameConstants::AllocateTransientConstantBuffer(cb);
        if (handle.ptr == 0 && entity < g_kMAX_ENTITIES)
        {
            // 一時 CB が尽きたときは、そのエンティティ専用のスロットへ書く（インスタンス対象は ModelSystem が書かない）。
            if (UINT8* base = FrameConstants::GetConstantBufferPtr())
            {
                memcpy(base + static_cast<size_t>(entity) * FrameConstants::g_kCB_ALIGNED_SIZE, &cb, sizeof(cb));
                handle = FrameConstants::GetConstantBufferHandle(entity);
            }
        }
        return handle;
    }
} // namespace

void InstancingSystem::ExecuteMainBatches(const DrawContext& ctx, vector<InstanceBatch>& batches)
{
    // 視錐台・LOD・インポスター置き換えは GPU。全バッチを先にまとめてカリングしてから描く。
    GpuCullView view{};
    {
        XMFLOAT4X4 m;
        XMStoreFloat4x4(&m, ctx.viewProjection);
        const XMFLOAT4 planes[6] = {{m._14 + m._11, m._24 + m._21, m._34 + m._31, m._44 + m._41},
                                    {m._14 - m._11, m._24 - m._21, m._34 - m._31, m._44 - m._41},
                                    {m._14 + m._12, m._24 + m._22, m._34 + m._32, m._44 + m._42},
                                    {m._14 - m._12, m._24 - m._22, m._34 - m._32, m._44 - m._42},
                                    {m._13, m._23, m._33, m._43},
                                    {m._14 - m._13, m._24 - m._23, m._34 - m._33, m._44 - m._43}};
        for (int i = 0; i < 6; ++i)
        {
            XMStoreFloat4(&view.Planes[i], XMPlaneNormalize(XMLoadFloat4(&planes[i])));
        }
    }
    view.EnableFrustum = true;
    view.Impostor = ctx.impostorPass;
    vector<GpuBatchRecord> records;
    if (!CullBatchesOnGpu(ctx, batches, view, records))
    {
        return;
    }
    size_t batchIndex = 0;
    for (auto& batch : batches)
    {
        const size_t currentBatch = batchIndex++;
        if (batch.Entities.empty() || currentBatch >= records.size() || !records[currentBatch].Valid)
        {
            continue;
        }

        if (batch.Kind == InstanceKind::AnimatedMesh && batch.AnimatedModel)
        {
            batch.AnimatedModel->DispatchGpuSkinning(ctx.commandList, false);
            const auto ready = CD3DX12_RESOURCE_BARRIER::Transition(
                batch.AnimatedModel->GetMeshData(batch.AnimatedMeshIndex).VertexBuffer.Get(),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
            ctx.commandList->ResourceBarrier(1, &ready);
        }

        const D3D12_GPU_DESCRIPTOR_HANDLE batchConstants =
            AllocateInstancedBatchConstants(ctx, batch.Entities[0], batch.Material);
        auto bindGraphics = [&]() {
            ctx.commandList->SetPipelineState(batch.Pso);
            if (batchConstants.ptr != 0)
            {
                ctx.commandList->SetGraphicsRootDescriptorTable(0, batchConstants);
            }
            ctx.commandList->SetGraphicsRootDescriptorTable(
                1, CD3DX12_GPU_DESCRIPTOR_HANDLE(ctx.heap->GetGPUDescriptorHandleForHeapStart(), batch.TextureIndex,
                                                 FrameConstants::GetCbvIncrementSize()));
            ctx.commandList->SetGraphicsRootDescriptorTable(
                6, CD3DX12_GPU_DESCRIPTOR_HANDLE(ctx.heap->GetGPUDescriptorHandleForHeapStart(), batch.NormalIndex,
                                                 FrameConstants::GetCbvIncrementSize()));
            if (batch.Material)
            {
                MaterialBindings::SetMaterial(batch.Entities[0], *batch.Material);
            }
        };

        DrawCulledBatch(ctx, batch, records[currentBatch], bindGraphics);

        if (batch.Kind == InstanceKind::AnimatedMesh && batch.AnimatedModel)
        {
            const auto back = CD3DX12_RESOURCE_BARRIER::Transition(
                batch.AnimatedModel->GetMeshData(batch.AnimatedMeshIndex).VertexBuffer.Get(),
                D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ctx.commandList->ResourceBarrier(1, &back);
        }
    }
    FinishGpuCulling(ctx);
}

bool InstancingSystem::CullBatchesOnGpu(const DrawContext& ctx, vector<InstanceBatch>& batches,
                                        const GpuCullView& view, vector<GpuBatchRecord>& records)
{
    records.assign(batches.size(), GpuBatchRecord{});
    if (!m_GpuCullingInitialized || !m_CullLodGraph.IsInitialized() || !m_ArgsGraph.IsInitialized() || batches.empty())
    {
        return false;
    }

    // 1. 候補（ワールド行列・境界・LOD 距離・インポスター情報）を並べる。判定はしない。
    UINT validCount = 0;
    UINT firstBatch = m_BatchCursor;
    GpuInstanceInput* const frameInputs =
        m_MappedInstances + static_cast<UINT64>(ctx.frameIndex) * kMaxInstancesPerFrame;
    for (size_t index = 0; index < batches.size(); ++index)
    {
        InstanceBatch& batch = batches[index];
        if (batch.Entities.empty() || m_FrameCursor + batch.Entities.size() > kMaxInstancesPerFrame ||
            m_BatchCursor >= kMaxBatchesPerFrame)
        {
            continue;
        }
        GpuBatchRecord& record = records[index];
        record.FirstInput = m_FrameCursor;
        record.Count = static_cast<UINT>(batch.Entities.size());
        record.BatchIndex = m_BatchCursor++;
        record.Valid = true;
        ++validCount;

        GpuInstanceInput* destination = frameInputs + record.FirstInput;
        for (EntityID entity : batch.Entities)
        {
            const auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
            GpuInstanceInput input{};
            input.World = transform.WorldMatrix;
            XMFLOAT3 center = batch.BoundsCenter, extents = batch.BoundsExtents;
            if (ComponentManager::HasComponent<AABBComponent>(entity))
            {
                const auto& bounds = ComponentManager::GetComponentUnchecked<AABBComponent>(entity);
                center = bounds.Center;
                extents = bounds.Extents;
            }
            float enableCull = 1.0f;
            if (!batch.HasBounds && !ComponentManager::HasComponent<AABBComponent>(entity))
            {
                enableCull = 0.0f; // 境界が無い物は以前どおり常に描く
            }
            if (ComponentManager::HasComponent<InstancingComponent>(entity) &&
                !ComponentManager::GetComponentUnchecked<InstancingComponent>(entity).EnableFrustumCulling)
            {
                enableCull = 0.0f;
            }
            input.LocalCenter = {center.x, center.y, center.z, 1.0f};
            input.LocalExtents = {extents.x, extents.y, extents.z, 0.0f};

            float lod1 = FLT_MAX;
            float lod2 = FLT_MAX;
            if (ComponentManager::HasComponent<LODComponent>(entity))
            {
                const auto& lod = ComponentManager::GetComponentUnchecked<LODComponent>(entity);
                if (lod.UseLOD)
                {
                    lod1 = max(lod.Lod1Distance, 0.0f);
                    lod2 = max(lod.Lod2Distance, lod1);
                }
            }
            input.LodDistances = {lod1, lod2, enableCull, 0.0f};
            if (view.Impostor && batch.Kind == InstanceKind::StaticMesh)
            {
                const auto info = ImpostorRenderer::GetInstancedInfo(entity);
                if (info.Valid)
                {
                    input.Impostor = info.CenterDistance;
                    input.ImpostorInfo = {info.Group, info.Base, info.Capacity, 1u};
                }
            }
            *destination++ = input;
        }
        m_FrameCursor += record.Count;
    }
    if (validCount == 0)
    {
        return false;
    }
    const UINT batchCount = m_BatchCursor - firstBatch;

    // インポスターの出力先（PrepareGpu が UAV 状態にしている）。使わないときは LOD 用バッファを仮に結ぶ。
    D3D12_GPU_VIRTUAL_ADDRESS impostorInstances = m_LodInstances[0]->GetGPUVirtualAddress();
    D3D12_GPU_VIRTUAL_ADDRESS impostorArgs = m_LodCounts->GetGPUVirtualAddress();
    const bool impostor = view.Impostor && ImpostorRenderer::GetGpuOutputs(impostorInstances, impostorArgs);

    ID3D12GraphicsCommandList* list = ctx.commandList;
    // 2. 件数を 0 にして、出力を UAV へ。
    {
        D3D12_RESOURCE_BARRIER barriers[4] = {
            CD3DX12_RESOURCE_BARRIER::Transition(m_LodCounts.Get(), D3D12_RESOURCE_STATE_COMMON,
                                                 D3D12_RESOURCE_STATE_COPY_DEST),
            CD3DX12_RESOURCE_BARRIER::Transition(m_LodInstances[0].Get(), D3D12_RESOURCE_STATE_COMMON,
                                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            CD3DX12_RESOURCE_BARRIER::Transition(m_LodInstances[1].Get(), D3D12_RESOURCE_STATE_COMMON,
                                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            CD3DX12_RESOURCE_BARRIER::Transition(m_LodInstances[2].Get(), D3D12_RESOURCE_STATE_COMMON,
                                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
        list->ResourceBarrier(_countof(barriers), barriers);
        list->CopyBufferRegion(m_LodCounts.Get(), static_cast<UINT64>(firstBatch) * 3 * sizeof(UINT),
                               m_ZeroCountsUpload.Get(), 0, static_cast<UINT64>(batchCount) * 3 * sizeof(UINT));
        const auto toUav = CD3DX12_RESOURCE_BARRIER::Transition(m_LodCounts.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResourceBarrier(1, &toUav);
    }

    // 3. カリング + LOD + インポスター（GPU）。
    list->SetComputeRootSignature(m_CullLodRootSignature.Get());
    list->SetComputeRootUnorderedAccessView(1, m_LodInstances[0]->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(2, m_LodInstances[1]->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(3, m_LodInstances[2]->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(4, m_LodCounts->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(5, impostorInstances);
    list->SetComputeRootUnorderedAccessView(6, impostorArgs);
    struct CullConstants
    {
        XMFLOAT4 Planes[6];
        XMFLOAT4 CameraPosition;
        UINT CandidateCount;
        UINT AvailableLodCount;
        UINT EnableFrustumCulling;
        UINT MinimumLod;
        UINT OutputBase;
        UINT CountOffset;
        UINT EnableImpostor;
        UINT Padding;
    };
    static_assert(sizeof(CullConstants) == 36 * 4);
    const D3D12_GPU_VIRTUAL_ADDRESS inputBase =
        m_InstanceUpload->GetGPUVirtualAddress() +
        static_cast<UINT64>(ctx.frameIndex) * kMaxInstancesPerFrame * sizeof(GpuInstanceInput);
    for (size_t index = 0; index < batches.size(); ++index)
    {
        const GpuBatchRecord& record = records[index];
        if (!record.Valid)
        {
            continue;
        }
        CullConstants constants{};
        memcpy(constants.Planes, view.Planes, sizeof(constants.Planes));
        constants.CameraPosition = {ctx.cameraPosition.x, ctx.cameraPosition.y, ctx.cameraPosition.z, 1.0f};
        constants.CandidateCount = record.Count;
        constants.AvailableLodCount = max(batches[index].AvailableLodCount, 1u);
        constants.EnableFrustumCulling = view.EnableFrustum ? 1u : 0u;
        constants.MinimumLod = view.MinimumLod;
        constants.OutputBase = record.FirstInput;
        constants.CountOffset = record.BatchIndex * 3 * sizeof(UINT);
        constants.EnableImpostor = impostor ? 1u : 0u;
        list->SetComputeRootShaderResourceView(0, inputBase + static_cast<UINT64>(record.FirstInput) *
                                                                  sizeof(GpuInstanceInput));
        list->SetComputeRoot32BitConstants(7, 36, &constants, 0);
        const UINT dispatchGrid[3] = {(record.Count + 63) / 64, 1, 1};
        m_CullLodGraph.DispatchCpu(list, ctx.frameIndex, dispatchGrid, sizeof(dispatchGrid));
    }

    // 4. 件数 → ExecuteIndirect の引数（GPU）。
    {
        D3D12_RESOURCE_BARRIER barriers[2] = {
            CD3DX12_RESOURCE_BARRIER::Transition(m_LodCounts.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(m_IndirectArguments.Get(), D3D12_RESOURCE_STATE_COMMON,
                                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
        list->ResourceBarrier(_countof(barriers), barriers);
    }
    list->SetComputeRootSignature(m_ArgsRootSignature.Get());
    list->SetComputeRootShaderResourceView(0, m_LodCounts->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(1, m_IndirectArguments->GetGPUVirtualAddress());
    for (size_t index = 0; index < batches.size(); ++index)
    {
        const GpuBatchRecord& record = records[index];
        if (!record.Valid)
        {
            continue;
        }
        const InstanceBatch& batch = batches[index];
        const UINT constants[6] = {batch.LodDrawCounts[0],
                                   batch.LodDrawCounts[1],
                                   batch.LodDrawCounts[2],
                                   batch.IndexCount != 0 ? 1u : 0u,
                                   record.BatchIndex * 3 * static_cast<UINT>(sizeof(UINT)),
                                   record.BatchIndex * 3 * static_cast<UINT>(sizeof(D3D12_DRAW_INDEXED_ARGUMENTS))};
        list->SetComputeRoot32BitConstants(2, 6, constants, 0);
        const UINT argumentRecord = 0;
        m_ArgsGraph.DispatchCpu(list, ctx.frameIndex, &argumentRecord, sizeof(argumentRecord));
    }
    {
        D3D12_RESOURCE_BARRIER barriers[4] = {
            CD3DX12_RESOURCE_BARRIER::Transition(m_IndirectArguments.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                 D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT),
            CD3DX12_RESOURCE_BARRIER::Transition(m_LodInstances[0].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(m_LodInstances[1].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(m_LodInstances[2].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
        list->ResourceBarrier(_countof(barriers), barriers);
    }
    return true;
}

void InstancingSystem::DrawCulledBatch(const DrawContext& ctx, const InstanceBatch& batch,
                                       const GpuBatchRecord& record, const function<void()>& bindGraphics)
{
    if (!record.Valid)
    {
        return;
    }
    const UINT lodCount = min(max(batch.AvailableLodCount, 1u), 3u);
    bool bound = false;
    for (UINT lod = 0; lod < lodCount; ++lod)
    {
        if (batch.LodDrawCounts[lod] == 0)
        {
            continue;
        }
        if (!bound)
        {
            bindGraphics();
            ctx.commandList->IASetVertexBuffers(0, 1, &batch.VertexBuffer);
            ctx.commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            bound = true;
        }
        ctx.commandList->SetGraphicsRootShaderResourceView(
            9, m_LodInstances[lod]->GetGPUVirtualAddress() + static_cast<UINT64>(record.FirstInput) * sizeof(XMFLOAT4X4));
        const UINT64 argumentOffset =
            (static_cast<UINT64>(record.BatchIndex) * 3 + lod) * sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
        // インスタンス数は GPU が書いた値（見えない LOD は 0 件で何も描かれない）。
        if (batch.IndexCount != 0)
        {
            ctx.commandList->IASetIndexBuffer(&batch.LodIndexBuffers[lod]);
            ctx.commandList->ExecuteIndirect(m_DrawIndexedSignature.Get(), 1, m_IndirectArguments.Get(),
                                             argumentOffset, nullptr, 0);
        }
        else
        {
            ctx.commandList->IASetIndexBuffer(nullptr);
            ctx.commandList->ExecuteIndirect(m_DrawSignature.Get(), 1, m_IndirectArguments.Get(), argumentOffset,
                                             nullptr, 0);
        }
    }
}

void InstancingSystem::FinishGpuCulling(const DrawContext& ctx)
{
    // 次の Draw 呼び出しが COMMON から始められるよう戻す。
    D3D12_RESOURCE_BARRIER barriers[5] = {
        CD3DX12_RESOURCE_BARRIER::Transition(m_IndirectArguments.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
                                             D3D12_RESOURCE_STATE_COMMON),
        CD3DX12_RESOURCE_BARRIER::Transition(m_LodCounts.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                             D3D12_RESOURCE_STATE_COMMON),
        CD3DX12_RESOURCE_BARRIER::Transition(m_LodInstances[0].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                             D3D12_RESOURCE_STATE_COMMON),
        CD3DX12_RESOURCE_BARRIER::Transition(m_LodInstances[1].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                             D3D12_RESOURCE_STATE_COMMON),
        CD3DX12_RESOURCE_BARRIER::Transition(m_LodInstances[2].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                             D3D12_RESOURCE_STATE_COMMON)};
    ctx.commandList->ResourceBarrier(_countof(barriers), barriers);
}

void InstancingSystem::Draw(RenderPass renderPass, bool receivingPostProcessOnly)
{
    DrawContext ctx{};
    ctx.receivingPostProcessOnly = receivingPostProcessOnly;
    if (!SetupDrawContext(renderPass, ctx))
    {
        return;
    }

    if (renderPass == RenderPass::ShadowMap)
    {
        BatchBuilder builder;
        BuildShadowBatches(ctx, builder);
        ExecuteShadowBatches(ctx, builder.batches);
        return;
    }

    ctx.impostorPass = renderPass == RenderPass::PrimaryScene && !ctx.transparentPass;
    if (ctx.impostorPass)
    {
        // メイン カメラのエンティティ可視判定（GPU）。CPU 側の間引き（アニメーション更新）は
        // この結果の読み戻しを使うので、CPU 経路の描画が無いフレームでも毎フレーム実行する。
        GpuObjectCulling::CameraView(ctx.commandList);
    }
    if (ctx.impostorPass)
    {
        // GPU Driven 側の候補（置き換えの判定は GPUOcclusionCS）を集める。
        ImpostorRenderer::BeginPass(ctx.deferredOpaque, ctx.receivingPostProcessOnly);
    }
    BatchBuilder builder;
    BuildMainBatches(ctx, builder);
    if (ctx.impostorPass)
    {
        // 候補がそろったので出力領域を確定し、引数の初期化と GPU Driven 側の追加を GPU で行う。
        ImpostorRenderer::FinalizeLayout();
        ImpostorRenderer::PrepareGpu(ctx.commandList);
    }
    ExecuteMainBatches(ctx, builder.batches);
    if (ctx.impostorPass)
    {
        ImpostorRenderer::Flush(ctx.commandList, ctx.frameIndex);
    }
}

