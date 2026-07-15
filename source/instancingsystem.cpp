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
        AnimatedMesh,
		StaticMesh
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
		std::array<D3D12_INDEX_BUFFER_VIEW, 3> LodIndexBuffers{};
		std::array<UINT, 3> LodDrawCounts{};
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

    const MaterialComponent& DefaultMaterial()
    {
        static const MaterialComponent material{};
        return material;
    }

    bool ShouldCastShadow(EntityID entity)
    {
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
            !(material.ShaderClassMode == MaterialMode::Manual &&
                material.ShaderClass == ShaderClass::Shadow);
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
        const InstancingComponent* instancing =
			ComponentManager::HasComponent<InstancingComponent>(entity)
			? &ComponentManager::GetComponentUnchecked<InstancingComponent>(entity)
			: nullptr;
		const uint32_t groupId = instancing ? instancing->GroupId : 0;
        const uint64_t geometryIdentity = groupId != 0
            ? static_cast<uint64_t>(groupId)
            : (geometryHash != 0 ? geometryHash : vertexBuffer);
        string key = to_string(static_cast<UINT>(kind)) + "|" +
            to_string(reinterpret_cast<uintptr_t>(pso)) + "|" +
            to_string(geometryIdentity) + "|" + to_string(vertexCount) + "|" +
            to_string(indexBuffer) + "|" + to_string(indexCount) + "|" +
            to_string(textureIndex) + "|" + to_string(normalIndex) + "|" +
            to_string(HashMaterial(material)) + "|" + to_string(meshIndex) + "|" +
            to_string(groupId);
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
        !ComponentManager::HasComponent<TransformComponent>(entity))
    {
        return false;
    }

	const bool useInstancing =
		ComponentManager::HasComponent<InstancingComponent>(entity) &&
		ComponentManager::GetComponentUnchecked<InstancingComponent>(entity).UseInstancing;
	const bool useLod =
		ComponentManager::HasComponent<LODComponent>(entity) &&
		ComponentManager::GetComponentUnchecked<LODComponent>(entity).UseLOD;
	if (!useInstancing && !useLod)
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
    {
        return false;
    }

    auto createDefaultBuffer = [&](UINT64 size, D3D12_RESOURCE_FLAGS flags,
        D3D12_RESOURCE_STATES state, ComPtr<ID3D12Resource>& resource)
        {
            const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
            const auto description = CD3DX12_RESOURCE_DESC::Buffer(size, flags);
            return SUCCEEDED(device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &description, state,
                nullptr, IID_PPV_ARGS(&resource)));
        };

    const UINT64 transformBytes =
        static_cast<UINT64>(kMaxInstancesPerFrame) * sizeof(XMFLOAT4X4);
    for (auto& resource : m_LodInstances)
    {
        if (!createDefaultBuffer(
            transformBytes,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            resource))
        {
            return false;
        }
    }
    if (!createDefaultBuffer(
        3 * sizeof(UINT), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, m_LodCounts) ||
        !createDefaultBuffer(
            3 * sizeof(D3D12_DRAW_INDEXED_ARGUMENTS),
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            m_IndirectArguments))
    {
        return false;
    }

    const UINT zeros[3]{};
    const auto uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    const auto zeroDescription = CD3DX12_RESOURCE_DESC::Buffer(sizeof(zeros));
    if (FAILED(device->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &zeroDescription,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&m_ZeroCountsUpload))))
    {
        return false;
    }
    void* mappedZeros = nullptr;
    const D3D12_RANGE noRead{ 0, 0 };
    if (FAILED(m_ZeroCountsUpload->Map(0, &noRead, &mappedZeros)))
    {
        return false;
    }
    memcpy(mappedZeros, zeros, sizeof(zeros));
    m_ZeroCountsUpload->Unmap(0, nullptr);

    auto createRootSignature = [&](D3D12_ROOT_PARAMETER* parameters, UINT count,
        ComPtr<ID3D12RootSignature>& rootSignature)
        {
            CD3DX12_ROOT_SIGNATURE_DESC description{};
            description.Init(count, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE);
            ComPtr<ID3DBlob> serialized;
            ComPtr<ID3DBlob> errors;
            if (FAILED(D3D12SerializeRootSignature(
                &description, D3D_ROOT_SIGNATURE_VERSION_1,
                &serialized, &errors)))
            {
                if (errors)
                {
                    Debug::Log("%s\n", static_cast<const char*>(errors->GetBufferPointer()));
                }
                return false;
            }
            return SUCCEEDED(device->CreateRootSignature(
                0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                IID_PPV_ARGS(&rootSignature)));
        };

    CD3DX12_ROOT_PARAMETER cullParameters[6]{};
    cullParameters[0].InitAsShaderResourceView(0);
    cullParameters[1].InitAsUnorderedAccessView(0);
    cullParameters[2].InitAsUnorderedAccessView(1);
    cullParameters[3].InitAsUnorderedAccessView(2);
    cullParameters[4].InitAsUnorderedAccessView(3);
	cullParameters[5].InitAsConstants(32, 0);
    if (!createRootSignature(cullParameters, _countof(cullParameters), m_CullLodRootSignature))
    {
        return false;
    }

    CD3DX12_ROOT_PARAMETER argsParameters[3]{};
    argsParameters[0].InitAsShaderResourceView(0);
    argsParameters[1].InitAsUnorderedAccessView(0);
    argsParameters[2].InitAsConstants(4, 0);
    if (!createRootSignature(argsParameters, _countof(argsParameters), m_ArgsRootSignature))
    {
        return false;
    }

    auto createComputePso = [&](const char* path, ID3D12RootSignature* rootSignature,
        ComPtr<ID3D12PipelineState>& pso)
        {
            rendererResource resource{};
            resource.csoPath = path;
            ComPtr<ID3DBlob> shader;
            resource.ppBlob = shader.GetAddressOf();
            if (!RendererShader::LoadShaderBlob(resource))
            {
                return false;
            }
            D3D12_COMPUTE_PIPELINE_STATE_DESC description{};
            description.pRootSignature = rootSignature;
            description.CS = CD3DX12_SHADER_BYTECODE(shader.Get());
            return SUCCEEDED(device->CreateComputePipelineState(
                &description, IID_PPV_ARGS(&pso)));
        };
    if (!createComputePso(
        "shader/hlsl/build/GpuInstanceCullLodCS.cso",
        m_CullLodRootSignature.Get(), m_CullLodPso) ||
        !createComputePso(
            "shader/hlsl/build/GpuInstanceArgsCS.cso",
            m_ArgsRootSignature.Get(), m_ArgsPso))
    {
        return false;
    }

    D3D12_INDIRECT_ARGUMENT_DESC indexedArgument{};
    indexedArgument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
    D3D12_COMMAND_SIGNATURE_DESC indexedDescription{};
    indexedDescription.ByteStride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
    indexedDescription.NumArgumentDescs = 1;
    indexedDescription.pArgumentDescs = &indexedArgument;
    if (FAILED(device->CreateCommandSignature(
        &indexedDescription, nullptr, IID_PPV_ARGS(&m_DrawIndexedSignature))))
    {
        return false;
    }

    D3D12_INDIRECT_ARGUMENT_DESC drawArgument{};
    drawArgument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
    D3D12_COMMAND_SIGNATURE_DESC drawDescription{};
    // Keep each record aligned to the indexed argument size so both signatures
    // consume the same GPU-generated argument array.
    drawDescription.ByteStride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
    drawDescription.NumArgumentDescs = 1;
    drawDescription.pArgumentDescs = &drawArgument;
    return SUCCEEDED(device->CreateCommandSignature(
        &drawDescription, nullptr, IID_PPV_ARGS(&m_DrawSignature)));
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
    const auto desc = CD3DX12_RESOURCE_DESC::Buffer(count * sizeof(GpuInstanceInput));
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
    s_Available = CreateGpuCullingResources(device);
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
	for (auto& resource : m_LodInstances) resource.Reset();
	m_LodCounts.Reset();
	m_IndirectArguments.Reset();
	m_ZeroCountsUpload.Reset();
	m_CullLodRootSignature.Reset();
	m_CullLodPso.Reset();
    m_ArgsRootSignature.Reset();
	m_ArgsPso.Reset();
	m_DrawIndexedSignature.Reset();
	m_DrawSignature.Reset();
}

