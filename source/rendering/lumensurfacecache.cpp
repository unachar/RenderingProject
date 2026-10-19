#include "pch.h"
#include "rendererstate.h"
#include "lumensurfacecache.h"
#include "graphicsdevice.h"
#include "envflags.h"

namespace
{
    bool CreateBuffer(ID3D12Device* device, ComPtr<ID3D12Resource>& resource, UINT64 bytes, bool upload)
    {
        const auto heap = CD3DX12_HEAP_PROPERTIES(upload ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_DEFAULT);
        const auto desc = CD3DX12_RESOURCE_DESC::Buffer(
            (std::max)(bytes, 256ull), upload ? D3D12_RESOURCE_FLAG_NONE : D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                         upload ? D3D12_RESOURCE_STATE_GENERIC_READ
                                                                : D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                         nullptr, IID_PPV_ARGS(resource.ReleaseAndGetAddressOf())));
    }

    bool Upload(ID3D12Resource* buffer, const void* source, size_t bytes)
    {
        void* mapped = nullptr;
        D3D12_RANGE noRead{};
        if (FAILED(buffer->Map(0, &noRead, &mapped)))
        {
            return false;
        }
        if (bytes != 0)
        {
            memcpy(mapped, source, bytes);
        }
        buffer->Unmap(0, nullptr);
        return true;
    }

    void Hash(uint64_t& hash, const void* data, size_t size)
    {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < size; ++i)
        {
            hash = (hash ^ bytes[i]) * 1099511628211ull;
        }
    }
}

