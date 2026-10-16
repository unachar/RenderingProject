#pragma once
#include <d3d12.h>
#include <wrl.h>
#include <array>
#include <vector>
#include <cstdint>
#include <span>

// Retirement follows GraphicsDevice's existing frame fences. BeginFrame is called
// only after the allocator for that swap-chain frame has become safe to reuse.
class GpuSceneBuffer
{
  public:
    static constexpr uint32_t FrameCount = 3;
    void BeginFrame(uint32_t frame);
    bool Ensure(ID3D12Device *device, uint32_t count, uint32_t stride, uint32_t frame, const wchar_t *name,
                bool unorderedAccess = false);
    uint64_t Stage(ID3D12Device *device, uint32_t frame, const void *data, uint32_t count,
                   span<const uint32_t> changes);
    void Copy(ID3D12GraphicsCommandList *commands, uint32_t frame) const;
    ID3D12Resource *Resource() const { return m_Resource.Get(); }
    uint32_t Capacity() const { return m_Capacity; }
    uint64_t MemoryBytes() const;
    D3D12_RESOURCE_STATES State = D3D12_RESOURCE_STATE_COPY_DEST;

  private:
    struct CopyRange
    {
        uint64_t Destination, Source, Size;
    };
    Microsoft::WRL::ComPtr<ID3D12Resource> m_Resource;
    array<Microsoft::WRL::ComPtr<ID3D12Resource>, FrameCount> m_Upload;
    array<vector<Microsoft::WRL::ComPtr<ID3D12Resource>>, FrameCount> m_Retired;
    array<uint64_t, FrameCount> m_UploadBytes{};
    vector<CopyRange> m_Copies;
    uint32_t m_Capacity = 0;
    uint32_t m_Stride = 0;
    bool m_NeedsFullUpload = false;
};

