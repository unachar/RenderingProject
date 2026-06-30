#pragma once
#include "main.h"

class Grid
{
private:
    static ComPtr<ID3D12Resource> VertexBuffer;
    static D3D12_VERTEX_BUFFER_VIEW VertexBufferView;
    static UINT m_VertexCount;
    static ComPtr<ID3D12PipelineState> m_LinePso;
    static ComPtr<ID3D12PipelineState> m_DeferredLinePso;
    static bool m_IsInitialized;

public:
    static void Init(ID3D12Device* device, ID3D12RootSignature* rootSignature,
        int gridSize = 20, float spacing = 1.0f);
    static void Uninit();

    static ID3D12PipelineState* GetLinePso() { return m_LinePso.Get(); }
    static ID3D12PipelineState* GetDeferredLinePso() { return m_DeferredLinePso.Get(); }
    static D3D12_VERTEX_BUFFER_VIEW* GetVertexBufferView() { return &VertexBufferView; }
    static UINT GetVertexCount() { return m_VertexCount; }
    static bool IsInitialized() { return m_IsInitialized; }
};


