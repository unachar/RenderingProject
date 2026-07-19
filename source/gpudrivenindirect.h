#pragma once

#include "pch.h"
#include "renderercore.h"
#include "rendererstate.h"
#include "renderershader.h"
#include "animationmodel.h"
#include "staticmodel.h"
#include "occlusionculling.h"
#include "renderersettings.h"
#include <unordered_map>
#include <vector>







class GpuDrivenIndirectDrawCache
{
private:
    static constexpr UINT g_kMAX_CULL_DISPATCHES_PER_FRAME = 16384;
    struct ShadowCommand
    {
        D3D12_VERTEX_BUFFER_VIEW VertexBuffer{};
        D3D12_INDEX_BUFFER_VIEW IndexBuffer{};
        D3D12_DRAW_INDEXED_ARGUMENTS Draw{};
    };

    struct VelocityCommand
    {
        D3D12_VERTEX_BUFFER_VIEW CurrentVertexBuffer{};
        D3D12_VERTEX_BUFFER_VIEW PreviousVertexBuffer{};
        D3D12_INDEX_BUFFER_VIEW IndexBuffer{};
        D3D12_DRAW_INDEXED_ARGUMENTS Draw{};
    };

    struct alignas(256) CullConstants
    {
        XMFLOAT4X4 World{};
        XMFLOAT4X4 ViewProjection{};
        XMFLOAT4 LocalCenter{};
        XMFLOAT4 LocalExtents{};
        UINT CommandStrideBytes = 0;
        UINT CandidateCount = 0;
        UINT EnableFrustumCulling = 0;
		UINT Phase = 0;
		UINT EnableOcclusion = 0;
		UINT HiZWidth = 0;
		UINT HiZHeight = 0;
		UINT HiZMipCount = 0;
		UINT PreviousHiZValid = 0;
		UINT CurrentHiZValid = 0;
		UINT Padding[14]{};
    };
    static_assert(sizeof(CullConstants) == 256);

    struct IndirectBatch
    {
        ComPtr<ID3D12Resource> CandidateBuffer{};
        ComPtr<ID3D12Resource> VisibleBuffer{};
        ComPtr<ID3D12Resource> CountBuffer{};
        UINT CommandCount = 0;
        UINT SourceMeshCount = 0;
        UINT CommandStride = 0;
		bool Initialized = false;
    };

    struct PrimaryBatch
    {
        struct Group
        {
            int TextureIndex = -1;
            IndirectBatch Commands{};
        };
        std::vector<Group> Groups{};
        UINT SourceMeshCount = 0;
    };

    ComPtr<ID3D12CommandSignature> m_ShadowSignature{};
    ComPtr<ID3D12CommandSignature> m_VelocitySignature{};
    ComPtr<ID3D12RootSignature> m_CullRootSignature{};
    ComPtr<ID3D12PipelineState> m_CullPipelineState{};
    ComPtr<ID3D12Resource> m_ZeroCounterUpload{};
    ComPtr<ID3D12Resource> m_CullConstantsUpload{};
    UINT8* m_MappedCullConstants = nullptr;
    UINT m_CullConstantFrameIndex = UINT_MAX;
    UINT m_CullConstantCursor = 0;
    bool m_InitializationAttempted = false;

    std::unordered_map<const AnimationModelResource*, IndirectBatch> m_AnimatedShadowBatches{};
    std::unordered_map<const StaticModelResource*, IndirectBatch> m_StaticShadowBatches{};
    std::unordered_map<const AnimationModelResource*, IndirectBatch> m_AnimatedVelocityBatches{};
    std::unordered_map<const StaticModelResource*, IndirectBatch> m_StaticVelocityBatches{};
    std::unordered_map<const AnimationModelResource*, PrimaryBatch> m_AnimatedPrimaryBatches{};
    std::unordered_map<const StaticModelResource*, PrimaryBatch> m_StaticPrimaryBatches{};

