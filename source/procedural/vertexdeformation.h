#pragma once
#include "pch.h"
#include "ecs.h"

struct VertexDeformationView
{
    ID3D12Resource* CurrentResource = nullptr;
    ID3D12Resource* PreviousResource = nullptr;
    D3D12_VERTEX_BUFFER_VIEW CurrentView{};
    D3D12_VERTEX_BUFFER_VIEW PreviousView{};
    uint64_t GeometryRevision = 0;
};

class VertexDeformation
{
public:
    static bool IsRequested(EntityID entity);
    // Call once after the safe frame allocator has been reset, before any geometry passes.
    static void PrepareFrame(ID3D12GraphicsCommandList* commandList, UINT frameIndex);
    static bool TryResolveMesh(EntityID entity, VertexDeformationView& output);
    static bool TryResolveStaticMesh(EntityID entity, UINT meshIndex, VertexDeformationView& output,
        int outlineMode = -1);
    static bool TryGetLocalBounds(EntityID entity, XMFLOAT3& center, XMFLOAT3& extents);
    static void ResetHistory(EntityID entity);
    static void Shutdown();
    static UINT GetDispatchCount();
};