bool LumenSurfaceCache::Prepare(ID3D12Device* device, UINT frame, const vector<RaytracingGeometry>& geometry,
                                const XMFLOAT3& camera, UINT width, UINT height, UINT spacing, UINT captureBudget,
                                UINT lightingBudget, UINT debugView, bool resetLighting, UINT sequence,
                                bool changingScene)
{
    if (!m_Atlases[0])
    {
        const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        for (UINT i = 0; i < m_Atlases.size(); ++i)
        {
            const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(
                i == 0 ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT, AtlasWidth, AtlasHeight, 1, 1,
                1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                       IID_PPV_ARGS(&m_Atlases[i]))))
            {
                Reset();
                return false;
            }
            m_Atlases[i]->SetName(L"Lumen Surface Atlas");
        }
        if (!CreateBuffer(device, m_Bounds, Capacity * 32ull, false))
        {
            Reset();
            return false;
        }
    }
    const UINT columns = (width + spacing - 1) / spacing;
    const UINT rows = (height + spacing - 1) / spacing;
    const UINT count = columns * rows;
    if (count > m_ProbeCount)
    {
        if (m_Probes && !GraphicsDevice::WaitForGpuIdle())
        {
            return false;
        }
        // 4 x float4 のレコードとガイドを2フレーム分保持する（現在と再投影履歴）。
        if (!CreateBuffer(device, m_Probes, UINT64(count) * 2 * 4 * 16, false) ||
            !CreateBuffer(device, m_ProbeGuides, UINT64(count) * 2 * 32, false))
        {
            return false;
        }
        m_ProbeCount = count;
        m_ProbeHistory = false;
        m_Probes->SetName(L"Lumen Screen Probe Radiance");
        m_ProbeGuides->SetName(L"Lumen Screen Probe Guides");
    }
    vector<Lumen::Input> inputs;
    inputs.reserve(geometry.size());
    for (const auto& mesh : geometry)
    {
        uint64_t revision = 14695981039346656037ull;
        Hash(revision, &mesh.World, sizeof(mesh.World));
        Hash(revision, &mesh.VertexView, sizeof(mesh.VertexView));
        Hash(revision, &mesh.IndexView, sizeof(mesh.IndexView));
        Hash(revision, &mesh.BaseColor, sizeof(mesh.BaseColor));
        Hash(revision, &mesh.Emission, sizeof(mesh.Emission));
        Hash(revision, &mesh.Metallic, sizeof(mesh.Metallic));
        Hash(revision, &mesh.UsesAssetMaterial, sizeof(mesh.UsesAssetMaterial));
        const auto texture = mesh.Texture.Get(), emission = mesh.EmissionTexture.Get();
        Hash(revision, &texture, sizeof(texture));
        Hash(revision, &emission, sizeof(emission));
        Hash(revision, &mesh.TextureResidentMip, sizeof(mesh.TextureResidentMip));
        Hash(revision, &mesh.EmissionResidentMip, sizeof(mesh.EmissionResidentMip));
        const XMFLOAT3 offset{mesh.World._41 - camera.x, mesh.World._42 - camera.y, mesh.World._43 - camera.z};
        const float distance = offset.x * offset.x + offset.y * offset.y + offset.z * offset.z;
        inputs.push_back({!mesh.Deforming && mesh.Transmission <= 0.001f ? mesh.SurfaceOwner : 0, revision, distance});
    }
    // アップロード成功後だけ所有関係を公開し、失敗したフレームは再試行時に再取得する。
    auto scheduler = m_Scheduler;
    m_Schedule = scheduler.Prepare(inputs, captureBudget, lightingBudget);
    vector<UINT> lookupSlots = m_Schedule.Slots;
    for (auto& slot : lookupSlots)
    {
        if (slot != Lumen::InvalidSlot &&
            !Lumen::CanReuseSurfaceLighting(sequence, m_LastUpdate[slot], m_LastSceneChange, changingScene))
        {
            slot = Lumen::InvalidSlot;
        }
    }
    for (const auto work : m_Schedule.Updates)
    {
        m_LastUpdate[work.Slot] = sequence;
    }
    auto& uploads = m_Frames[frame];
    const UINT64 mapBytes = (std::max)(256ull, UINT64(inputs.size()) * 4);
    if (!uploads.Map || uploads.Map->GetDesc().Width < mapBytes)
    {
        if (!CreateBuffer(device, uploads.Map, mapBytes, true))
        {
            return false;
        }
    }
    if (!uploads.Work && !CreateBuffer(device, uploads.Work, Capacity * 2ull * 16, true))
    {
        return false;
    }
    if (!uploads.Parameters && !CreateBuffer(device, uploads.Parameters, 256, true))
    {
        return false;
    }
    vector<XMUINT4> work;
    const auto append = [&](const vector<Lumen::Work>& list) {
        for (const auto entry : list)
        {
            const auto& mesh = geometry[entry.Instance];
            work.push_back({entry.Instance, entry.Slot,
                            mesh.VertexView.SizeInBytes / (std::max)(mesh.VertexView.StrideInBytes, 1u), 0});
        }
    };
    append(m_Schedule.Captures);
    append(m_Schedule.Updates);
    m_Clear = m_Clear || resetLighting;
    const bool gridStable = columns == m_ProbeColumns && rows == m_ProbeRows && spacing == m_ProbeSpacing;
    const bool probeHistory = m_ProbeHistory && gridStable && !resetLighting;
    const UINT parity = m_ProbeParity ^ 1u;
    m_Constants.Counts = {1, static_cast<UINT>(m_Schedule.Captures.size()),
                          static_cast<UINT>(m_Schedule.Updates.size()), parity | (probeHistory ? 2u : 0u)};
    m_Constants.Grid = {columns, rows, spacing, Capacity};
    static const bool singleBounce = ReadEnvironmentFlagOnce("DX12_LUMEN_SINGLE_BOUNCE");
    // 既定は深度バッファによる再接続判定。DX12_LUMEN_RAY_VISIBILITY で TLAS のレイ判定に戻す。
    static const bool rayVisibility = ReadEnvironmentFlagOnce("DX12_LUMEN_RAY_VISIBILITY");
    m_Constants.Settings = {changingScene ? 2.f : 16.f, static_cast<float>(debugView), rayVisibility ? 1.f : 0.f,
                            singleBounce ? 0.f : 1.f};
    if (!Upload(uploads.Map.Get(), lookupSlots.data(), lookupSlots.size() * 4) ||
        !Upload(uploads.Work.Get(), work.data(), work.size() * sizeof(XMUINT4)) ||
        !Upload(uploads.Parameters.Get(), &m_Constants, sizeof(m_Constants)))
    {
        return false;
    }
    m_Scheduler = std::move(scheduler);
    m_ProbeParity = parity;
    m_ProbeColumns = columns;
    m_ProbeRows = rows;
    m_ProbeSpacing = spacing;
    m_ProbeHistory = true;
    return true;
}

void LumenSurfaceCache::WriteDescriptors(ID3D12Device* device, UINT frame, D3D12_CPU_DESCRIPTOR_HANDLE start,
                                         UINT increment)
{
    auto target = [&](UINT index) {
        return CD3DX12_CPU_DESCRIPTOR_HANDLE(start, index, increment);
    };
    for (UINT i = 0; i < 3; ++i)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
        desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.Buffer.StructureByteStride = i == 0 ? 4 : 16;
        auto* resource = i == 0 ? m_Frames[frame].Map.Get() : m_Frames[frame].Work.Get();
        desc.Buffer.NumElements =
            resource ? static_cast<UINT>(resource->GetDesc().Width / desc.Buffer.StructureByteStride) : 1;
        device->CreateShaderResourceView(resource, &desc, target(i));
    }
    for (UINT i = 0; i < 11; ++i)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
        ID3D12Resource* resource = nullptr;
        if (i < 8)
        {
            UINT atlas = i;
            if (i >= 4 && i < 6)
            {
                atlas = 4 + (m_Read ^ (i - 4));
            }
            else if (i >= 6)
            {
                atlas = 6 + (m_Read ^ (i - 6));
            }
            resource = m_Atlases[atlas].Get();
            desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            desc.Format = i == 0 ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT;
        }
        else
        {
            switch (i)
            {
                case 8:
                    resource = m_Bounds.Get();
                    break;
                case 9:
                    resource = m_Probes.Get();
                    break;
                default:
                    resource = m_ProbeGuides.Get();
                    break;
            }
            desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            desc.Buffer.StructureByteStride = i == 9 ? 16 : 32;
            desc.Buffer.NumElements =
                resource ? static_cast<UINT>(resource->GetDesc().Width / desc.Buffer.StructureByteStride) : 1;
        }
        device->CreateUnorderedAccessView(resource, nullptr, &desc, target(3 + i));
    }
}

