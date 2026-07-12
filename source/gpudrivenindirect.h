#pragma once

#include "pch.h"
#include "renderercore.h"
#include "animationmodel.h"
#include "staticmodel.h"
#include <unordered_map>
#include <vector>

// Stage-one GPU-driven rendering for the existing renderer.
//
// The current model root signature uses descriptor tables for per-entity data,
// so one ExecuteIndirect call is emitted per entity after its constants are
// bound. Inside that call, all mesh VBV/IBV/draw commands are consumed by the
// GPU from an indirect argument buffer. A later compute-culling stage can write
// the same command layout and an optional count buffer without changing callers.
class GpuDrivenIndirectDrawCache
{
private:
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

    struct IndirectBatch
    {
        ComPtr<ID3D12Resource> ArgumentBuffer{};
        UINT CommandCount = 0;
        UINT SourceMeshCount = 0;
    };

    ComPtr<ID3D12CommandSignature> m_ShadowSignature{};
    ComPtr<ID3D12CommandSignature> m_VelocitySignature{};
    bool m_InitializationAttempted = false;

    std::unordered_map<const AnimationModelResource*, IndirectBatch> m_AnimatedShadowBatches{};
    std::unordered_map<const StaticModelResource*, IndirectBatch> m_StaticShadowBatches{};
    std::unordered_map<const AnimationModelResource*, IndirectBatch> m_AnimatedVelocityBatches{};
    std::unordered_map<const StaticModelResource*, IndirectBatch> m_StaticVelocityBatches{};

    static D3D12_DRAW_INDEXED_ARGUMENTS MakeDrawArguments(UINT indexCount)
    {
        D3D12_DRAW_INDEXED_ARGUMENTS draw{};
        draw.IndexCountPerInstance = indexCount;
        draw.InstanceCount = 1;
        draw.StartIndexLocation = 0;
        draw.BaseVertexLocation = 0;
        draw.StartInstanceLocation = 0;
        return draw;
    }

    static bool CreateUploadArgumentBuffer(
        const void* data,
        size_t sizeInBytes,
        ComPtr<ID3D12Resource>& output)
    {
        ID3D12Device* device = RendererCore::GetDevice();
        if (!device || !data || sizeInBytes == 0)
        {
            return false;
        }

        const CD3DX12_HEAP_PROPERTIES heapProperties(D3D12_HEAP_TYPE_UPLOAD);
        const CD3DX12_RESOURCE_DESC bufferDescription =
            CD3DX12_RESOURCE_DESC::Buffer(static_cast<UINT64>(sizeInBytes));

        HRESULT result = device->CreateCommittedResource(
            &heapProperties,
            D3D12_HEAP_FLAG_NONE,
            &bufferDescription,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&output));
        if (FAILED(result))
        {
            Debug::Log("ERROR: Failed to create ExecuteIndirect argument buffer (hr=0x%08X).\n",
                static_cast<unsigned int>(result));
            return false;
        }

        void* mappedData = nullptr;
        const D3D12_RANGE readRange{ 0, 0 };
        result = output->Map(0, &readRange, &mappedData);
        if (FAILED(result) || !mappedData)
        {
            Debug::Log("ERROR: Failed to map ExecuteIndirect argument buffer (hr=0x%08X).\n",
                static_cast<unsigned int>(result));
            output.Reset();
            return false;
        }

