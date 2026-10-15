#include "pch.h"
#include "virtualgeometryresources.h"
#include "graphicsdevice.h"
#include "graphicslog.h"
#include "virtualgeometrystreaming.h"
#include "virtualgeometryruntime.h"
#include "materialmanager.h"
#include <future>
#include <numeric>

namespace VirtualGeometryResources
{
namespace
{
struct State
{
    struct Prepared
    {
        std::shared_ptr<VirtualGeometryPageCache::PageManifest> Manifest;
        std::string Error;
    };
    struct Pending
    {
        std::shared_ptr<const VirtualGeometryRuntime::RuntimeAsset> Owner;
        std::future<Prepared> Work;
        bool Finished = false;
    };
    VirtualGeometryGPU::PackedAsset Geometry;
    std::vector<VirtualGeometryGPUAsset> Table;
    std::vector<uint64_t> SourceTriangles;
    std::vector<std::shared_ptr<const VirtualGeometryPageCache::PageManifest>> Manifests;
    std::vector<Pending> PendingAssets;
    std::unordered_map<const VirtualGeometryRuntime::RuntimeAsset*, uint32_t> Ids;
    GpuSceneBuffer Clusters, Groups, Hierarchy, Roots, Vertices, Triangles, Assets;
    bool Dirty = true;
};
State& Data()
{
    static State state;
    return state;
}
bool Install(uint32_t id, std::shared_ptr<const VirtualGeometryPageCache::PageManifest> manifest)
{
    auto& state = Data();
    VirtualGeometryGPUAsset table;
    std::string error;
    if (!manifest || !VirtualGeometryStreaming::CanRegister(*manifest))
    {
        WriteGraphicsLog("Geometry Page registration refused before metadata append\n");
        return false;
    }
    const std::array<size_t,4> original = {state.Geometry.Clusters.size(),state.Geometry.Groups.size(),
        state.Geometry.HierarchyIndices.size(),state.Geometry.Roots.size()};
    if (!VirtualGeometryPages::Validate(manifest->Metadata, manifest->PinnedPages, error) ||
        !VirtualGeometryGPU::Append(manifest->Metadata, state.Geometry, table, error))
    {
        WriteGraphicsLog("Geometry Page metadata registration failed: %s\n", error.c_str());
        return false;
    }
    const auto& owner = state.PendingAssets.at(id).Owner;
    const uint32_t priority = owner ? owner->ImportSettings.StreamingPriority : manifest->StreamingPriority;
    uint32_t pageBase = VirtualGeometryStreaming::Register(manifest,priority);
    if (pageBase == UINT32_MAX)
    {
        WriteGraphicsLog("Geometry Page admission failed: pinned pages exceed budget or device unsupported\n");
        state.Geometry.Clusters.resize(original[0]);
        state.Geometry.Groups.resize(original[1]);
        state.Geometry.HierarchyIndices.resize(original[2]);
        state.Geometry.Roots.resize(original[3]);
        return false;
    }
    // Flag 2 changes these legacy geometry offsets to Page base/count.
    table.VertexOffset = pageBase;
    table.TriangleOffset = uint32_t(manifest->Pages.size());
    table.Flags = (table.Flags | 2u) & ~1u;
    table.Flags |= std::min(2u, priority)<<16;
    state.Table[id] = table;
    state.SourceTriangles.resize(state.Table.size());
    state.SourceTriangles[id] = 0;
    for (const auto& cluster : manifest->Metadata.Clusters)
    {
        if (cluster.Level == 0)
        {
            state.SourceTriangles[id] += cluster.TriangleCount;
        }
    }
    state.Manifests[id] = std::move(manifest);
    if (state.Manifests[id]->SourceTriangles)
    {
        state.SourceTriangles[id] = state.Manifests[id]->SourceTriangles;
    }
    std::string registryError;
    MaterialManager::Registry().Register(state.Manifests[id]->Path, AssetType::Mesh, registryError);
    state.Dirty = true;
    return true;
}
void PollPreparation()
{
    auto& state = Data();
    uint32_t running = 0;
    for (uint32_t id = 0; id < state.PendingAssets.size(); ++id)
    {
        auto& pending = state.PendingAssets[id];
        if (!pending.Work.valid())
        {
            continue;
        }
        if (pending.Work.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        {
            ++running;
            continue;
        }
        auto result = pending.Work.get();
        pending.Finished = true;
        if (result.Manifest)
        {
            Install(id, std::move(result.Manifest));
        }
        else
        {
            WriteGraphicsLog("Geometry Page preparation fallback: %s\n", result.Error.c_str());
        }
    }
    for (uint32_t id = 0; id < state.PendingAssets.size() && running < 2; ++id)
    {
        auto& pending = state.PendingAssets[id];
        if (pending.Finished || pending.Work.valid() || !pending.Owner)
        {
            continue;
        }
        if (pending.Owner->Pages)
        {
            Install(id,pending.Owner->Pages);
            pending.Finished = true;
            continue;
        }
        auto asset = pending.Owner;
        auto compressionSettings = asset->CompressionSettings;
        char rawMode[8]{};
        if (GetEnvironmentVariableA("DX12_VG_COMPRESSION_RAW", rawMode, sizeof(rawMode)))
        {
            compressionSettings.Enabled = atoi(rawMode) == 0;
        }
        pending.Work = std::async(std::launch::async, [asset, compressionSettings]
        {
            State::Prepared result;
            try
            {
                auto path = asset->CachePath;
                if (path.empty())
                {
                    path = std::filesystem::path("asset/virtualgeometry-cache") / std::to_string(asset->Key);
                }
                // Stable content-keyed encoded cache; never overwrite the legacy CPU reference asset.
                path += "." + std::to_string(VirtualGeometryCompression::SettingsKey(compressionSettings));
                if (compressionSettings.Enabled)
                {
                    path += ".encoded.vgmesh";
                }
                else
                {
                    path += ".raw.vgmesh";
                }
                auto manifest = std::make_shared<VirtualGeometryPageCache::PageManifest>();
                manifest->StreamingPriority = asset->ImportSettings.StreamingPriority;
                if (VirtualGeometryPageCache::Open(path, *manifest, result.Error) &&
                    manifest->Version == VirtualGeometryPageCache::FormatVersion::Production)
                {
                    manifest->StreamingPriority = asset->ImportSettings.StreamingPriority;
                    result.Manifest = std::move(manifest);
                    return result;
                }
                VirtualGeometryPages::PagedAsset packed;
                if (!VirtualGeometryPages::Build(asset->Hierarchy, compressionSettings, packed, result.Error) ||
                    !VirtualGeometryPageCache::WriteAtomic(path, packed, result.Error))
                {
                    return result;
                }
                for (const auto& reason : packed.CompressionFallbacks)
                {
                    WriteGraphicsLog("Geometry compression %s: %s\n", asset->Hierarchy.DebugName.c_str(), reason.c_str());
                }
                if (VirtualGeometryPageCache::Open(path, *manifest, result.Error))
                {
                    manifest->StreamingPriority = asset->ImportSettings.StreamingPriority;
                    result.Manifest = std::move(manifest);
                }
            }
            catch (const std::exception& exception)
            {
                result.Error = exception.what();
            }
            return result;
        });
        ++running;
    }
}
}
uint32_t Register(std::shared_ptr<const VirtualGeometryRuntime::RuntimeAsset> asset)
{
    auto& state = Data();
    if (!asset || !asset->Error.empty() || state.Table.size() >= UINT32_MAX)
    {
        return UINT32_MAX;
    }
    auto found = state.Ids.find(asset.get());
    if (found != state.Ids.end())
    {
        return found->second;
    }
    uint32_t id = uint32_t(state.Table.size());
    state.Table.emplace_back();
    state.Manifests.emplace_back();
    state.PendingAssets.emplace_back();
    state.PendingAssets.back().Owner = asset;
    state.Ids.emplace(asset.get(), id);
    state.Dirty = true;
    return id;
}
const std::vector<uint64_t>& SourceTriangleCounts() { return Data().SourceTriangles; }
std::shared_ptr<const VirtualGeometryPageCache::PageManifest> ManifestFor(const VirtualGeometryRuntime::RuntimeAsset* asset)
{
    const auto& state = Data();
    const auto found = state.Ids.find(asset);
    return found == state.Ids.end() ? nullptr : state.Manifests.at(found->second);
}
uint32_t RegisterManifest(std::shared_ptr<const VirtualGeometryPageCache::PageManifest> manifest)
{
    auto& state = Data();
    uint32_t id = uint32_t(state.Table.size());
    state.Table.emplace_back();
    state.Manifests.emplace_back();
    state.PendingAssets.emplace_back();
    state.PendingAssets.back().Finished = true;
    if (!Install(id, std::move(manifest)))
    {
        state.Table.pop_back();
        state.Manifests.pop_back();
        state.PendingAssets.pop_back();
        return UINT32_MAX;
    }
    return id;
}
void Prepare(RenderGraph& graph)
{
    auto& state = Data();
    const uint32_t frame = GraphicsDevice::GetFrameIndex();
    PollPreparation();
    VirtualGeometryStreaming::Prepare(graph);
    for (uint32_t id = 0; id < state.Table.size(); ++id)
    {
        if (state.Manifests[id] && (state.Table[id].Flags & 1u) == 0 &&
            VirtualGeometryStreaming::RootsReady(state.Table[id].VertexOffset, *state.Manifests[id]))
        {
            state.Table[id].Flags |= 1u;
            state.Dirty = true;
        }
    }
    const auto& geometry = state.Geometry;
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
    upload(state.Assets, state.Table, "Nanite Asset Upload");
    state.Dirty = false;
    VirtualGeometryStreaming::RecordMetadataBytes(MemoryBytes());
}
bool Ready(uint32_t asset) { return asset < Data().Table.size() && (Data().Table[asset].Flags & 1u) != 0; }
uint32_t AssetCount() { return uint32_t(Data().Table.size()); }
uint64_t RegisteredClusterCount() { return Data().Geometry.Clusters.size(); }
const std::vector<VirtualGeometryGPUAsset>& AssetTable() { return Data().Table; }
const VirtualGeometryGPU::PackedAsset& Metadata() { return Data().Geometry; }
VirtualGeometryCompression::Metrics CompressionMetrics()
{
    VirtualGeometryCompression::Metrics total;
    for (const auto& manifest : Data().Manifests)
    {
        if (!manifest)
        {
            continue;
        }
        const auto& metrics = manifest->Compression;
        total.EncodedBytes += metrics.EncodedBytes;
        total.RawBytes += metrics.RawBytes;
        total.FallbackCount += metrics.FallbackCount;
        total.PositionQuantized += metrics.PositionQuantized;
        total.HalfUV += metrics.HalfUV;
        total.Index8Clusters += metrics.Index8Clusters;
        total.Index16Clusters += metrics.Index16Clusters;
    }
    return total;
}
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
void Shutdown()
{
    VirtualGeometryStreaming::Shutdown();
    Data() = {};
}
}