void InstancingSystem::Draw(RenderPass renderPass, bool receivingPostProcessOnly)
{
    if (!s_Available || !m_MappedInstances ||
        (renderPass != RenderPass::PrimaryScene &&
            renderPass != RenderPass::OverlayScene &&
            renderPass != RenderPass::ShadowMap))
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
	XMFLOAT3 cameraPosition{};
	const EntityID cameraEntity = Camera::GetCameraEntity();
	if (ComponentManager::HasComponent<TransformComponent>(cameraEntity))
	{
		cameraPosition = ComponentManager::GetComponentUnchecked<TransformComponent>(cameraEntity).Position;
	}

	auto executeGpuCullLod = [&](InstanceBatch& batch, auto&& bindGraphics)
		{
			if (batch.Entities.empty() ||
				m_FrameCursor + batch.Entities.size() > kMaxInstancesPerFrame)
			{
				return;
			}

			const UINT firstInput = m_FrameCursor;
			GpuInstanceInput* destination = m_MappedInstances +
				static_cast<UINT64>(frameIndex) * kMaxInstancesPerFrame + firstInput;
			for (EntityID entity : batch.Entities)
			{
				const auto& transform =
					ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
				destination->World = transform.WorldMatrix;
				XMFLOAT3 center = batch.BoundsCenter;
				XMFLOAT3 extents = batch.BoundsExtents;
				if (ComponentManager::HasComponent<AABBComponent>(entity))
				{
					const auto& bounds = ComponentManager::GetComponentUnchecked<AABBComponent>(entity);
					center = bounds.Center;
					extents = bounds.Extents;
				}
				if (!batch.HasBounds && !ComponentManager::HasComponent<AABBComponent>(entity))
				{
					extents = { 0.5f, 0.5f, 0.5f };
				}
				destination->LocalCenter = { center.x, center.y, center.z, 1.0f };
				destination->LocalExtents = { extents.x, extents.y, extents.z, 0.0f };

				float lod1 = 12.0f;
				float lod2 = 28.0f;
				float enableCull = 1.0f;
				if (ComponentManager::HasComponent<InstancingComponent>(entity))
				{
					const auto& settings =
						ComponentManager::GetComponentUnchecked<InstancingComponent>(entity);
					enableCull = settings.EnableFrustumCulling ? 1.0f : 0.0f;
				}
				if (ComponentManager::HasComponent<LODComponent>(entity) &&
					ComponentManager::GetComponentUnchecked<LODComponent>(entity).UseLOD)
				{
					const auto& lod = ComponentManager::GetComponentUnchecked<LODComponent>(entity);
					lod1 = max(lod.Lod1Distance, 0.0f);
					lod2 = max(lod.Lod2Distance, lod1);
				}
				else
				{
					lod1 = FLT_MAX;
					lod2 = FLT_MAX;
				}
				destination->LodDistances = { lod1, lod2, enableCull, 0.0f };
				++destination;
			}
			m_FrameCursor += static_cast<UINT>(batch.Entities.size());

			const auto countToCopy = CD3DX12_RESOURCE_BARRIER::Transition(
				m_LodCounts.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
				D3D12_RESOURCE_STATE_COPY_DEST);
			commandList->ResourceBarrier(1, &countToCopy);
			commandList->CopyBufferRegion(
				m_LodCounts.Get(), 0, m_ZeroCountsUpload.Get(), 0, 3 * sizeof(UINT));
			const auto countToUav = CD3DX12_RESOURCE_BARRIER::Transition(
				m_LodCounts.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			commandList->ResourceBarrier(1, &countToUav);

			struct CullConstants
			{
				XMFLOAT4 FrustumPlanes[6]{};
				XMFLOAT4 CameraPosition{};
				UINT CandidateCount = 0;
				UINT AvailableLodCount = 1;
				UINT EnableFrustumCulling = 1;
				UINT Padding = 0;
			} constants{};
			XMFLOAT4X4 vp{};
			XMStoreFloat4x4(&vp, viewProjection);
			auto setPlane = [&](UINT planeIndex, float a, float b, float c, float d)
				{
					const float length = sqrtf(a * a + b * b + c * c);
					const float inverseLength = length > 1.0e-6f ? 1.0f / length : 1.0f;
					constants.FrustumPlanes[planeIndex] =
						{ a * inverseLength, b * inverseLength, c * inverseLength, d * inverseLength };
				};
			// DirectXMath uses row vectors. Clip-space planes are therefore
			// extracted from the columns of View * Projection.
			setPlane(0, vp._11 + vp._14, vp._21 + vp._24, vp._31 + vp._34, vp._41 + vp._44);
			setPlane(1, vp._14 - vp._11, vp._24 - vp._21, vp._34 - vp._31, vp._44 - vp._41);
			setPlane(2, vp._12 + vp._14, vp._22 + vp._24, vp._32 + vp._34, vp._42 + vp._44);
			setPlane(3, vp._14 - vp._12, vp._24 - vp._22, vp._34 - vp._32, vp._44 - vp._42);
			setPlane(4, vp._13, vp._23, vp._33, vp._43);
			setPlane(5, vp._14 - vp._13, vp._24 - vp._23, vp._34 - vp._33, vp._44 - vp._43);
			constants.CameraPosition =
				{ cameraPosition.x, cameraPosition.y, cameraPosition.z, 1.0f };
			constants.CandidateCount = static_cast<UINT>(batch.Entities.size());
			constants.AvailableLodCount = batch.AvailableLodCount;

			const UINT64 frameBase = static_cast<UINT64>(frameIndex) * kMaxInstancesPerFrame;
			commandList->SetComputeRootSignature(m_CullLodRootSignature.Get());
			commandList->SetPipelineState(m_CullLodPso.Get());
			commandList->SetComputeRootShaderResourceView(
				0,
				m_InstanceUpload->GetGPUVirtualAddress() +
				(frameBase + firstInput) * sizeof(GpuInstanceInput));
			for (UINT lod = 0; lod < 3; ++lod)
			{
				commandList->SetComputeRootUnorderedAccessView(
					1 + lod, m_LodInstances[lod]->GetGPUVirtualAddress());
			}
			commandList->SetComputeRootUnorderedAccessView(
				4, m_LodCounts->GetGPUVirtualAddress());
			commandList->SetComputeRoot32BitConstants(
				5, 32, &constants, 0);
			commandList->Dispatch((constants.CandidateCount + 63) / 64, 1, 1);

			D3D12_RESOURCE_BARRIER uavBarriers[4] =
			{
				CD3DX12_RESOURCE_BARRIER::UAV(m_LodInstances[0].Get()),
				CD3DX12_RESOURCE_BARRIER::UAV(m_LodInstances[1].Get()),
				CD3DX12_RESOURCE_BARRIER::UAV(m_LodInstances[2].Get()),
				CD3DX12_RESOURCE_BARRIER::UAV(m_LodCounts.Get())
			};
			commandList->ResourceBarrier(_countof(uavBarriers), uavBarriers);

			const auto countToSrv = CD3DX12_RESOURCE_BARRIER::Transition(
				m_LodCounts.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			commandList->ResourceBarrier(1, &countToSrv);
			commandList->SetComputeRootSignature(m_ArgsRootSignature.Get());
			commandList->SetPipelineState(m_ArgsPso.Get());
			commandList->SetComputeRootShaderResourceView(0, m_LodCounts->GetGPUVirtualAddress());
			commandList->SetComputeRootUnorderedAccessView(1, m_IndirectArguments->GetGPUVirtualAddress());
			UINT argumentConstants[4] =
			{
				batch.LodDrawCounts[0],
				batch.LodDrawCounts[1],
				batch.LodDrawCounts[2],
				batch.IndexCount != 0 ? 1u : 0u
			};
			commandList->SetComputeRoot32BitConstants(2, 4, argumentConstants, 0);
			commandList->Dispatch(1, 1, 1);
			const auto argsUav = CD3DX12_RESOURCE_BARRIER::UAV(m_IndirectArguments.Get());
			commandList->ResourceBarrier(1, &argsUav);

			D3D12_RESOURCE_BARRIER ready[4] =
			{
				CD3DX12_RESOURCE_BARRIER::Transition(m_LodInstances[0].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(m_LodInstances[1].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(m_LodInstances[2].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(m_IndirectArguments.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT)
			};
			commandList->ResourceBarrier(_countof(ready), ready);

			for (UINT lod = 0; lod < batch.AvailableLodCount; ++lod)
			{
				bindGraphics();
				commandList->SetGraphicsRootShaderResourceView(
					9, m_LodInstances[lod]->GetGPUVirtualAddress());
				commandList->IASetVertexBuffers(0, 1, &batch.VertexBuffer);
				commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
				if (batch.IndexCount != 0)
				{
					commandList->IASetIndexBuffer(&batch.LodIndexBuffers[lod]);
					commandList->ExecuteIndirect(
						m_DrawIndexedSignature.Get(), 1,
						m_IndirectArguments.Get(),
						lod * sizeof(D3D12_DRAW_INDEXED_ARGUMENTS), nullptr, 0);
				}
				else
				{
					commandList->IASetIndexBuffer(nullptr);
					commandList->ExecuteIndirect(
						m_DrawSignature.Get(), 1,
						m_IndirectArguments.Get(),
						lod * sizeof(D3D12_DRAW_INDEXED_ARGUMENTS), nullptr, 0);
				}
			}

			D3D12_RESOURCE_BARRIER restore[5] =
			{
				CD3DX12_RESOURCE_BARRIER::Transition(m_LodInstances[0].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
				CD3DX12_RESOURCE_BARRIER::Transition(m_LodInstances[1].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
				CD3DX12_RESOURCE_BARRIER::Transition(m_LodInstances[2].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
				CD3DX12_RESOURCE_BARRIER::Transition(m_LodCounts.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
				CD3DX12_RESOURCE_BARRIER::Transition(m_IndirectArguments.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
			};
			commandList->ResourceBarrier(_countof(restore), restore);
		};

    if (renderPass == RenderPass::ShadowMap)
    {
        ID3D12PipelineState* shadowPso = PsoManager::GetOrCreateShadowMapInstancedPso();
        if (!shadowPso)
        {
            return;
        }

        std::vector<InstanceBatch> batches;
        std::unordered_map<string, size_t> batchLookup;
        for (EntityID entity : World::GetView<AnimationModelComponent, TransformComponent>())
        {
            if (!CanInstance(entity) || !ShouldCastShadow(entity) ||
                !RendererResource::ShouldDrawEntityInCurrentShadowPass(entity))
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
                const string key = to_string(reinterpret_cast<uintptr_t>(model)) + "|" +
                    to_string(meshIndex);
                auto found = batchLookup.find(key);
                if (found == batchLookup.end())
                {
                    InstanceBatch batch{};
                    batch.Kind = InstanceKind::AnimatedMesh;
                    batch.Pso = shadowPso;
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
						if (mesh.LodIndexCounts[lod - 1] != 0)
						{
							batch.AvailableLodCount = lod + 1;
						}
					}
                    batch.AnimatedModel = model;
                    batch.AnimatedMeshIndex = meshIndex;
                    batch.Entities.push_back(entity);
                    batchLookup.emplace(key, batches.size());
                    batches.push_back(std::move(batch));
                }
                else
                {
                    batches[found->second].Entities.push_back(entity);
                }
            }
        }

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
				const string key = "S|" + to_string(reinterpret_cast<uintptr_t>(model)) + "|" + to_string(meshIndex);
				auto found = batchLookup.find(key);
				if (found == batchLookup.end())
				{
					InstanceBatch batch{};
					batch.Kind = InstanceKind::StaticMesh;
					batch.Pso = shadowPso;
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
					batch.Entities.push_back(entity);
					batchLookup.emplace(key, batches.size());
					batches.push_back(std::move(batch));
				}
				else
				{
					batches[found->second].Entities.push_back(entity);
				}
			}
		}

		auto addNonIndexedShadow = [&](EntityID entity, InstanceKind kind,
			const D3D12_VERTEX_BUFFER_VIEW& vertexBuffer, UINT vertexCount,
			uint64_t geometryHash, const XMFLOAT3& center, const XMFLOAT3& extents, bool hasBounds)
			{
				const string key = "N|" + to_string(static_cast<UINT>(kind)) + "|" +
					to_string(geometryHash != 0 ? geometryHash : vertexBuffer.BufferLocation);
				auto found = batchLookup.find(key);
				if (found == batchLookup.end())
				{
					InstanceBatch batch{};
					batch.Kind = kind;
					batch.Pso = shadowPso;
					batch.VertexBuffer = vertexBuffer;
					batch.VertexCount = vertexCount;
					batch.LodDrawCounts = { vertexCount, vertexCount, vertexCount };
					batch.BoundsCenter = center;
					batch.BoundsExtents = extents;
					batch.HasBounds = hasBounds;
					batch.Entities.push_back(entity);
					batchLookup.emplace(key, batches.size());
					batches.push_back(std::move(batch));
				}
				else
				{
					batches[found->second].Entities.push_back(entity);
				}
			};
		for (EntityID entity : World::GetView<SpriteComponent, TransformComponent>())
		{
			const auto& sprite = ComponentManager::GetComponentUnchecked<SpriteComponent>(entity);
			if (CanInstance(entity) && ShouldCastShadow(entity) && sprite.Is3D &&
				sprite.VertexBufferView.BufferLocation != 0 && sprite.VertexCount != 0)
			{
				addNonIndexedShadow(entity, InstanceKind::Sprite3D, sprite.VertexBufferView,
					sprite.VertexCount, sprite.GeometryHash, sprite.LocalBoundsCenter,
					sprite.LocalBoundsExtents, sprite.HasLocalBounds);
			}
		}
		for (EntityID entity : World::GetView<MeshComponent, TransformComponent>())
		{
			if (ComponentManager::HasComponent<AnimationModelComponent>(entity) ||
				ComponentManager::HasComponent<StaticModelComponent>(entity)) continue;
			const auto& mesh = ComponentManager::GetComponentUnchecked<MeshComponent>(entity);
			if (CanInstance(entity) && ShouldCastShadow(entity) &&
				mesh.VertexBufferView.BufferLocation != 0 && mesh.VertexCount != 0)
			{
				addNonIndexedShadow(entity, InstanceKind::Mesh, mesh.VertexBufferView,
					mesh.VertexCount, mesh.GeometryHash, mesh.LocalBoundsCenter,
					mesh.LocalBoundsExtents, mesh.HasLocalBounds);
			}
		}

        if (batches.empty())
        {
            return;
        }

        commandList->SetGraphicsRootSignature(RendererShader::GetModelRootSignature());
        if (RendererResource::GetShadowCB())
        {
            commandList->SetGraphicsRootConstantBufferView(
                5, RendererResource::GetCurrentShadowConstantBufferAddress());
        }
        commandList->SetPipelineState(shadowPso);
        commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        std::unordered_set<AnimationModelResource*> skinnedModels;
        for (auto& batch : batches)
        {
            if (batch.AnimatedModel && skinnedModels.insert(batch.AnimatedModel).second)
            {
                batch.AnimatedModel->DispatchGpuSkinning(commandList);
                commandList->SetGraphicsRootSignature(RendererShader::GetModelRootSignature());
                commandList->SetGraphicsRootConstantBufferView(
                    5, RendererResource::GetCurrentShadowConstantBufferAddress());
                commandList->SetPipelineState(shadowPso);
            }

			const MeshData* animatedMesh = batch.AnimatedModel
				? &batch.AnimatedModel->GetMeshData(batch.AnimatedMeshIndex)
				: nullptr;
			if (animatedMesh)
			{
				const auto ready = CD3DX12_RESOURCE_BARRIER::Transition(
					animatedMesh->VertexBuffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
					D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
				commandList->ResourceBarrier(1, &ready);
			}

			executeGpuCullLod(batch, [&]()
				{
					commandList->SetGraphicsRootSignature(RendererShader::GetModelRootSignature());
					commandList->SetGraphicsRootConstantBufferView(
						5, RendererResource::GetCurrentShadowConstantBufferAddress());
					commandList->SetPipelineState(shadowPso);
				});

			if (animatedMesh)
			{
				const auto back = CD3DX12_RESOURCE_BARRIER::Transition(
					animatedMesh->VertexBuffer.Get(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
					D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
				commandList->ResourceBarrier(1, &back);
			}
        }
        return;
    }

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

    for (EntityID entity : World::GetView<SpriteComponent, TransformComponent>())
    {
        if (!CanInstance(entity)) continue;
        const auto& sprite = ComponentManager::GetComponentUnchecked<SpriteComponent>(entity);
        if (!sprite.Is3D || sprite.VertexBufferView.BufferLocation == 0 || sprite.VertexCount == 0) continue;
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
		batch.LodDrawCounts = { sprite.VertexCount, sprite.VertexCount, sprite.VertexCount };
        batch.TextureIndex = texture;
        batch.NormalIndex = normal;
        batch.Material = material;
        batch.BoundsCenter = sprite.LocalBoundsCenter;
        batch.BoundsExtents = sprite.LocalBoundsExtents;
        batch.HasBounds = sprite.HasLocalBounds;
        add(entity, batch, MakeKey(entity, batch.Kind, pso, batch.VertexBuffer.BufferLocation, sprite.GeometryHash, batch.VertexCount, 0, 0, texture, normal, material));
    }

    for (EntityID entity : World::GetView<MeshComponent, TransformComponent>())
    {
        if (!CanInstance(entity) ||
			ComponentManager::HasComponent<AnimationModelComponent>(entity) ||
			ComponentManager::HasComponent<StaticModelComponent>(entity)) continue;
        const auto& mesh = ComponentManager::GetComponentUnchecked<MeshComponent>(entity);
        if (mesh.VertexBufferView.BufferLocation == 0 || mesh.VertexCount == 0) continue;
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
		batch.LodDrawCounts = { mesh.VertexCount, mesh.VertexCount, mesh.VertexCount };
        batch.TextureIndex = texture;
        batch.NormalIndex = normal;
        batch.Material = material;
        batch.BoundsCenter = mesh.LocalBoundsCenter;
        batch.BoundsExtents = mesh.LocalBoundsExtents;
        batch.HasBounds = mesh.HasLocalBounds;
        add(entity, batch, MakeKey(entity, batch.Kind, pso, batch.VertexBuffer.BufferLocation, mesh.GeometryHash, batch.VertexCount, 0, 0, texture, normal, material));
    }

    for (EntityID entity : World::GetView<AnimationModelComponent, TransformComponent>())
    {
        if (!CanInstance(entity)) continue;
        const auto& animation = ComponentManager::GetComponentUnchecked<AnimationModelComponent>(entity);
        AnimationModelResource* model = ModelManager::GetAnimModel(animation.ModelId);
        if (!model) continue;
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
			batch.LodIndexBuffers[0] = mesh.IndexBufferView;
			batch.LodDrawCounts[0] = mesh.IndexCount;
			batch.AvailableLodCount = 1;
			for (UINT lod = 1; lod < MeshData::LodCount; ++lod)
			{
				batch.LodIndexBuffers[lod] = mesh.GetLodIndexBufferView(lod);
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
            add(entity, batch, MakeKey(entity, batch.Kind, pso, batch.VertexBuffer.BufferLocation, 0, batch.VertexCount,
                batch.IndexBuffer.BufferLocation, batch.IndexCount, texture, normal, material,
				meshIndex, &animation));
        }
    }

	for (EntityID entity : World::GetView<StaticModelComponent, TransformComponent>())
	{
		if (!CanInstance(entity)) continue;
		const auto& component = ComponentManager::GetComponentUnchecked<StaticModelComponent>(entity);
		StaticModelResource* model = ModelManager::GetStaticModel(component.ModelId);
		if (!model) continue;
		const MaterialComponent* material = ComponentManager::HasComponent<MaterialComponent>(entity)
			? &ComponentManager::GetComponentUnchecked<MaterialComponent>(entity) : nullptr;
		if (!acceptsPass(entity, material)) continue;
		const int entityTexture = material && material->UseTexture && material->TextureID >= 0
			? material->TextureID : defaultTexture;
		const int normal = material && material->NormalMapID >= 0
			? material->NormalMapID : defaultTexture;
		for (UINT meshIndex = 0; meshIndex < model->GetMeshCount(); ++meshIndex)
		{
			const StaticMeshData& mesh = model->GetMeshData(meshIndex);
			if (!mesh.VertexBuffer || !mesh.IndexBuffer || mesh.IndexCount == 0) continue;
			const int texture = mesh.TextureIndex >= 0 ? mesh.TextureIndex : entityTexture;
			rendererResource resource{};
			resource.vsPath = "shader/hlsl/build/modelshaderInstancedVS.cso";
			resource.psPath = ResolvePixelShader(entity, InstanceKind::StaticMesh);
			resource.isModel = true;
			resource.enableAlphaBlend = transparentPass;
			ID3D12PipelineState* pso = PsoManager::GetOrCreateGraphicsPso(resource);
			if (!pso) continue;
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
			batch.TextureIndex = texture;
			batch.NormalIndex = normal;
			batch.Material = material;
			batch.BoundsCenter = model->GetAabbCenter();
			batch.BoundsExtents = model->GetAabbExtents();
			batch.HasBounds = true;
			batch.StaticModel = model;
			batch.AnimatedMeshIndex = meshIndex;
			add(entity, batch, MakeKey(
				entity, batch.Kind, pso, batch.VertexBuffer.BufferLocation, 0, mesh.VertexCount,
				batch.IndexBuffer.BufferLocation, batch.IndexCount, texture, normal, material, meshIndex));
		}
	}

    std::unordered_set<AnimationModelResource*> skinned;
    const UINT descriptorIncrement = RendererResource::GetCbvIncrementSize();
    const auto descriptorStart = heap->GetGPUDescriptorHandleForHeapStart();
    RendererDraw::BeginModelPass();
    for (auto& batch : batches)
    {
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

		const EntityID representative = batch.Entities.front();
		ConstantBuffer3D batchConstants{};
		batchConstants.World = XMMatrixIdentity();
		batchConstants.View = XMMatrixTranspose(view);
		batchConstants.Projection = XMMatrixTranspose(projection);
		batchConstants.CameraPos = cameraPosition;
		batchConstants.PreviousWorld = XMMatrixIdentity();
		batchConstants.PreviousViewProjection = XMMatrixIdentity();
		if (batch.Material)
		{
			const MaterialComponent& material = *batch.Material;
			batchConstants.UseTexture = material.UseTexture ? 1 : 0;
			batchConstants.UseNormalMap = material.NormalMapID >= 0 ? 1 : 0;
			batchConstants.MaterialMode = static_cast<int>(material.ShaderClassMode);
			batchConstants.ShaderClass = static_cast<int>(material.ShaderClass);
			batchConstants.MaterialMetallic = material.Metallic;
			batchConstants.MaterialRoughness = material.Roughness;
			batchConstants.MaterialFresnel = material.Fresnel;
			batchConstants.MaterialAlpha = material.Alpha;
			batchConstants.MaterialIsTransparent = material.IsTransparent ? 1 : 0;
			batchConstants.ToonOutlineWidth = material.ToonOutlineWidth;
			batchConstants.ToonOutlineScreenWidth = material.ToonOutlineScreenWidth;
			batchConstants.ViewportSize =
			{
				max(static_cast<float>(RendererCore::GetSceneWidth()), 1.0f),
				max(static_cast<float>(RendererCore::GetSceneHeight()), 1.0f)
			};
			batchConstants.ToonOutlineUseScreenSpace =
				material.ToonOutlineWidthModeSetting == ToonOutlineWidthMode::ScreenPixels ? 1 : 0;
		}
		const D3D12_GPU_DESCRIPTOR_HANDLE batchConstantHandle =
			RendererResource::AllocateTransientConstantBuffer(batchConstants);
		executeGpuCullLod(batch, [&]()
			{
				RendererDraw::BeginModelPass();
				commandList->SetPipelineState(batch.Pso);
				commandList->SetGraphicsRootDescriptorTable(
					0, batchConstantHandle);
				commandList->SetGraphicsRootDescriptorTable(
					1, CD3DX12_GPU_DESCRIPTOR_HANDLE(
						descriptorStart, batch.TextureIndex, descriptorIncrement));
				commandList->SetGraphicsRootDescriptorTable(
					6, CD3DX12_GPU_DESCRIPTOR_HANDLE(
						descriptorStart, batch.NormalIndex, descriptorIncrement));
				RendererResource::SetMaterial(
					representative, batch.Material ? *batch.Material : DefaultMaterial());
			});

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
