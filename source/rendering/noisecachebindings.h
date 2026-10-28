#pragma once
#include "rendererstate.h"
#include "procedural/proceduralnoisecache.h"

// All users record on the graphics queue. Private preview/export queues continue
// to use the analytical evaluator and do not touch this shared cache.
class NoiseCacheBindings : protected RendererState
{
  public:
    static constexpr UINT MainRoot = 37;
    static constexpr UINT GpuDrivenRoot = 14;
    static constexpr UINT MeshRoot = 18;
    static constexpr UINT ReflectionRoot = 22;

    static void Request()
    {
        if (Enabled())
        {
            s_Requested = true;
        }
    }

    static bool InitializeFallback()
    {
        std::string error;
        if (!s_Cache.InitializeFallback(m_Device.Get(), error))
        {
            Debug::Log("Noise cache: %s\n", error.c_str());
            return false;
        }
        s_Cache.WriteFallbackViews(m_Device.Get(), Cpu(g_kNOISE_CACHE_DESCRIPTOR_START), m_CbvIncrementSize);
        s_NullViewsWritten = true;
        return true;
    }

    static void BeginFrame(ID3D12GraphicsCommandList* commands)
    {
        if (!s_NullViewsWritten)
        {
            return;
        }
        if (s_Requested && !s_Cache.Ready() && !s_Failed)
        {
            std::string error;
            if (!s_Cache.Initialize(m_Device.Get(), error))
            {
                Debug::Log("Noise cache: %s; using analytical evaluation\n", error.c_str());
                s_Failed = true;
            }
            else
            {
                s_Cache.WriteViews(m_Device.Get(), Cpu(g_kNOISE_CACHE_DESCRIPTOR_START + 2), m_CbvIncrementSize);
                Debug::Log("Noise cache: enabled, 65 MiB adaptive tiles + 2 MiB requests\n");
            }
        }
        s_Requested = false;
        s_Cache.BeginFrame(commands);
    }

    static void FinishFrame(ID3D12GraphicsCommandList* commands)
    {
        if (s_Requested)
        {
            s_Cache.FinishFrame(commands);
        }
    }

    static void Shutdown()
    {
        s_Cache.Release();
        s_Requested = false;
        s_Failed = false;
        s_NullViewsWritten = false;
    }

    // The main root already occupies 63 DWORDs: one table fits the 64-DWORD limit.
    static void InitializeMainRoot(CD3DX12_ROOT_PARAMETER& root, CD3DX12_DESCRIPTOR_RANGE (&ranges)[2])
    {
        ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 15);
        ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 15);
        root.InitAsDescriptorTable(2, ranges, D3D12_SHADER_VISIBILITY_PIXEL);
    }

    static void BindMain(ID3D12GraphicsCommandList* commands)
    {
        const UINT index = g_kNOISE_CACHE_DESCRIPTOR_START + (s_Cache.Ready() ? 2u : 0u);
        commands->SetGraphicsRootDescriptorTable(MainRoot,
            CD3DX12_GPU_DESCRIPTOR_HANDLE(m_CbvHeap->GetGPUDescriptorHandleForHeapStart(), index, m_CbvIncrementSize));
    }

    static void BindGraphics(ID3D12GraphicsCommandList* commands, UINT root)
    {
        commands->SetGraphicsRootShaderResourceView(root, SampleAddress());
        commands->SetGraphicsRootUnorderedAccessView(root + 1, RequestAddress());
    }

    static void BindCompute(ID3D12GraphicsCommandList* commands, UINT root)
    {
        commands->SetComputeRootShaderResourceView(root, SampleAddress());
        commands->SetComputeRootUnorderedAccessView(root + 1, RequestAddress());
    }

  private:
    static bool Enabled()
    {
        static const bool enabled = [] {
            char value[8]{};
            return GetEnvironmentVariableA("DX12_NOISE_CACHE", value, sizeof(value)) == 0 || atoi(value) != 0;
        }();
        return enabled;
    }

    static D3D12_CPU_DESCRIPTOR_HANDLE Cpu(UINT index)
    {
        return CD3DX12_CPU_DESCRIPTOR_HANDLE(m_CbvHeap->GetCPUDescriptorHandleForHeapStart(), index, m_CbvIncrementSize);
    }

    static D3D12_GPU_VIRTUAL_ADDRESS SampleAddress()
    {
        return s_Cache.Samples() ? s_Cache.Samples()->GetGPUVirtualAddress() : 0;
    }

    static D3D12_GPU_VIRTUAL_ADDRESS RequestAddress()
    {
        return s_Cache.Requests() ? s_Cache.Requests()->GetGPUVirtualAddress() : 0;
    }

    inline static ProcStudio::NoiseSampleCache s_Cache;
    inline static bool s_Requested = false;
    inline static bool s_Failed = false;
    inline static bool s_NullViewsWritten = false;
};