D3D12_GPU_VIRTUAL_ADDRESS LumenSurfaceCache::ConstantsAddress(UINT frame) const
{
    return m_Frames[frame].Parameters->GetGPUVirtualAddress();
}

void LumenSurfaceCache::Barrier(ID3D12GraphicsCommandList* commands) const
{
    // 全キャッシュ段階はグラフィックスキューの UAV を使う。取得、照明、利用側の順序を保証する。
    const auto barrier = CD3DX12_RESOURCE_BARRIER::UAV(nullptr);
    commands->ResourceBarrier(1, &barrier);
}

void LumenSurfaceCache::BeginLighting(ID3D12GraphicsCommandList* commands)
{
    // このフレームに書き込みがなければ、現在の読み取りアトラスを保持し、コピーも交換もしない。
    if (m_Constants.Counts.z == 0)
    {
        return;
    }
    // クリアと取得は両方のアトラスに書くため、交換後の書き込みアトラスで不足するのは
    // 前回の照明パスで更新したスロットだけ。毎フレームの 2 x 3 MiB コピーを避け、その行だけコピーする。
    if (m_Clear)
    {
        m_FullCopy = false;
        m_DirtySlots.clear();
    }
    const bool full = m_FullCopy;
    if (!full && m_DirtySlots.empty())
    {
        return;
    }
    vector<D3D12_RESOURCE_BARRIER> before, after;
    for (UINT base : {4u, 6u})
    {
        auto* source = m_Atlases[base + m_Read].Get();
        auto* destination = m_Atlases[base + (m_Read ^ 1u)].Get();
        before.push_back(CD3DX12_RESOURCE_BARRIER::Transition(source, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                              D3D12_RESOURCE_STATE_COPY_SOURCE));
        before.push_back(CD3DX12_RESOURCE_BARRIER::Transition(destination, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                              D3D12_RESOURCE_STATE_COPY_DEST));
        after.push_back(CD3DX12_RESOURCE_BARRIER::Transition(source, D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
        after.push_back(CD3DX12_RESOURCE_BARRIER::Transition(destination, D3D12_RESOURCE_STATE_COPY_DEST,
                                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
    }
    commands->ResourceBarrier(static_cast<UINT>(before.size()), before.data());
    for (UINT base : {4u, 6u})
    {
        auto* source = m_Atlases[base + m_Read].Get();
        auto* destination = m_Atlases[base + (m_Read ^ 1u)].Get();
        if (full)
        {
            commands->CopyResource(destination, source);
            continue;
        }
        const CD3DX12_TEXTURE_COPY_LOCATION dst(destination, 0), src(source, 0);
        for (UINT slot : m_DirtySlots)
        {
            const D3D12_BOX box{0, slot * CardSize, 0, AtlasWidth, (slot + 1) * CardSize, 1};
            commands->CopyTextureRegion(&dst, 0, slot * CardSize, 0, &src, &box);
        }
    }
    commands->ResourceBarrier(static_cast<UINT>(after.size()), after.data());
    m_FullCopy = false;
}

void LumenSurfaceCache::EndLighting(ID3D12GraphicsCommandList* commands)
{
    Barrier(commands);
    m_Clear = false;
    if (m_Constants.Counts.z == 0)
    {
        return;
    }
    // 現在のフレームは前のアトラスを読み、次のフレームで更新済みの内容を使う。
    m_Read ^= 1u;
    m_DirtySlots.clear();
    for (const auto work : m_Schedule.Updates)
    {
        m_DirtySlots.push_back(work.Slot);
    }
}

UINT LumenSurfaceCache::ResidentMeshes() const
{
    return static_cast<UINT>(std::count_if(m_Schedule.Slots.begin(), m_Schedule.Slots.end(), [](UINT slot) {
        return slot != Lumen::InvalidSlot;
    }));
}

UINT64 LumenSurfaceCache::MemoryBytes() const
{
    return m_Atlases[0] ? UINT64(AtlasWidth) * AtlasHeight * (16 + 7 * 8) + Capacity * 32ull +
                              UINT64(m_ProbeCount) * 2 * (64 + 32)
                        : 0;
}

void LumenSurfaceCache::Reset()
{
    *this = LumenSurfaceCache{};
}