        memcpy(mappedData, data, sizeInBytes);
        output->Unmap(0, nullptr);
        return true;
    }

    bool EnsureInitialized()
    {
        if (m_ShadowSignature && m_VelocitySignature)
        {
            return true;
        }
        if (m_InitializationAttempted)
        {
            return false;
        }
        m_InitializationAttempted = true;

        ID3D12Device* device = RendererCore::GetDevice();
        if (!device)
        {
            return false;
        }

        D3D12_INDIRECT_ARGUMENT_DESC shadowArguments[3]{};
        shadowArguments[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW;
        shadowArguments[0].VertexBuffer.Slot = 0;
        shadowArguments[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW;
        shadowArguments[2].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;

        D3D12_COMMAND_SIGNATURE_DESC shadowSignatureDescription{};
        shadowSignatureDescription.ByteStride = sizeof(ShadowCommand);
        shadowSignatureDescription.NumArgumentDescs = _countof(shadowArguments);
        shadowSignatureDescription.pArgumentDescs = shadowArguments;

        HRESULT result = device->CreateCommandSignature(
            &shadowSignatureDescription,
            nullptr,
            IID_PPV_ARGS(&m_ShadowSignature));
        if (FAILED(result))
        {
            Debug::Log("ERROR: Failed to create shadow ExecuteIndirect signature (hr=0x%08X).\n",
                static_cast<unsigned int>(result));
            return false;
        }

        D3D12_INDIRECT_ARGUMENT_DESC velocityArguments[4]{};
        velocityArguments[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW;
        velocityArguments[0].VertexBuffer.Slot = 0;
        velocityArguments[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW;
        velocityArguments[1].VertexBuffer.Slot = 1;
        velocityArguments[2].Type = D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW;
        velocityArguments[3].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;

        D3D12_COMMAND_SIGNATURE_DESC velocitySignatureDescription{};
        velocitySignatureDescription.ByteStride = sizeof(VelocityCommand);
        velocitySignatureDescription.NumArgumentDescs = _countof(velocityArguments);
        velocitySignatureDescription.pArgumentDescs = velocityArguments;

        result = device->CreateCommandSignature(
            &velocitySignatureDescription,
            nullptr,
            IID_PPV_ARGS(&m_VelocitySignature));
        if (FAILED(result))
        {
            Debug::Log("ERROR: Failed to create velocity ExecuteIndirect signature (hr=0x%08X).\n",
                static_cast<unsigned int>(result));
            m_ShadowSignature.Reset();
            return false;
        }

        return true;
    }

    static bool BuildAnimatedShadowBatch(
        const AnimationModelResource* model,
        IndirectBatch& batch)
    {
        std::vector<ShadowCommand> commands;
        commands.reserve(model->GetMeshCount());
        for (UINT meshIndex = 0; meshIndex < model->GetMeshCount(); ++meshIndex)
        {
            const MeshData& mesh = model->GetMeshData(meshIndex);
            if (!mesh.VertexBuffer || !mesh.IndexBuffer || mesh.IndexCount == 0)
            {
                continue;
            }
            commands.push_back({
                mesh.VertexBufferView,
                mesh.IndexBufferView,
                MakeDrawArguments(mesh.IndexCount)
                });
        }

        batch.ArgumentBuffer.Reset();
        batch.CommandCount = static_cast<UINT>(commands.size());
        batch.SourceMeshCount = model->GetMeshCount();
        return !commands.empty() && CreateUploadArgumentBuffer(
            commands.data(),
            commands.size() * sizeof(ShadowCommand),
            batch.ArgumentBuffer);
    }

    static bool BuildStaticShadowBatch(
        const StaticModelResource* model,
        IndirectBatch& batch)
    {
        std::vector<ShadowCommand> commands;
        commands.reserve(model->GetMeshCount());
        for (UINT meshIndex = 0; meshIndex < model->GetMeshCount(); ++meshIndex)
        {
            const StaticMeshData& mesh = model->GetMeshData(meshIndex);
            if (!mesh.VertexBuffer || !mesh.IndexBuffer || mesh.IndexCount == 0)
            {
                continue;
            }
            commands.push_back({
                mesh.VertexBufferView,
                mesh.IndexBufferView,
                MakeDrawArguments(mesh.IndexCount)
                });
        }

        batch.ArgumentBuffer.Reset();
        batch.CommandCount = static_cast<UINT>(commands.size());
        batch.SourceMeshCount = model->GetMeshCount();
        return !commands.empty() && CreateUploadArgumentBuffer(
            commands.data(),
            commands.size() * sizeof(ShadowCommand),
            batch.ArgumentBuffer);
    }

    static bool BuildAnimatedVelocityBatch(
        const AnimationModelResource* model,
        IndirectBatch& batch)
    {
        std::vector<VelocityCommand> commands;
        commands.reserve(model->GetMeshCount());
        for (UINT meshIndex = 0; meshIndex < model->GetMeshCount(); ++meshIndex)
        {
            const MeshData& mesh = model->GetMeshData(meshIndex);
            if (!mesh.VertexBuffer || !mesh.IndexBuffer || !mesh.PreviousVertexValid || mesh.IndexCount == 0)
            {
                continue;
            }
            commands.push_back({
                mesh.VertexBufferView,
                mesh.PreviousVertexBufferView,
                mesh.IndexBufferView,
                MakeDrawArguments(mesh.IndexCount)
                });
        }

        batch.ArgumentBuffer.Reset();
        batch.CommandCount = static_cast<UINT>(commands.size());
        batch.SourceMeshCount = model->GetMeshCount();
        return !commands.empty() && CreateUploadArgumentBuffer(
            commands.data(),
            commands.size() * sizeof(VelocityCommand),
            batch.ArgumentBuffer);
    }

    static bool BuildStaticVelocityBatch(
        const StaticModelResource* model,
        IndirectBatch& batch)
    {
        std::vector<VelocityCommand> commands;
        commands.reserve(model->GetMeshCount());
        for (UINT meshIndex = 0; meshIndex < model->GetMeshCount(); ++meshIndex)
        {
            const StaticMeshData& mesh = model->GetMeshData(meshIndex);
            if (!mesh.VertexBuffer || !mesh.IndexBuffer || mesh.IndexCount == 0)
            {
                continue;
            }
            commands.push_back({
                mesh.VertexBufferView,
                mesh.VertexBufferView,
                mesh.IndexBufferView,
                MakeDrawArguments(mesh.IndexCount)
                });
        }

        batch.ArgumentBuffer.Reset();
        batch.CommandCount = static_cast<UINT>(commands.size());
        batch.SourceMeshCount = model->GetMeshCount();
        return !commands.empty() && CreateUploadArgumentBuffer(
            commands.data(),
            commands.size() * sizeof(VelocityCommand),
            batch.ArgumentBuffer);
    }

    template<typename ModelType, typename MapType, typename Builder>
    static IndirectBatch* FindOrBuildBatch(
        const ModelType* model,
        MapType& batches,
        Builder&& builder)
    {
        IndirectBatch& batch = batches[model];
        if (!batch.ArgumentBuffer || batch.SourceMeshCount != model->GetMeshCount())
        {
            if (!builder(model, batch))
            {
                return nullptr;
            }
        }
        return &batch;
    }

public:
    bool ExecuteShadow(
        ID3D12GraphicsCommandList* commandList,
        const AnimationModelResource* model)
    {
        if (!commandList || !model || !EnsureInitialized())
        {
            return false;
        }
        IndirectBatch* batch = FindOrBuildBatch(
            model,
            m_AnimatedShadowBatches,
            BuildAnimatedShadowBatch);
        if (!batch)
        {
            return false;
        }
        commandList->ExecuteIndirect(
            m_ShadowSignature.Get(),
            batch->CommandCount,
            batch->ArgumentBuffer.Get(),
            0,
            nullptr,
            0);
        return true;
    }

    bool ExecuteShadow(
        ID3D12GraphicsCommandList* commandList,
        const StaticModelResource* model)
    {
        if (!commandList || !model || !EnsureInitialized())
        {
            return false;
        }
        IndirectBatch* batch = FindOrBuildBatch(
            model,
            m_StaticShadowBatches,
            BuildStaticShadowBatch);
        if (!batch)
        {
            return false;
        }
        commandList->ExecuteIndirect(
            m_ShadowSignature.Get(),
            batch->CommandCount,
            batch->ArgumentBuffer.Get(),
            0,
            nullptr,
            0);
        return true;
    }

    bool ExecuteVelocity(
        ID3D12GraphicsCommandList* commandList,
        const AnimationModelResource* model)
    {
        if (!commandList || !model || !EnsureInitialized())
        {
            return false;
        }
        IndirectBatch* batch = FindOrBuildBatch(
            model,
            m_AnimatedVelocityBatches,
            BuildAnimatedVelocityBatch);
        if (!batch)
        {
            return false;
        }
        commandList->ExecuteIndirect(
            m_VelocitySignature.Get(),
            batch->CommandCount,
            batch->ArgumentBuffer.Get(),
            0,
            nullptr,
            0);
        return true;
    }

    bool ExecuteVelocity(
        ID3D12GraphicsCommandList* commandList,
        const StaticModelResource* model)
    {
        if (!commandList || !model || !EnsureInitialized())
        {
            return false;
        }
        IndirectBatch* batch = FindOrBuildBatch(
            model,
            m_StaticVelocityBatches,
            BuildStaticVelocityBatch);
        if (!batch)
        {
            return false;
        }
        commandList->ExecuteIndirect(
            m_VelocitySignature.Get(),
            batch->CommandCount,
            batch->ArgumentBuffer.Get(),
            0,
            nullptr,
            0);
        return true;
    }

    void Reset()
    {
        m_AnimatedShadowBatches.clear();
        m_StaticShadowBatches.clear();
        m_AnimatedVelocityBatches.clear();
        m_StaticVelocityBatches.clear();
        m_ShadowSignature.Reset();
        m_VelocitySignature.Reset();
        m_InitializationAttempted = false;
    }
};
