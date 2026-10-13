#include "pch.h"
#include "meshletmanager.h"
#include "staticmodel.h"
#include "graphicsdevice.h"
#include "graphicslog.h"
#include <sstream>
#include <numeric>
#include <unordered_map>

namespace MeshletManager
{
namespace
{
struct State
{
    Meshlets::Asset Data;
    std::vector<Meshlets::Range> Ranges;
    std::unordered_map<uint32_t, Meshlets::Range> DisabledRanges;
    GpuSceneBuffer Meshlets, Vertices, Primitives, Bounds, Clusters, Range;
    bool Dirty = false;
};
State &Data()
{
    static State state;
    return state;
}
} // namespace
void BuildImportedMesh(StaticMeshData &mesh, Meshlets::BuildConfig config)
{
    try
    {
        std::vector<Meshlets::Position> positions;
        positions.reserve(mesh.CpuVertices.size());
        for (const auto &v : mesh.CpuVertices)
            positions.push_back({v.Position.x, v.Position.y, v.Position.z});
        for (uint32_t lod = 0; lod < StaticMeshData::LodCount; ++lod)
        {
            const auto &indices = lod ? mesh.CpuLodIndices[lod - 1] : mesh.CpuIndices;
            if (indices.empty())
                continue;
            const auto key = Meshlets::GeometryKey(positions, indices, config);
            std::ostringstream name;
            name << "asset/meshlet-cache/" << std::hex << key << ".meshlets";
            auto &asset = mesh.MeshletLODs[lod];
            if (!Meshlets::Load(name.str(), asset, key, positions))
            {
#if defined(NDEBUG)
                // Shipping loads prebuilt data; missing or invalid assets use the traditional path.
                asset = {};
                continue;
#else
                asset = Meshlets::Build(positions, indices, config);
                if (!Meshlets::Save(name.str(), asset, key))
                    WriteGraphicsLog("Meshlet asset save failed: %s\n", name.str().c_str());
#endif
            }
        }
    }
    catch (const std::exception &error)
    {
        mesh.MeshletLODs = {};
        WriteGraphicsLog("Meshlet import fallback: %s\n", error.what());
    }
}
void Register(uint32_t meshID, const Meshlets::Asset &asset, uint32_t baseVertex)
{
    auto &state = Data();
    if (state.Ranges.size() <= meshID)
        state.Ranges.resize(size_t(meshID) + 1);
    if (state.Ranges[meshID].MeshletCount || asset.Meshlets.empty())
        return;
    auto &data = state.Data;
    const uint64_t limits[] = {
        data.Meshlets.size() + asset.Meshlets.size(), data.Vertices.size() + asset.Vertices.size(),
        data.Primitives.size() + asset.Primitives.size(), data.Clusters.size() + asset.Clusters.size()};
    for (auto limit : limits)
        if (limit > UINT32_MAX)
            throw std::overflow_error("Global meshlet range overflow");
    Meshlets::Range range{
        static_cast<uint32_t>(data.Meshlets.size()), static_cast<uint32_t>(asset.Meshlets.size()),
        static_cast<uint32_t>(data.Clusters.size()), static_cast<uint32_t>(asset.Clusters.size())};
    uint32_t vertexOffset = static_cast<uint32_t>(data.Vertices.size()),
             primitiveOffset = static_cast<uint32_t>(data.Primitives.size());
    for (auto meshlet : asset.Meshlets)
    {
        meshlet.VertexOffset += vertexOffset;
        meshlet.PrimitiveOffset += primitiveOffset;
        data.Meshlets.push_back(meshlet);
    }
    for (auto vertex : asset.Vertices)
    {
        if (uint64_t(baseVertex) + vertex > UINT32_MAX)
            throw std::overflow_error("Meshlet global vertex overflow");
        data.Vertices.push_back(baseVertex + vertex);
    }
    data.Primitives.insert(data.Primitives.end(), asset.Primitives.begin(), asset.Primitives.end());
    data.Bounds.insert(data.Bounds.end(), asset.Bounds.begin(), asset.Bounds.end());
    for (auto cluster : asset.Clusters)
    {
        cluster.MeshletOffset += range.MeshletOffset;
        data.Clusters.push_back(cluster);
    }
    state.Ranges[meshID] = range;
    state.Dirty = true;
}
void Invalidate(uint32_t meshID)
{
    if (meshID < Data().Ranges.size())
    {
        if (Data().Ranges[meshID].MeshletCount)
            Data().DisabledRanges[meshID] = Data().Ranges[meshID];
        Data().Ranges[meshID] = {};
        Data().Dirty = true;
    }
}
void RestoreForValidation(uint32_t meshID)
{
    auto &state = Data();
    const auto found = state.DisabledRanges.find(meshID);
    if (found != state.DisabledRanges.end())
    {
        state.Ranges.at(meshID) = found->second;
        state.DisabledRanges.erase(found);
        state.Dirty = true;
    }
}
void Prepare(RenderGraph &graph, uint32_t meshes)
{
    auto &state = Data();
    state.Ranges.resize((std::max)(state.Ranges.size(), size_t(meshes)));
    const uint32_t frame = GraphicsDevice::GetFrameIndex();
    auto upload = [&](GpuSceneBuffer &buffer, const auto &values, const char *name) {
        using Element = typename std::decay_t<decltype(values)>::value_type;
        buffer.BeginFrame(frame);
        bool resized = buffer.Ensure(GraphicsDevice::GetDevice(), static_cast<uint32_t>(values.size()),
                                     sizeof(Element), frame, L"Global Meshlet Asset");
        std::vector<uint32_t> changes;
        if (state.Dirty || resized)
        {
            changes.resize(values.size());
            std::iota(changes.begin(), changes.end(), 0u);
        }
        uint64_t bytes = buffer.Stage(GraphicsDevice::GetDevice(), frame, values.data(),
                                      static_cast<uint32_t>(values.size()), changes);
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
            auto *target = &buffer;
            graph.AddPass(
                name, [=](RenderGraph::PassBuilder &b) { b.Write(handle, D3D12_RESOURCE_STATE_COPY_DEST); },
                [=](ID3D12GraphicsCommandList *list) { target->Copy(list, frame); });
        }
    };
    upload(state.Meshlets, state.Data.Meshlets, "Global Meshlet Upload");
    upload(state.Vertices, state.Data.Vertices, "Meshlet Remap Upload");
    upload(state.Primitives, state.Data.Primitives, "Meshlet Primitive Upload");
    upload(state.Bounds, state.Data.Bounds, "Meshlet Bounds Upload");
    upload(state.Clusters, state.Data.Clusters, "Cluster Upload");
    upload(state.Range, state.Ranges, "Meshlet Range Upload");
    state.Dirty = false;
}
const std::vector<Meshlets::Range> &Ranges()
{
    return Data().Ranges;
}
const Meshlets::Asset &AssetData()
{
    return Data().Data;
}
GpuSceneBuffer &MeshletBuffer()
{
    return Data().Meshlets;
}
GpuSceneBuffer &VertexBuffer()
{
    return Data().Vertices;
}
GpuSceneBuffer &PrimitiveBuffer()
{
    return Data().Primitives;
}
GpuSceneBuffer &BoundsBuffer()
{
    return Data().Bounds;
}
GpuSceneBuffer &ClusterBuffer()
{
    return Data().Clusters;
}
GpuSceneBuffer &RangeBuffer()
{
    return Data().Range;
}
uint64_t MemoryBytes()
{
    auto &s = Data();
    return s.Meshlets.MemoryBytes() + s.Vertices.MemoryBytes() + s.Primitives.MemoryBytes() +
           s.Bounds.MemoryBytes() + s.Clusters.MemoryBytes() + s.Range.MemoryBytes();
}
void Shutdown()
{
    Data() = {};
}
} // namespace MeshletManager
