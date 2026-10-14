#include "pch.h"
#include "virtualgeometryresources.h"
#include "graphicsdevice.h"
#include "graphicslog.h"
#include <numeric>

namespace VirtualGeometryResources
{
namespace
{
struct State
{
    VirtualGeometryGPU::AssetRegistry Registry;
    GpuSceneBuffer Clusters, Groups, Hierarchy, Roots, Vertices, Triangles, Assets;
    bool Dirty = true;
    uint32_t ReadyCount = 0;
};
State& Data()
{
    static State state;
    return state;
}
}
uint32_t Register(std::shared_ptr<const VirtualGeometryRuntime::RuntimeAsset> asset)
{
    auto& state = Data();
    auto before = state.Registry.Table().size();
    std::string error;
    uint32_t id = state.Registry.Register(std::move(asset), error);
    state.Dirty |= state.Registry.Table().size() != before;
    if (id == UINT32_MAX && !error.empty())
    {
        WriteGraphicsLog("Native VirtualGeometry registration fallback: %s\n", error.c_str());
    }
    return id;
}
void Prepare(RenderGraph& graph)
{
    auto& state = Data();
    const uint32_t frame = GraphicsDevice::GetFrameIndex();
    const auto& geometry = state.Registry.Geometry();
    state.ReadyCount = 0;
    auto upload = [&](GpuSceneBuffer& buffer, const auto& values, const char* name)
    {
        using Element = typename std::decay_t<decltype(values)>::value_type;
        buffer.BeginFrame(frame);
        const bool resized = buffer.Ensure(GraphicsDevice::GetDevice(), uint32_t(values.size()), sizeof(Element), frame,
            L"Native VirtualGeometry asset");
        std::vector<uint32_t> changes;
        if (state.Dirty || resized)
        {
            changes.resize(values.size());
            std::iota(changes.begin(), changes.end(), 0u);
        }
        uint64_t bytes = buffer.Stage(GraphicsDevice::GetDevice(), frame, values.data(), uint32_t(values.size()), changes);
        RenderGraph::ImportedResource input{};
        input.Name = name;
        input.Resource = buffer.Resource();
        input.InitialState = buffer.State;
        input.HasFinalState = true;
        input.FinalState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        buffer.State = input.FinalState;
        auto handle = graph.ImportResource(input);
        if (bytes)
        {
            auto* target = &buffer;
            graph.AddPass(name,
                [=](RenderGraph::PassBuilder& builder) { builder.Write(handle, D3D12_RESOURCE_STATE_COPY_DEST); },
                [=](ID3D12GraphicsCommandList* list) { target->Copy(list, frame); });
        }
    };
    upload(state.Clusters, geometry.Clusters, "Nanite Cluster Upload");
    upload(state.Groups, geometry.Groups, "Nanite Group Upload");
    upload(state.Hierarchy, geometry.HierarchyIndices, "Nanite Hierarchy Upload");
    upload(state.Roots, geometry.Roots, "Nanite Root Upload");
    upload(state.Vertices, geometry.Vertices, "Nanite Vertex Upload");
    upload(state.Triangles, geometry.Triangles, "Nanite Triangle Upload");
    upload(state.Assets, state.Registry.Table(), "Nanite Asset Upload");
    state.ReadyCount = uint32_t(state.Registry.Table().size());
    state.Dirty = false;
}
bool Ready(uint32_t asset) { return asset < Data().ReadyCount; }
uint32_t AssetCount() { return uint32_t(Data().Registry.Table().size()); }
const std::vector<VirtualGeometryGPUAsset>& AssetTable() { return Data().Registry.Table(); }
GpuSceneBuffer& ClusterBuffer() { return Data().Clusters; }
GpuSceneBuffer& GroupBuffer() { return Data().Groups; }
GpuSceneBuffer& HierarchyIndexBuffer() { return Data().Hierarchy; }
GpuSceneBuffer& RootBuffer() { return Data().Roots; }
GpuSceneBuffer& VertexBuffer() { return Data().Vertices; }
GpuSceneBuffer& TriangleBuffer() { return Data().Triangles; }
GpuSceneBuffer& AssetBuffer() { return Data().Assets; }
uint64_t MemoryBytes()
{
    auto& state = Data();
    return state.Clusters.MemoryBytes() + state.Groups.MemoryBytes() + state.Hierarchy.MemoryBytes() +
        state.Roots.MemoryBytes() + state.Vertices.MemoryBytes() + state.Triangles.MemoryBytes() + state.Assets.MemoryBytes();
}
void Shutdown() { Data() = {}; }
}
