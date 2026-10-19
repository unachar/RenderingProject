#pragma once
// =============================================================================================
// 物理ベース描画の参照テーブル（PbrLuts）
//   LTC 行列（RGBA32F 64x64）と LTC 振幅 / DFG（RG32F 64x64）を GPU に転送する。
//   DFG はスプリットサム IBL とマルチスキャッタリングのエネルギー補償、LTC は矩形エリアライトに使う。
//   テーブルは ltctables.h（オフラインでフィットした値）。初期化時に 1 回だけ転送する。
//   ディファード照明のルート引数 27（t42, t43）にバインドする。ヘッダーのみ。
// =============================================================================================
#include "pch.h"
#include "rendererstate.h"
#include "graphicslog.h"
#include "ltctables.h"

class PbrLuts final
{
  public:
    static bool Initialize(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12DescriptorHeap* engineHeap)
    {
        Shutdown();
        if (!device || !queue || !engineHeap)
        {
            return false;
        }
        s_EngineHeap = engineHeap;
        s_Increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        constexpr UINT size = LtcTables::g_kSize;
        if (!CreateTexture(device, s_Matrix, DXGI_FORMAT_R32G32B32A32_FLOAT, size, L"LTC Matrix LUT") ||
            !CreateTexture(device, s_Amplitude, DXGI_FORMAT_R32G32_FLOAT, size, L"LTC Amplitude / DFG LUT"))
        {
            return Fail();
        }

        // 両テクスチャのステージングバッファ（行ピッチは256バイトに整列）。
        const UINT matrixPitch = Align(size * 16u, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
        const UINT amplitudePitch = Align(size * 8u, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
        const UINT amplitudeOffset = Align(matrixPitch * size, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
        const UINT64 uploadBytes = amplitudeOffset + amplitudePitch * size;
        ComPtr<ID3D12Resource> upload;
        const auto uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        const auto uploadDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadBytes);
        if (FAILED(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload))))
        {
            return Fail();
        }
        BYTE* mapped = nullptr;
        if (FAILED(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped))))
        {
            return Fail();
        }
        for (UINT row = 0; row < size; ++row)
        {
            memcpy(mapped + row * matrixPitch, LtcTables::g_kLtcMatrix + row * size * 4, size * 16u);
            memcpy(mapped + amplitudeOffset + row * amplitudePitch, LtcTables::g_kLtcAmplitude + row * size * 2,
                   size * 8u);
        }
        upload->Unmap(0, nullptr);

        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        ComPtr<ID3D12Fence> fence;
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                             IID_PPV_ARGS(&list))) ||
            FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
        {
            return Fail();
        }
        const auto copy = [&](ID3D12Resource* texture, DXGI_FORMAT format, UINT64 offset, UINT pitch) {
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
            footprint.Offset = offset;
            footprint.Footprint = {format, size, size, 1, pitch};
            const CD3DX12_TEXTURE_COPY_LOCATION destination(texture, 0);
            const CD3DX12_TEXTURE_COPY_LOCATION source(upload.Get(), footprint);
            list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        };
        copy(s_Matrix.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT, 0, matrixPitch);
        copy(s_Amplitude.Get(), DXGI_FORMAT_R32G32_FLOAT, amplitudeOffset, amplitudePitch);
        const D3D12_RESOURCE_BARRIER toRead[] = {
            CD3DX12_RESOURCE_BARRIER::Transition(s_Matrix.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(s_Amplitude.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
        list->ResourceBarrier(2, toRead);
        if (FAILED(list->Close()))
        {
            return Fail();
        }
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!event || FAILED(queue->Signal(fence.Get(), 1)) || FAILED(fence->SetEventOnCompletion(1, event)))
        {
            if (event)
            {
                CloseHandle(event);
            }
            return Fail();
        }
        WaitForSingleObject(event, INFINITE);
        CloseHandle(event);

        WriteSrv(device, s_Matrix.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT, 0);
        WriteSrv(device, s_Amplitude.Get(), DXGI_FORMAT_R32G32_FLOAT, 1);
        s_Available = true;
        WriteGraphicsLog("PbrLuts: LTC / DFG tables uploaded (%ux%u)\n", size, size);
        return true;
    }

    static void Shutdown()
    {
        s_Available = false;
        s_Matrix.Reset();
        s_Amplitude.Reset();
        s_EngineHeap.Reset();
    }

    static bool IsAvailable()
    {
        return s_Available;
    }

    // 後処理のルートシグネチャのルートパラメーター27（t42、t43）。
    static D3D12_GPU_DESCRIPTOR_HANDLE GetSrv()
    {
        return CD3DX12_GPU_DESCRIPTOR_HANDLE(s_EngineHeap->GetGPUDescriptorHandleForHeapStart(),
                                             RendererState::g_kQUALITY_DESCRIPTOR_START, s_Increment);
    }

    // 初期化失敗時もテーブルを有効に保つため、null 記述子を書き込む。
    static void WriteNullDescriptors(ID3D12Device* device, ID3D12DescriptorHeap* engineHeap)
    {
        s_EngineHeap = engineHeap;
        s_Increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        WriteSrv(device, nullptr, DXGI_FORMAT_R32G32B32A32_FLOAT, 0);
        WriteSrv(device, nullptr, DXGI_FORMAT_R32G32_FLOAT, 1);
    }

  private:
    static inline bool s_Available = false;
    static inline UINT s_Increment = 0;
    static inline ComPtr<ID3D12DescriptorHeap> s_EngineHeap;
    static inline ComPtr<ID3D12Resource> s_Matrix, s_Amplitude;

    static UINT Align(UINT value, UINT alignment)
    {
        return (value + alignment - 1) & ~(alignment - 1);
    }

    static bool Fail()
    {
        WriteGraphicsLog("PbrLuts: initialisation failed, analytic DFG fallback\n");
        s_Matrix.Reset();
        s_Amplitude.Reset();
        s_Available = false;
        return false;
    }

    static bool CreateTexture(ID3D12Device* device, ComPtr<ID3D12Resource>& texture, DXGI_FORMAT format, UINT size,
                              const wchar_t* name)
    {
        const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(format, size, size, 1, 1);
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                   nullptr, IID_PPV_ARGS(texture.ReleaseAndGetAddressOf()))))
        {
            return false;
        }
        texture->SetName(name);
        return true;
    }

    static void WriteSrv(ID3D12Device* device, ID3D12Resource* resource, DXGI_FORMAT format, UINT index)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
        desc.Format = format;
        desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(
            resource, &desc,
            CD3DX12_CPU_DESCRIPTOR_HANDLE(s_EngineHeap->GetCPUDescriptorHandleForHeapStart(),
                                          RendererState::g_kQUALITY_DESCRIPTOR_START + index, s_Increment));
    }
};
