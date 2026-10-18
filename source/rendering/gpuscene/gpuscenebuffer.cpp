#include "pch.h"
#include "gpuscenebuffer.h"
#include "rendererstate.h"
static_assert(GpuSceneBuffer::FrameCount == RendererState::g_kFRAME_COUNT);

namespace
{
void ThrowIfFailed(HRESULT result)
{
    if (FAILED(result))
    {
        throw runtime_error("GPU scene buffer allocation/map failed");
    }
}

ComPtr<ID3D12Resource> CreateBuffer(ID3D12Device* device, uint64_t bytes, D3D12_HEAP_TYPE heapType,
                                    D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags)
{
    ComPtr<ID3D12Resource> resource;
    const auto heap = CD3DX12_HEAP_PROPERTIES(heapType);
    const auto description = CD3DX12_RESOURCE_DESC::Buffer(bytes, flags);
    ThrowIfFailed(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description, state, nullptr,
                                                  IID_PPV_ARGS(&resource)));
    return resource;
}
} // namespace

void GpuSceneBuffer::BeginFrame(uint32_t frame)
{
    m_Retired.at(frame).clear();
    m_Copies.clear();
}

bool GpuSceneBuffer::Ensure(ID3D12Device* device, uint32_t count, uint32_t stride, uint32_t frame, const wchar_t* name,
                            bool unorderedAccess)
{
    if (m_Resource && count <= m_Capacity)
    {
        return false;
    }
    uint32_t capacity = max(1024u, m_Capacity);
    while (capacity < count)
    {
        if (capacity > UINT32_MAX / 2)
        {
            throw overflow_error("GPU scene capacity overflow");
        }
        capacity *= 2;
    }
    auto resource =
        CreateBuffer(device, uint64_t(capacity) * stride, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST,
                     unorderedAccess ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE);
    resource->SetName(name);
    if (m_Resource)
    {
        m_Retired.at(frame).push_back(m_Resource);
    }
    m_Resource = resource;
    m_Capacity = capacity;
    m_Stride = stride;
    State = D3D12_RESOURCE_STATE_COPY_DEST;
    m_NeedsFullUpload = true;
    return true;
}

uint64_t GpuSceneBuffer::Stage(ID3D12Device* device, uint32_t frame, const void* data, uint32_t count,
                               span<const uint32_t> changes)
{
    m_Copies.clear();
    uint64_t bytes = uint64_t(m_NeedsFullUpload ? count : changes.size()) * m_Stride;
    if (!bytes)
    {
        return 0;
    }
    if (m_UploadBytes.at(frame) < bytes)
    {
        if (m_Upload[frame])
        {
            m_Retired[frame].push_back(m_Upload[frame]);
        }
        uint64_t capacity = 65536;
        while (capacity < bytes)
        {
            capacity *= 2;
        }
        m_Upload[frame] = CreateBuffer(device, capacity, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
                                       D3D12_RESOURCE_FLAG_NONE);
        m_UploadBytes[frame] = capacity;
    }
    void* mapped = nullptr;
    const D3D12_RANGE noRead{0, 0};
    ThrowIfFailed(m_Upload[frame]->Map(0, &noRead, &mapped));
    const auto* source = static_cast<const uint8_t*>(data);
    auto* destination = static_cast<uint8_t*>(mapped);
    if (m_NeedsFullUpload)
    {
        memcpy(destination, source, static_cast<size_t>(bytes));
        m_Copies.push_back({0, 0, bytes});
    }
    else
    {
        uint64_t offset = 0;
        for (uint32_t id : changes)
        {
            const uint64_t target = uint64_t(id) * m_Stride;
            memcpy(destination + offset, source + target, m_Stride);
            if (!m_Copies.empty() && m_Copies.back().Destination + m_Copies.back().Size == target)
            {
                m_Copies.back().Size += m_Stride;
            }
            else
            {
                m_Copies.push_back({target, offset, m_Stride});
            }
            offset += m_Stride;
        }
    }
    const D3D12_RANGE written{0, static_cast<SIZE_T>(bytes)};
    m_Upload[frame]->Unmap(0, &written);
    m_NeedsFullUpload = false;
    return bytes;
}

void GpuSceneBuffer::Copy(ID3D12GraphicsCommandList* commands, uint32_t frame) const
{
    for (const auto& range : m_Copies)
    {
        commands->CopyBufferRegion(m_Resource.Get(), range.Destination, m_Upload.at(frame).Get(), range.Source,
                                   range.Size);
    }
}

uint64_t GpuSceneBuffer::MemoryBytes() const
{
    uint64_t bytes = uint64_t(m_Capacity) * m_Stride;
    for (uint64_t upload : m_UploadBytes)
    {
        bytes += upload;
    }
    for (const auto& frame : m_Retired)
    {
        for (const auto& resource : frame)
        {
            bytes += resource->GetDesc().Width;
        }
    }
    return bytes;
}