    static D3D12_DRAW_INDEXED_ARGUMENTS MakeDrawArguments(UINT indexCount)
    {
        D3D12_DRAW_INDEXED_ARGUMENTS draw{};
        draw.IndexCountPerInstance = indexCount;
        draw.InstanceCount = 1;
        return draw;
    }

    static bool CreateUploadBuffer(const void* data, UINT64 sizeInBytes, ComPtr<ID3D12Resource>& output)
    {
        ID3D12Device* device = RendererCore::GetDevice();
        if (!device || sizeInBytes == 0)
        {
            return false;
        }

        const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        const auto description = CD3DX12_RESOURCE_DESC::Buffer(sizeInBytes);
        HRESULT result = device->CreateCommittedResource(
            &heap,
            D3D12_HEAP_FLAG_NONE,
            &description,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&output));
        if (FAILED(result))
        {
            return false;
        }

        if (data)
        {
            void* mapped = nullptr;
            const D3D12_RANGE noRead{ 0, 0 };
            result = output->Map(0, &noRead, &mapped);
            if (FAILED(result) || !mapped)
            {
                output.Reset();
                return false;
            }
            memcpy(mapped, data, static_cast<size_t>(sizeInBytes));
            output->Unmap(0, nullptr);
        }
        return true;
    }

    static bool CreateUavBuffer(UINT64 sizeInBytes, ComPtr<ID3D12Resource>& output)
    {
        ID3D12Device* device = RendererCore::GetDevice();
        if (!device || sizeInBytes == 0)
        {
            return false;
        }

        const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        const auto description = CD3DX12_RESOURCE_DESC::Buffer(
            sizeInBytes,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        return SUCCEEDED(device->CreateCommittedResource(
            &heap,
            D3D12_HEAP_FLAG_NONE,
            &description,
			D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(&output)));
    }

    bool CreateCullPipeline()
    {
        ID3D12Device* device = RendererCore::GetDevice();
        if (!device)
        {
            return false;
        }

		CD3DX12_DESCRIPTOR_RANGE hiZRanges[2]{};
		hiZRanges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);
		hiZRanges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2);
		CD3DX12_ROOT_PARAMETER parameters[6]{};
        parameters[0].InitAsShaderResourceView(0, 0);
        parameters[1].InitAsUnorderedAccessView(0, 0);
        parameters[2].InitAsUnorderedAccessView(1, 0);
		parameters[3].InitAsConstantBufferView(0, 0);
		parameters[4].InitAsDescriptorTable(1, &hiZRanges[0]);
		parameters[5].InitAsDescriptorTable(1, &hiZRanges[1]);
		CD3DX12_STATIC_SAMPLER_DESC hiZSampler(
			0,
			D3D12_FILTER_MIN_MAG_MIP_POINT,
			D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
			D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
			D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

        CD3DX12_ROOT_SIGNATURE_DESC rootDescription{};
		rootDescription.Init(_countof(parameters), parameters, 1, &hiZSampler, D3D12_ROOT_SIGNATURE_FLAG_NONE);

        ComPtr<ID3DBlob> serialized;
        ComPtr<ID3DBlob> errors;
        HRESULT result = D3D12SerializeRootSignature(
            &rootDescription,
            D3D_ROOT_SIGNATURE_VERSION_1,
            &serialized,
            &errors);
        if (FAILED(result))
        {
            if (errors)
            {
                Debug::Log("%s\n", static_cast<const char*>(errors->GetBufferPointer()));
            }
            return false;
        }

        result = device->CreateRootSignature(
            0,
            serialized->GetBufferPointer(),
            serialized->GetBufferSize(),
            IID_PPV_ARGS(&m_CullRootSignature));
        if (FAILED(result))
        {
            return false;
        }

        ComPtr<ID3DBlob> computeShader;
        rendererResource shaderResource{};
        shaderResource.csoPath = "shader/hlsl/build/GpuDrivenCullCS.cso";
        shaderResource.ppBlob = computeShader.GetAddressOf();
        if (!RendererShader::LoadShaderBlob(shaderResource))
        {
            Debug::Log("GPU culling shader load failed: %s\n", shaderResource.csoPath);
            return false;
        }

        D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDescription{};
        pipelineDescription.pRootSignature = m_CullRootSignature.Get();
        pipelineDescription.CS = {
            computeShader->GetBufferPointer(),
            computeShader->GetBufferSize()
        };
        if (FAILED(device->CreateComputePipelineState(
            &pipelineDescription,
            IID_PPV_ARGS(&m_CullPipelineState))))
        {
            return false;
        }

        const UINT zero = 0;
        if (!CreateUploadBuffer(&zero, sizeof(zero), m_ZeroCounterUpload))
        {
            return false;
        }

        const UINT64 constantsSize =
            static_cast<UINT64>(RendererState::g_kFRAME_COUNT) *
            g_kMAX_CULL_DISPATCHES_PER_FRAME * sizeof(CullConstants);
        if (!CreateUploadBuffer(nullptr, constantsSize, m_CullConstantsUpload))
        {
            return false;
        }
        const D3D12_RANGE noRead{ 0, 0 };
        return SUCCEEDED(m_CullConstantsUpload->Map(
            0,
            &noRead,
            reinterpret_cast<void**>(&m_MappedCullConstants)));
    }

    bool EnsureInitialized()
    {
        if (m_ShadowSignature && m_VelocitySignature && m_CullPipelineState)
        {
            return true;
        }
        if (m_InitializationAttempted)
        {
            return false;
        }
        m_InitializationAttempted = true;

        ID3D12Device* device = RendererCore::GetDevice();
        if (!device || !CreateCullPipeline())
        {
            return false;
        }

        D3D12_INDIRECT_ARGUMENT_DESC shadowArguments[3]{};
        shadowArguments[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW;
        shadowArguments[0].VertexBuffer.Slot = 0;
        shadowArguments[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW;
        shadowArguments[2].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
        D3D12_COMMAND_SIGNATURE_DESC shadowDescription{};
        shadowDescription.ByteStride = sizeof(ShadowCommand);
        shadowDescription.NumArgumentDescs = _countof(shadowArguments);
        shadowDescription.pArgumentDescs = shadowArguments;
        if (FAILED(device->CreateCommandSignature(
            &shadowDescription,
            nullptr,
            IID_PPV_ARGS(&m_ShadowSignature))))
        {
            return false;
        }

        D3D12_INDIRECT_ARGUMENT_DESC velocityArguments[4]{};
        velocityArguments[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW;
        velocityArguments[0].VertexBuffer.Slot = 0;
        velocityArguments[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW;
        velocityArguments[1].VertexBuffer.Slot = 1;
        velocityArguments[2].Type = D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW;
        velocityArguments[3].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
        D3D12_COMMAND_SIGNATURE_DESC velocityDescription{};
        velocityDescription.ByteStride = sizeof(VelocityCommand);
        velocityDescription.NumArgumentDescs = _countof(velocityArguments);
        velocityDescription.pArgumentDescs = velocityArguments;
        return SUCCEEDED(device->CreateCommandSignature(
            &velocityDescription,
            nullptr,
            IID_PPV_ARGS(&m_VelocitySignature)));
    }

    template<typename Command>
    static bool FinalizeBatch(const std::vector<Command>& commands, UINT sourceMeshCount, IndirectBatch& batch)
    {
        if (commands.empty())
        {
            return false;
        }

        const UINT64 byteSize = static_cast<UINT64>(commands.size()) * sizeof(Command);
        batch = {};
        batch.CommandCount = static_cast<UINT>(commands.size());
        batch.SourceMeshCount = sourceMeshCount;
        batch.CommandStride = sizeof(Command);
        return CreateUploadBuffer(commands.data(), byteSize, batch.CandidateBuffer) &&
            CreateUavBuffer(byteSize, batch.VisibleBuffer) &&
            CreateUavBuffer(sizeof(UINT), batch.CountBuffer);
    }

    static bool BuildAnimatedShadowBatch(const AnimationModelResource* model, IndirectBatch& batch)
    {
        std::vector<ShadowCommand> commands;
        commands.reserve(model->GetMeshCount());
        for (UINT index = 0; index < model->GetMeshCount(); ++index)
        {
            const MeshData& mesh = model->GetMeshData(index);
            if (mesh.VertexBuffer && mesh.IndexBuffer && mesh.IndexCount > 0)
            {
                commands.push_back({ mesh.VertexBufferView, mesh.IndexBufferView, MakeDrawArguments(mesh.IndexCount) });
            }
        }
        return FinalizeBatch(commands, model->GetMeshCount(), batch);
    }

    static bool BuildStaticShadowBatch(const StaticModelResource* model, IndirectBatch& batch)
    {
        std::vector<ShadowCommand> commands;
        commands.reserve(model->GetMeshCount());
        for (UINT index = 0; index < model->GetMeshCount(); ++index)
        {
            const StaticMeshData& mesh = model->GetMeshData(index);
            if (mesh.VertexBuffer && mesh.IndexBuffer && mesh.IndexCount > 0)
            {
                commands.push_back({ mesh.VertexBufferView, mesh.IndexBufferView, MakeDrawArguments(mesh.IndexCount) });
            }
        }
        return FinalizeBatch(commands, model->GetMeshCount(), batch);
    }

    static bool BuildAnimatedVelocityBatch(const AnimationModelResource* model, IndirectBatch& batch)
    {
        std::vector<VelocityCommand> commands;
        commands.reserve(model->GetMeshCount());
        for (UINT index = 0; index < model->GetMeshCount(); ++index)
        {
            const MeshData& mesh = model->GetMeshData(index);
            if (mesh.VertexBuffer && mesh.PreviousVertexBuffer && mesh.IndexBuffer &&
                mesh.PreviousVertexValid && mesh.IndexCount > 0)
            {
                commands.push_back({
                    mesh.VertexBufferView,
                    mesh.PreviousVertexBufferView,
                    mesh.IndexBufferView,
                    MakeDrawArguments(mesh.IndexCount)
                    });
            }
        }
        return FinalizeBatch(commands, model->GetMeshCount(), batch);
    }

    static bool BuildStaticVelocityBatch(const StaticModelResource* model, IndirectBatch& batch)
    {
        std::vector<VelocityCommand> commands;
        commands.reserve(model->GetMeshCount());
        for (UINT index = 0; index < model->GetMeshCount(); ++index)
        {
            const StaticMeshData& mesh = model->GetMeshData(index);
            if (mesh.VertexBuffer && mesh.IndexBuffer && mesh.IndexCount > 0)
            {
                commands.push_back({
                    mesh.VertexBufferView,
                    mesh.VertexBufferView,
                    mesh.IndexBufferView,
                    MakeDrawArguments(mesh.IndexCount)
                    });
            }
        }
        return FinalizeBatch(commands, model->GetMeshCount(), batch);
    }

    template<typename Model, typename Mesh>
    static bool BuildPrimaryBatch(const Model* model, PrimaryBatch& batch)
    {
        batch = {};
        batch.SourceMeshCount = model->GetMeshCount();
        std::vector<std::pair<int, std::vector<ShadowCommand>>> groupedCommands;
        for (UINT index = 0; index < model->GetMeshCount(); ++index)
        {
            const Mesh& mesh = model->GetMeshData(index);
            if (!mesh.VertexBuffer || !mesh.IndexBuffer || mesh.IndexCount == 0)
            {
                continue;
            }

            auto group = std::find_if(
                groupedCommands.begin(),
                groupedCommands.end(),
                [&](const auto& candidate) { return candidate.first == mesh.TextureIndex; });
            if (group == groupedCommands.end())
            {
                groupedCommands.emplace_back(mesh.TextureIndex, std::vector<ShadowCommand>{});
                group = std::prev(groupedCommands.end());
            }
            group->second.push_back({
                mesh.VertexBufferView,
                mesh.IndexBufferView,
                MakeDrawArguments(mesh.IndexCount)
                });
        }

        for (auto& [textureIndex, commands] : groupedCommands)
        {
            PrimaryBatch::Group group{};
            group.TextureIndex = textureIndex;
            if (!FinalizeBatch(commands, model->GetMeshCount(), group.Commands))
            {
                return false;
            }
            batch.Groups.push_back(std::move(group));
        }
        return !batch.Groups.empty();
    }

    template<typename Model, typename Map, typename Mesh>
    static PrimaryBatch* FindOrBuildPrimary(const Model* model, Map& map)
    {
        PrimaryBatch& batch = map[model];
        if (batch.Groups.empty() || batch.SourceMeshCount != model->GetMeshCount())
        {
            if (!BuildPrimaryBatch<Model, Mesh>(model, batch))
            {
                return nullptr;
            }
        }
        return &batch;
    }

    template<typename Model, typename Map, typename Builder>
    static IndirectBatch* FindOrBuild(const Model* model, Map& map, Builder&& builder)
    {
        IndirectBatch& batch = map[model];
        if (!batch.CandidateBuffer || batch.SourceMeshCount != model->GetMeshCount())
        {
            if (!builder(model, batch))
            {
                return nullptr;
            }
        }
        return &batch;
    }

    bool CullAndExecute(
        ID3D12GraphicsCommandList* commandList,
        ID3D12CommandSignature* signature,
        IndirectBatch& batch,
        EntityID entity,
        const XMMATRIX& world,
        const XMMATRIX& viewProjection,
        const XMFLOAT3& localCenter,
        const XMFLOAT3& localExtents,
        bool enableFrustumCulling,
		ID3D12PipelineState* graphicsPipelineState,
		bool enableOcclusion = false)
    {
        if (!commandList || !signature || !graphicsPipelineState ||
            entity >= g_kMAX_ENTITIES || !m_MappedCullConstants)
        {
            return false;
        }

        D3D12_RESOURCE_BARRIER countToCopy = CD3DX12_RESOURCE_BARRIER::Transition(
            batch.CountBuffer.Get(),
			batch.Initialized ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_COPY_DEST);
        commandList->ResourceBarrier(1, &countToCopy);
        commandList->CopyBufferRegion(batch.CountBuffer.Get(), 0, m_ZeroCounterUpload.Get(), 0, sizeof(UINT));
        D3D12_RESOURCE_BARRIER countToUav = CD3DX12_RESOURCE_BARRIER::Transition(
            batch.CountBuffer.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        commandList->ResourceBarrier(1, &countToUav);

        const UINT frameIndex = RendererCore::GetFrameIndex() % RendererState::g_kFRAME_COUNT;
        if (m_CullConstantFrameIndex != frameIndex)
        {
            m_CullConstantFrameIndex = frameIndex;
            m_CullConstantCursor = 0;
        }
        if (m_CullConstantCursor >= g_kMAX_CULL_DISPATCHES_PER_FRAME)
        {
            Debug::Log("GPU culling constant ring exhausted for frame %u\n", frameIndex);
            return false;
        }
        const UINT64 constantIndex =
            static_cast<UINT64>(frameIndex) * g_kMAX_CULL_DISPATCHES_PER_FRAME +
            m_CullConstantCursor++;
        CullConstants constants{};
        XMStoreFloat4x4(&constants.World, world);
        XMStoreFloat4x4(&constants.ViewProjection, viewProjection);
        constants.LocalCenter = XMFLOAT4(localCenter.x, localCenter.y, localCenter.z, 1.0f);
        constants.LocalExtents = XMFLOAT4(localExtents.x, localExtents.y, localExtents.z, 0.0f);
        constants.CommandStrideBytes = batch.CommandStride;
        constants.CandidateCount = batch.CommandCount;
		constants.EnableFrustumCulling = enableFrustumCulling ? 1u : 0u;
		constants.Phase = OcclusionCulling::GetPhase();
		constants.EnableOcclusion = enableOcclusion && RendererSettings::GetTwoPhaseOcclusionEnabled() ? 1u : 0u;
		constants.HiZWidth = OcclusionCulling::GetWidth();
		constants.HiZHeight = OcclusionCulling::GetHeight();
		constants.HiZMipCount = OcclusionCulling::GetMipCount();
		constants.PreviousHiZValid = OcclusionCulling::HasPrevious() ? 1u : 0u;
		constants.CurrentHiZValid = OcclusionCulling::HasCurrent() ? 1u : 0u;
        memcpy(
            m_MappedCullConstants + constantIndex * sizeof(CullConstants),
            &constants,
            sizeof(constants));

        commandList->SetComputeRootSignature(m_CullRootSignature.Get());
        commandList->SetPipelineState(m_CullPipelineState.Get());
        commandList->SetComputeRootShaderResourceView(0, batch.CandidateBuffer->GetGPUVirtualAddress());
        commandList->SetComputeRootUnorderedAccessView(1, batch.VisibleBuffer->GetGPUVirtualAddress());
        commandList->SetComputeRootUnorderedAccessView(2, batch.CountBuffer->GetGPUVirtualAddress());
		commandList->SetComputeRootConstantBufferView(
            3,
			m_CullConstantsUpload->GetGPUVirtualAddress() + constantIndex * sizeof(CullConstants));
		commandList->SetComputeRootDescriptorTable(4, OcclusionCulling::GetPreviousSrv());
		commandList->SetComputeRootDescriptorTable(5, OcclusionCulling::GetCurrentSrv());
        commandList->Dispatch((batch.CommandCount + 63) / 64, 1, 1);

        D3D12_RESOURCE_BARRIER uavBarriers[2] =
        {
            CD3DX12_RESOURCE_BARRIER::UAV(batch.VisibleBuffer.Get()),
            CD3DX12_RESOURCE_BARRIER::UAV(batch.CountBuffer.Get())
        };
        commandList->ResourceBarrier(_countof(uavBarriers), uavBarriers);

        D3D12_RESOURCE_BARRIER toIndirect[2] =
        {
            CD3DX12_RESOURCE_BARRIER::Transition(
                batch.VisibleBuffer.Get(),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT),
            CD3DX12_RESOURCE_BARRIER::Transition(
                batch.CountBuffer.Get(),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT)
        };
        commandList->ResourceBarrier(_countof(toIndirect), toIndirect);

        commandList->SetPipelineState(graphicsPipelineState);
        commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commandList->ExecuteIndirect(
            signature,
            batch.CommandCount,
            batch.VisibleBuffer.Get(),
            0,
            batch.CountBuffer.Get(),
            0);

        D3D12_RESOURCE_BARRIER toUav[2] =
        {
            CD3DX12_RESOURCE_BARRIER::Transition(
                batch.VisibleBuffer.Get(),
                D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            CD3DX12_RESOURCE_BARRIER::Transition(
                batch.CountBuffer.Get(),
                D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        };
        commandList->ResourceBarrier(_countof(toUav), toUav);
		batch.Initialized = true;
        return true;
    }

    static bool ExecuteCandidatesDirect(
        ID3D12GraphicsCommandList* commandList,
        ID3D12CommandSignature* signature,
        const IndirectBatch& batch,
        ID3D12PipelineState* graphicsPipelineState)
    {
        if (!commandList || !signature || !batch.CandidateBuffer ||
            batch.CommandCount == 0 || !graphicsPipelineState)
        {
            return false;
        }

        commandList->SetPipelineState(graphicsPipelineState);
        commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commandList->ExecuteIndirect(
            signature,
            batch.CommandCount,
            batch.CandidateBuffer.Get(),
            0,
            nullptr,
            0);
        return true;
    }

public:
    template<typename Model, typename Mesh, typename Map>
    bool ExecutePrimaryImpl(
        ID3D12GraphicsCommandList* commandList,
        const Model* model,
        Map& map,
        EntityID entity,
        const XMMATRIX& world,
        const XMMATRIX& viewProjection,
        int fallbackTextureIndex,
        D3D12_GPU_DESCRIPTOR_HANDLE descriptorHeapStart,
        UINT descriptorIncrement,
        ID3D12PipelineState* graphicsPipelineState)
    {
        if (!EnsureInitialized() || !model)
        {
            return false;
        }
        PrimaryBatch* batch = FindOrBuildPrimary<Model, Map, Mesh>(model, map);
        if (!batch)
        {
            return false;
        }

        bool submitted = false;
        for (auto& group : batch->Groups)
        {
            const int textureIndex = group.TextureIndex >= 0
                ? group.TextureIndex
                : fallbackTextureIndex;
            commandList->SetGraphicsRootDescriptorTable(
                1,
                CD3DX12_GPU_DESCRIPTOR_HANDLE(
                    descriptorHeapStart,
                    textureIndex,
                    descriptorIncrement));
            submitted |= CullAndExecute(
                commandList,
                m_ShadowSignature.Get(),
                group.Commands,
                entity,
                world,
                viewProjection,
                model->GetAabbCenter(),
                model->GetAabbExtents(),
                true,
				graphicsPipelineState,
				true);
        }
        return submitted;
    }

    bool ExecutePrimary(
        ID3D12GraphicsCommandList* commandList,
        const AnimationModelResource* model,
        EntityID entity,
        const XMMATRIX& world,
        const XMMATRIX& viewProjection,
        int fallbackTextureIndex,
        D3D12_GPU_DESCRIPTOR_HANDLE descriptorHeapStart,
        UINT descriptorIncrement,
        ID3D12PipelineState* graphicsPipelineState)
    {
        return ExecutePrimaryImpl<AnimationModelResource, MeshData>(
            commandList, model, m_AnimatedPrimaryBatches, entity, world,
            viewProjection, fallbackTextureIndex, descriptorHeapStart,
            descriptorIncrement, graphicsPipelineState);
    }

    bool ExecutePrimary(
        ID3D12GraphicsCommandList* commandList,
        const StaticModelResource* model,
        EntityID entity,
        const XMMATRIX& world,
        const XMMATRIX& viewProjection,
        int fallbackTextureIndex,
        D3D12_GPU_DESCRIPTOR_HANDLE descriptorHeapStart,
        UINT descriptorIncrement,
        ID3D12PipelineState* graphicsPipelineState)
    {
        return ExecutePrimaryImpl<StaticModelResource, StaticMeshData>(
            commandList, model, m_StaticPrimaryBatches, entity, world,
            viewProjection, fallbackTextureIndex, descriptorHeapStart,
            descriptorIncrement, graphicsPipelineState);
    }

    bool ExecuteShadow(
        ID3D12GraphicsCommandList* commandList,
        const AnimationModelResource* model,
        EntityID entity,
        const XMMATRIX& world,
        const XMMATRIX& lightViewProjection,
        ID3D12PipelineState* graphicsPipelineState,
        bool cpuCulledVirtualPage)
    {
        if (!EnsureInitialized() || !model)
        {
            return false;
        }
        IndirectBatch* batch = FindOrBuild(model, m_AnimatedShadowBatches, BuildAnimatedShadowBatch);
        if (!batch)
        {
            return false;
        }
        if (cpuCulledVirtualPage)
        {
            return ExecuteCandidatesDirect(
                commandList, m_ShadowSignature.Get(), *batch, graphicsPipelineState);
        }
        return CullAndExecute(
            commandList,
            m_ShadowSignature.Get(),
            *batch,
            entity,
            world,
            lightViewProjection,
            model->GetAabbCenter(),
            model->GetAabbExtents(),
            true,
            graphicsPipelineState);
    }

    bool ExecuteShadow(
        ID3D12GraphicsCommandList* commandList,
        const StaticModelResource* model,
        EntityID entity,
        const XMMATRIX& world,
        const XMMATRIX& lightViewProjection,
        ID3D12PipelineState* graphicsPipelineState,
        bool cpuCulledVirtualPage)
    {
        if (!EnsureInitialized() || !model)
        {
            return false;
        }
        IndirectBatch* batch = FindOrBuild(model, m_StaticShadowBatches, BuildStaticShadowBatch);
        if (!batch)
        {
            return false;
        }
        if (cpuCulledVirtualPage)
        {
            return ExecuteCandidatesDirect(
                commandList, m_ShadowSignature.Get(), *batch, graphicsPipelineState);
        }
        return CullAndExecute(
            commandList,
            m_ShadowSignature.Get(),
            *batch,
            entity,
            world,
            lightViewProjection,
            model->GetAabbCenter(),
            model->GetAabbExtents(),
            true,
            graphicsPipelineState);
    }

    bool ExecuteVelocity(
        ID3D12GraphicsCommandList* commandList,
        const AnimationModelResource* model,
        EntityID entity,
        const XMMATRIX& world,
        const XMMATRIX& viewProjection,
        ID3D12PipelineState* graphicsPipelineState)
    {
        if (!EnsureInitialized() || !model)
        {
            return false;
        }
        IndirectBatch* batch = FindOrBuild(model, m_AnimatedVelocityBatches, BuildAnimatedVelocityBatch);
        return batch && CullAndExecute(
            commandList,
            m_VelocitySignature.Get(),
            *batch,
            entity,
            world,
            viewProjection,
            model->GetAabbCenter(),
            model->GetAabbExtents(),
            true,
            graphicsPipelineState);
    }

    bool ExecuteVelocity(
        ID3D12GraphicsCommandList* commandList,
        const StaticModelResource* model,
        EntityID entity,
        const XMMATRIX& world,
        const XMMATRIX& viewProjection,
        ID3D12PipelineState* graphicsPipelineState)
    {
        if (!EnsureInitialized() || !model)
        {
            return false;
        }
        IndirectBatch* batch = FindOrBuild(model, m_StaticVelocityBatches, BuildStaticVelocityBatch);
        return batch && CullAndExecute(
            commandList,
            m_VelocitySignature.Get(),
            *batch,
            entity,
            world,
            viewProjection,
            model->GetAabbCenter(),
            model->GetAabbExtents(),
            true,
            graphicsPipelineState);
    }

    void Reset()
    {
        if (m_CullConstantsUpload && m_MappedCullConstants)
        {
            m_CullConstantsUpload->Unmap(0, nullptr);
        }
        m_MappedCullConstants = nullptr;
        m_CullConstantFrameIndex = UINT_MAX;
        m_CullConstantCursor = 0;
        m_AnimatedShadowBatches.clear();
        m_StaticShadowBatches.clear();
        m_AnimatedVelocityBatches.clear();
        m_StaticVelocityBatches.clear();
        m_AnimatedPrimaryBatches.clear();
        m_StaticPrimaryBatches.clear();
        m_ShadowSignature.Reset();
        m_VelocitySignature.Reset();
        m_CullRootSignature.Reset();
        m_CullPipelineState.Reset();
        m_ZeroCounterUpload.Reset();
        m_CullConstantsUpload.Reset();
        m_InitializationAttempted = false;
    }
};
