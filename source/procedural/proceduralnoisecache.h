#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <string>

namespace ProcStudio
{
    // Adaptive UV tiles shared on one graphics queue. No CPU waits/readbacks.
    // Call BeginFrame before shader access and FinishFrame after all access, once per frame.
    // Complete outstanding submissions before destroying or releasing this object.
    class NoiseSampleCache
    {
      public:
        static constexpr UINT BucketCount = 8192;
        static constexpr UINT Ways = 4;
        static constexpr UINT HeaderBytes = 32;
        static constexpr UINT PageHeaderBytes = 32;
        static constexpr UINT TileExtent = 16;
        static constexpr UINT SampleBytes = 8;
        static constexpr UINT RequestRecordBytes = 256;
        static constexpr UINT PageCount = BucketCount * Ways;
        static constexpr UINT64 CacheBytes = HeaderBytes + UINT64(PageCount) * PageHeaderBytes +
            UINT64(PageCount) * TileExtent * TileExtent * SampleBytes;
        static constexpr UINT64 RequestBytes = UINT64(BucketCount) * RequestRecordBytes;
        static_assert(CacheBytes == 68157472 && RequestBytes == 2097152);

        bool Initialize(ID3D12Device* device, std::string& error);
        bool InitializeFallback(ID3D12Device* device, std::string& error);
        void Release();
        void BeginFrame(ID3D12GraphicsCommandList* commands);
        void FinishFrame(ID3D12GraphicsCommandList* commands);
        // Adjacent raw SRV/UAV descriptors; small fallback resources are separate.
        void WriteViews(ID3D12Device* device, D3D12_CPU_DESCRIPTOR_HANDLE first, UINT increment) const;
        void WriteFallbackViews(ID3D12Device* device, D3D12_CPU_DESCRIPTOR_HANDLE first, UINT increment) const;
        ID3D12Resource* Samples() const { return m_Samples ? m_Samples.Get() : m_FallbackSamples.Get(); }
        ID3D12Resource* Requests() const { return m_Requests ? m_Requests.Get() : m_FallbackRequests.Get(); }
        ID3D12Resource* FallbackSamples() const { return m_FallbackSamples.Get(); }
        ID3D12Resource* FallbackRequests() const { return m_FallbackRequests.Get(); }
        bool Ready() const { return m_Pipeline != nullptr; }

      private:
        void Merge(ID3D12GraphicsCommandList* commands, bool clear);
        Microsoft::WRL::ComPtr<ID3D12RootSignature> m_Root;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_Pipeline;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_Samples, m_Requests;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_FallbackSamples, m_FallbackRequests;
        bool m_Cleared = false;
        UINT m_Frame = 0;
    };
}
