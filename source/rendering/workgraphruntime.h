#pragma once

#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl.h>
#include <array>
#include <cstddef>

namespace WorkGraph
{
    constexpr UINT FrameSlots = 3;

    HRESULT CompileFile(const wchar_t* file, const wchar_t* entry, const wchar_t* profile,
                        Microsoft::WRL::ComPtr<ID3DBlob>& bytecode);
    HRESULT CompileSource(const char* source, size_t length, const wchar_t* sourceName, const wchar_t* entry,
                          const wchar_t* profile, Microsoft::WRL::ComPtr<ID3DBlob>& bytecode);
    bool EnablePreviewFeatures();
    bool IsSupported(ID3D12Device* device, bool requireMeshNodes = false);

    struct MeshGraphDescription
    {
        ID3D12RootSignature* RootSignature = nullptr;
        D3D12_SHADER_BYTECODE Library{};
        D3D12_SHADER_BYTECODE PixelShader{};
        const wchar_t* PixelShaderEntry = L"PixelMain";
        const wchar_t* ProgramName = L"MeshGraph";
        const wchar_t* EntryNode = L"Entry";
        const wchar_t* MeshNode = L"Mesh";
        const wchar_t* MeshShader = L"MeshMain";
        D3D12_BLEND_DESC Blend{};
        D3D12_RASTERIZER_DESC Rasterizer{};
        D3D12_DEPTH_STENCIL_DESC DepthStencil{};
        D3D12_RT_FORMAT_ARRAY RenderTargets{};
        DXGI_FORMAT DepthFormat = DXGI_FORMAT_UNKNOWN;
        UINT MaximumInputRecords = 1;
        UINT MaximumMeshRecordsPerEntry = 0;
    };

    class Graph
    {
      public:
        // The caller owns submission and must retire the frame fence before reusing its slot
        // or destroying/reinitializing the graph. Root bindings and output barriers stay explicit.
        bool DispatchCpu(ID3D12GraphicsCommandList* commands, UINT frameIndex, const void* records, UINT recordStride,
                         UINT numRecords = 1);
        // inputAddress points to a GPU D3D12_NODE_GPU_INPUT, including the resolved entry index.
        bool DispatchGpu(ID3D12GraphicsCommandList* commands, UINT frameIndex, D3D12_GPU_VIRTUAL_ADDRESS inputAddress);

        UINT EntrypointIndex() const
        {
            return m_EntrypointIndex;
        }

        UINT64 BackingMemoryBytes() const
        {
            return m_BackingMemoryBytes;
        }

        UINT64 DispatchCount() const
        {
            return m_DispatchCount;
        }

        bool IsInitialized() const
        {
            return m_StateObject != nullptr;
        }

        void Reset();

      protected:
        bool FinishInitialization(ID3D12Device* device, const D3D12_STATE_OBJECT_DESC& description,
                                  const wchar_t* programName, const wchar_t* entryNode, UINT maximumInputRecords);

      private:
        bool Bind(ID3D12GraphicsCommandList* commands, UINT frameIndex,
                  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList10>& graphCommands);
        Microsoft::WRL::ComPtr<ID3D12StateObject> m_StateObject;
        std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, FrameSlots> m_BackingMemory;
        D3D12_PROGRAM_IDENTIFIER m_ProgramIdentifier{};
        UINT m_EntrypointIndex = 0;
        UINT m_MaximumInputRecords = 1;
        UINT64 m_BackingMemoryBytes = 0;
        UINT64 m_DispatchCount = 0;
    };

    class ComputeGraph : public Graph
    {
      public:
        bool Initialize(ID3D12Device* device, ID3D12RootSignature* rootSignature, const wchar_t* file,
                        const wchar_t* entryNode);
    };

    class MeshGraph : public Graph
    {
      public:
        bool Initialize(ID3D12Device* device, const MeshGraphDescription& description);
    };
}
