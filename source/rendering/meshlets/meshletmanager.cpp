#include "pch.h"
#include "meshletmanager.h"
#include "virtualgeometry/virtualgeometrybuilder.h"
#include "staticmodel.h"
#include "graphicsdevice.h"
#include "graphicslog.h"
#include "gpuscene/gpuscene.h"
#include "gpudriven/gpudrivenrenderer.h"
#include "visibility/gpuvisibility.h"
#include <unordered_set>
#include <sstream>
#include <numeric>
#include <unordered_map>

namespace MeshletManager
{
namespace
{
struct State
{
    struct Entry
    {
        Meshlets::Asset Asset;
        uint32_t BaseVertex = 0;
    };

    unordered_map<uint32_t, Entry> Entries;
    Meshlets::Asset Data;
    vector<Meshlets::Range> Ranges;
    unordered_map<uint32_t, Meshlets::Range> DisabledRanges;
    GpuSceneBuffer Meshlets, Vertices, Primitives, Bounds, Clusters, Range;
    bool Dirty = false;
    bool Rebuild = false;
    uint32_t PreviousActive = UINT32_MAX;
};

State& Data()
{
    static State state;
    return state;
}
} // namespace

void BuildImportedMesh(StaticMeshData& mesh)
{
    mesh.VirtualGeometryLODs = {};
    mesh.MeshletLODs = {};
    mesh.VirtualGeometryBuilds = {};
    mesh.VirtualGeometryBuildError.clear();
    if (!mesh.VirtualGeometrySettings.Enabled)
    {
        return;
    }
    std::string eligibilityReason;
    if (!CanUseVirtualGeometry(mesh, eligibilityReason))
    {
        mesh.VirtualGeometryBuildError = eligibilityReason;
        WriteGraphicsLog("VirtualGeometry import fallback (%s): %s\n", mesh.MeshName.c_str(),
                         eligibilityReason.c_str());
        return;
    }
    try
    {
        vector<VirtualGeometryVertex> vertices;
        vertices.reserve(mesh.CpuVertices.size());
        for (const auto& v : mesh.CpuVertices)
        {
            vertices.push_back({{v.Position.x, v.Position.y, v.Position.z},
                                {v.Normal.x, v.Normal.y, v.Normal.z},
                                {v.TexCoord.x, v.TexCoord.y},
                                {v.Diffuse.x, v.Diffuse.y, v.Diffuse.z, v.Diffuse.w}});
        }
        for (uint32_t lod = 0; lod < StaticMeshData::LodCount; ++lod)
        {
            const auto& indices = lod ? mesh.CpuLodIndices[lod - 1] : mesh.CpuIndices;
            if (indices.empty())
            {
                continue;
            }
            if (mesh.VirtualGeometrySettings.BackgroundBuild)
            {
                if (lod != 0)
                {
                    continue; // Native hierarchy replaces discrete LODs; legacy indices remain available.
                }
                mesh.VirtualGeometryBuilds[lod] = VirtualGeometryRuntime::AcquireAsync(
                    std::move(vertices), indices, 0, mesh.AppliedMaterialPartId, mesh.VirtualGeometrySettings);
                continue;
            }
            auto asset = VirtualGeometryRuntime::Acquire(vertices, indices, 0, mesh.AppliedMaterialPartId,
                                                         "asset/virtualgeometry-cache", mesh.VirtualGeometrySettings);
            mesh.VirtualGeometryLODs[lod] = asset;
            if (asset->Error.empty())
            {
                mesh.MeshletLODs[lod] = asset->Meshlets;
            }
            else
            {
                mesh.VirtualGeometryBuildError = asset->Error;
                WriteGraphicsLog("Nanite import fallback (%s LOD%u): %s\n", mesh.MeshName.c_str(), lod,
                                 asset->Error.c_str());
            }
        }
    }
    catch (const exception& error)
    {
        mesh.VirtualGeometryLODs = {};
        mesh.MeshletLODs = {};
        mesh.VirtualGeometryBuildError = error.what();
        WriteGraphicsLog("Meshlet import fallback: %s\n", error.what());
    }
}

bool PollImportedMesh(StaticMeshData& mesh)
{
    bool changed = false;
    for (uint32_t lod = 0; lod < StaticMeshData::LodCount; ++lod)
    {
        auto& pending = mesh.VirtualGeometryBuilds[lod];
        if (!pending.valid() || pending.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        {
            continue;
        }
        try
        {
            const auto asset = pending.get();
            if (asset && asset->Error.empty())
            {
                mesh.VirtualGeometryLODs[lod] = asset;
                mesh.MeshletLODs[lod] = asset->Meshlets;
            }
            else
            {
                mesh.VirtualGeometryBuildError = asset ? asset->Error : "No VirtualGeometry build result";
            }
        }
        catch (const std::exception& error)
        {
            mesh.VirtualGeometryBuildError = error.what();
        }
        pending = {};
        changed = true;
    }
    if (changed)
    {
        GPUDrivenRenderer::UpdateGeometry(mesh);
    }
    return changed;
}

void Register(uint32_t meshID, const Meshlets::Asset& asset, uint32_t baseVertex)
{
    auto& state = Data();
    if (state.Ranges.size() <= meshID)
    {
        state.Ranges.resize(size_t(meshID) + 1);
        state.Dirty = true; // Upload new zero ranges even when a legacy asset has no meshlets.
    }
    if (state.DisabledRanges.contains(meshID) || state.Ranges[meshID].MeshletCount || asset.Meshlets.empty())
    {
        return;
    }
    state.Entries[meshID] = {asset, baseVertex};
    auto& data = state.Data;
    const uint64_t limits[] = {
        data.Meshlets.size() + asset.Meshlets.size(), data.Vertices.size() + asset.Vertices.size(),
        data.Primitives.size() + asset.Primitives.size(), data.Clusters.size() + asset.Clusters.size()};
    for (auto limit : limits)
    {
        if (limit > UINT32_MAX)
        {
            throw overflow_error("Global meshlet range overflow");
        }
    }
    Meshlets::Range range{static_cast<uint32_t>(data.Meshlets.size()), static_cast<uint32_t>(asset.Meshlets.size()),
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
        {
            throw overflow_error("Meshlet global vertex overflow");
        }
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

void Replace(uint32_t meshID, const Meshlets::Asset& asset, uint32_t baseVertex)
{
    auto& state = Data();
    state.DisabledRanges.erase(meshID);
    state.Entries[meshID] = {asset, baseVertex};
    state.Rebuild = true;
    state.Dirty = true;
}

void Invalidate(uint32_t meshID)
{
    if (meshID < Data().Ranges.size())
    {
        if (Data().Ranges[meshID].MeshletCount)
        {
            Data().DisabledRanges[meshID] = Data().Ranges[meshID];
        }
        Data().Ranges[meshID] = {};
        Data().Dirty = true;
    }
}

void RestoreForValidation(uint32_t meshID)
{
    auto& state = Data();
    const auto found = state.DisabledRanges.find(meshID);
    if (found != state.DisabledRanges.end())
    {
        state.DisabledRanges.erase(found);
        state.Rebuild = true;
        state.Dirty = true;
    }
}

void Prepare(RenderGraph& graph, uint32_t meshes)
{
    auto& state = Data();
    if (state.Dirty || state.PreviousActive != GpuScene::Statistics().ActiveInstances)
    {
        state.PreviousActive = GpuScene::Statistics().ActiveInstances;
        unordered_set<uint32_t> used, groups;
        for (uint32_t slot = 0; slot < GpuScene::Statistics().InstanceSlots; ++slot)
        {
            const auto* instance = GpuScene::GetInstance(slot);
            if (!instance || !(instance->Flags & 1u))
            {
                continue;
            }
            used.insert(instance->MeshIndex);
            groups.insert(instance->LODGroupIndex);
        }
        for (auto group : groups)
        {
            for (auto id : GPUVisibility::LODMeshes(group))
            {
                used.insert(id);
            }
        }
        for (auto it = state.Entries.begin(); it != state.Entries.end();)
        {
            if (!used.contains(it->first))
            {
                state.DisabledRanges.erase(it->first);
                it = state.Entries.erase(it);
                state.Rebuild = true;
            }
            else
            {
                ++it;
            }
        }
    }
    if (state.Rebuild)
    {
        // Compact once per frame after all edits. Keep GPU buffers alive; their upload
        // resources and resized allocations retain the existing frame/fence lifetime.
        auto entries = move(state.Entries);
        state.Data = {};
        state.Ranges.assign(max(size_t(meshes), state.Ranges.size()), {});
        vector<uint32_t> ids;
        for (const auto& [id, entry] : entries)
        {
            ids.push_back(id);
        }
        sort(ids.begin(), ids.end());
        for (auto id : ids)
        {
            const auto& entry = entries.at(id);
            if (!state.DisabledRanges.contains(id))
            {
                Register(id, entry.Asset, entry.BaseVertex);
            }
        }
        // Disabled assets retain source data for validation restore.
        for (auto id : ids)
        {
            if (state.DisabledRanges.contains(id))
            {
                state.Entries[id] = move(entries.at(id));
            }
        }
        state.Rebuild = false;
        state.Dirty = true;
    }
    if (state.Ranges.size() < meshes)
    {
        state.Ranges.resize(meshes);
        state.Dirty = true;
    }
    const uint32_t frame = GraphicsDevice::GetFrameIndex();
    auto upload = [&](GpuSceneBuffer& buffer, const auto& values, const char* name) {
        using Element = typename decay_t<decltype(values)>::value_type;
        buffer.BeginFrame(frame);
        bool resized = buffer.Ensure(GraphicsDevice::GetDevice(), static_cast<uint32_t>(values.size()), sizeof(Element),
                                     frame, L"Global Meshlet Asset");
        vector<uint32_t> changes;
        if (state.Dirty || resized)
        {
            changes.resize(values.size());
            iota(changes.begin(), changes.end(), 0u);
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
            auto* target = &buffer;
            graph.AddPass(
                name,
                [=](RenderGraph::PassBuilder& b) {
                    b.Write(handle, D3D12_RESOURCE_STATE_COPY_DEST);
                },
                [=](ID3D12GraphicsCommandList* list) {
                    target->Copy(list, frame);
                });
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

const vector<Meshlets::Range>& Ranges()
{
    return Data().Ranges;
}

const Meshlets::Asset& AssetData()
{
    return Data().Data;
}

GpuSceneBuffer& MeshletBuffer()
{
    return Data().Meshlets;
}

GpuSceneBuffer& VertexBuffer()
{
    return Data().Vertices;
}

GpuSceneBuffer& PrimitiveBuffer()
{
    return Data().Primitives;
}

GpuSceneBuffer& BoundsBuffer()
{
    return Data().Bounds;
}

GpuSceneBuffer& ClusterBuffer()
{
    return Data().Clusters;
}

GpuSceneBuffer& RangeBuffer()
{
    return Data().Range;
}

uint64_t MemoryBytes()
{
    auto& s = Data();
    return s.Meshlets.MemoryBytes() + s.Vertices.MemoryBytes() + s.Primitives.MemoryBytes() + s.Bounds.MemoryBytes() +
           s.Clusters.MemoryBytes() + s.Range.MemoryBytes();
}

void Shutdown()
{
    Data() = {};
}
} // namespace MeshletManager
