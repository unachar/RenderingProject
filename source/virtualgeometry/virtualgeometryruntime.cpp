#include "virtualgeometryruntime.h"
#include "virtualgeometrybuilder.h"
#include "virtualgeometryserializer.h"
#include "virtualgeometrypagecache.h"
#include "virtualgeometryvalidator.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <numeric>
#include <sstream>
#include <unordered_map>
#include <stdexcept>
#include <condition_variable>
#include <deque>
#include <thread>
#include <cstdlib>

namespace VirtualGeometryRuntime
{
namespace
{
using VertexKey = std::array<uint32_t, 12>;
VertexKey KeyOf(const VirtualGeometryVertex &vertex)
{
    VertexKey key;
    std::memcpy(key.data(), &vertex, sizeof(vertex));
    return key;
}
struct VertexHash
{
    size_t operator()(const VertexKey &key) const
    {
        uint64_t hash = 14695981039346656037ull;
        for (auto component : key)
        {
            hash ^= component;
            hash *= 1099511628211ull;
        }
        return static_cast<size_t>(hash);
    }
};
struct State
{
    std::mutex Mutex;
    Statistics Stats;
    std::unordered_map<std::string, std::weak_ptr<const RuntimeAsset>> Assets;
};
State &Data()
{
    static State state;
    return state;
}

uint64_t GeometryKey(std::span<const VirtualGeometryVertex> vertices, std::span<const uint32_t> indices,
                     uint32_t material, float part, const VGImportSettings& settings)
{
    uint64_t hash = 14695981039346656037ull;
    auto consume = [&](const void *data, size_t size)
    {
        const auto *bytes = static_cast<const uint8_t *>(data);
        for (size_t i = 0; i < size; ++i)
        {
            hash ^= bytes[i];
            hash *= 1099511628211ull;
        }
    };
    // Bump integration version when the fixed build settings or remap contract change.
    const uint32_t header[] = {VirtualGeometryConfig::AssetVersion, VirtualGeometryConfig::BuilderVersion, material};
    consume(header, sizeof(header));
    // Hash fields explicitly: compiler padding must never affect derived data identity.
    const auto config = settings.BuildConfig();
    const uint64_t settingsKey = VirtualGeometryImport::SettingsKey(settings);
    consume(&settingsKey, sizeof(settingsKey));
    const uint32_t limits[] = {config.MaxClusterVertices, config.MaxClusterTriangles,
        config.TargetClustersPerGroup, config.MaxHierarchyLevels};
    consume(limits, sizeof(limits));
    const float parameters[] = {config.SimplificationRatio, config.ConeWeight, config.NormalWeight,
        config.TexCoordWeight, config.ColorWeight, config.TargetRelativeError, config.BoundaryEpsilon};
    consume(parameters, sizeof(parameters));
    consume(&part, sizeof(part));
    const uint64_t counts[] = {vertices.size(), indices.size()};
    consume(counts, sizeof(counts));
    consume(vertices.data(), vertices.size_bytes());
    consume(indices.data(), indices.size_bytes());
    return hash;
}

Meshlets::Asset ConvertLeaves(const VirtualGeometryAsset &hierarchy,
                              std::span<const VirtualGeometryVertex> source)
{
    std::unordered_map<VertexKey, uint32_t, VertexHash> original;
    original.reserve(source.size());
    for (uint32_t i = 0; i < source.size(); ++i)
        original.try_emplace(KeyOf(source[i]), i);
    Meshlets::Asset output;
    output.Config = {64, 124, 32};
    for (const auto &cluster : hierarchy.Clusters)
    {
        if (cluster.Level != 0)
            continue; // Parents contain newly simplified vertices.
        if (cluster.VertexCount > 64 || cluster.TriangleCount > 126)
            throw std::runtime_error("VirtualGeometry cluster exceeds Mesh Shader limits");
        Meshlets::Meshlet meshlet{static_cast<uint32_t>(output.Vertices.size()), cluster.VertexCount,
                                  static_cast<uint32_t>(output.Primitives.size()), cluster.TriangleCount};
        for (uint32_t i = 0; i < cluster.VertexCount; ++i)
        {
            const auto found = original.find(KeyOf(hierarchy.Vertices.at(cluster.VertexOffset + i)));
            if (found == original.end())
                throw std::runtime_error("VirtualGeometry leaf vertex does not match source attributes");
            output.Vertices.push_back(found->second);
        }
        for (uint32_t i = 0; i < cluster.TriangleCount; ++i)
        {
            const size_t start = size_t(cluster.TriangleOffset) + size_t(i) * 3;
            uint32_t packed = 0;
            for (uint32_t j = 0; j < 3; ++j)
                packed |= uint32_t(hierarchy.TriangleIndices.at(start + j)) << (8 * j);
            output.Primitives.push_back(packed);
        }
        output.Meshlets.push_back(meshlet);
        output.Bounds.push_back(
            {{cluster.Bounds.Center[0], cluster.Bounds.Center[1], cluster.Bounds.Center[2]},
             cluster.Bounds.Radius,
             {cluster.Cone.Axis[0], cluster.Cone.Axis[1], cluster.Cone.Axis[2]},
             cluster.Cone.Cutoff});
    }
    std::vector<Meshlets::Position> positions;
    positions.reserve(source.size());
    for (const auto &vertex : source)
        positions.push_back({vertex.Position[0], vertex.Position[1], vertex.Position[2]});
    // Preserve VirtualGeometry's clustering, but compute bounds/cones with the
    // exact float arithmetic and sphere-centered contract of the current GPU culler.
    Meshlets::RefreshMeshletBounds(output, positions);
    // Preserve conservative cluster spheres using union AABBs of child spheres.
    for (uint32_t first = 0; first < output.Meshlets.size(); first += 32)
    {
        const uint32_t count = std::min(32u, static_cast<uint32_t>(output.Meshlets.size()) - first);
        float lower[3] = {INFINITY, INFINITY, INFINITY}, upper[3] = {-INFINITY, -INFINITY, -INFINITY};
        for (uint32_t i = first; i < first + count; ++i)
        {
            const auto &bounds = output.Bounds[i];
            const float center[] = {bounds.Center.X, bounds.Center.Y, bounds.Center.Z};
            for (uint32_t axis = 0; axis < 3; ++axis)
            {
                lower[axis] = std::min(lower[axis], center[axis] - bounds.Radius);
                upper[axis] = std::max(upper[axis], center[axis] + bounds.Radius);
            }
        }
        Meshlets::Cluster cluster;
        cluster.MeshletOffset = first;
        cluster.MeshletCount = count;
        cluster.Bounds.Center = {(lower[0] + upper[0]) * 0.5f, (lower[1] + upper[1]) * 0.5f,
                                 (lower[2] + upper[2]) * 0.5f};
        const float x = (upper[0] - lower[0]) * 0.5f, y = (upper[1] - lower[1]) * 0.5f,
                    z = (upper[2] - lower[2]) * 0.5f;
        cluster.Bounds.Radius = std::sqrt(x * x + y * y + z * z) + 1e-5f;
        output.Clusters.push_back(cluster);
    }
    std::string error;
    if (!Meshlets::Validate(output, positions, error))
        throw std::runtime_error(error);
    return output;
}
} // namespace

std::shared_ptr<const RuntimeAsset> Acquire(std::span<const VirtualGeometryVertex> vertices,
                                            std::span<const uint32_t> indices, uint32_t materialId,
                                            float materialPartId, const std::filesystem::path &cacheDirectory,
                                            const VGImportSettings& settings)
{
    auto &state = Data();
    std::unique_lock lock(state.Mutex);
    auto asset = std::make_shared<RuntimeAsset>();
    asset->ImportSettings = settings;
    asset->Key = GeometryKey(vertices, indices, materialId, materialPartId, settings);
    std::ostringstream name;
    name << std::hex << asset->Key << ".vgmesh";
    asset->CachePath = cacheDirectory / name.str();
    const std::string identity = asset->CachePath.generic_string();
    if (auto found = state.Assets.find(identity); found != state.Assets.end())
        if (auto shared = found->second.lock())
        {
            ++state.Stats.SharedHits;
            return shared;
        }
    const auto begin = std::chrono::steady_clock::now();
    lock.unlock(); // Long CPU builds must not block inspector/statistics reads.
    try
    {
        if (!settings.Enabled)
        {
            throw std::runtime_error("VirtualGeometry is disabled for this asset");
        }
        auto config = settings.BuildConfig();
        float lower[3] = {INFINITY, INFINITY, INFINITY};
        float upper[3] = {-INFINITY, -INFINITY, -INFINITY};
        for (const auto& vertex : vertices)
        {
            for (uint32_t axis = 0; axis < 3; ++axis)
            {
                lower[axis] = std::min(lower[axis], vertex.Position[axis]);
                upper[axis] = std::max(upper[axis], vertex.Position[axis]);
            }
        }
        const float extent = std::max({upper[0]-lower[0], upper[1]-lower[1], upper[2]-lower[2]});
        asset->CompressionSettings = settings.Compression(std::isfinite(extent) ? extent : 1.0f);
        std::string error;
        asset->CacheHit = VirtualGeometrySerializer::Load(asset->CachePath, asset->Hierarchy, error);
        if (asset->CacheHit)
        {
            const auto &loaded = asset->Hierarchy;
            asset->CacheHit = loaded.Config.MaxClusterVertices == config.MaxClusterVertices &&
                              loaded.Config.MaxClusterTriangles == config.MaxClusterTriangles && loaded.DebugName == name.str() &&
                              loaded.SourceMaterialId == materialId &&
                              loaded.SourceMaterialPartId == materialPartId &&
                              loaded.Stats.SourceVertices == vertices.size() &&
                              loaded.Stats.SourceTriangles == indices.size() / 3;
            // Validate leaf/source correspondence before accepting a cached asset.
            if (asset->CacheHit)
            {
                try
                {
                    asset->Meshlets = ConvertLeaves(loaded, vertices);
                }
                catch (const std::exception &)
                {
                    asset->CacheHit = false;
                }
            }
        }
        if (!asset->CacheHit)
        {
            auto result = VirtualGeometryBuilder::Build(vertices, indices, config, materialId);
            if (!result)
            {
                // Some imported boundary/seam topologies cannot be simplified safely.
                // Still register their exact finest clusters instead of rejecting Nanite.
                asset->HierarchyWarning = result.Error;
                config.MaxHierarchyLevels = 1;
                result = VirtualGeometryBuilder::Build(vertices, indices, config, materialId);
            }
            if (!result)
                throw std::runtime_error(result.Error);
            asset->Hierarchy = std::move(result.Asset);
            asset->Hierarchy.DebugName = name.str();
            asset->Hierarchy.SourceMaterialPartId = materialPartId;
            asset->Meshlets = ConvertLeaves(asset->Hierarchy, vertices);
            // Cache failure is non-fatal: the validated in-memory asset remains drawable.
            std::error_code directoryError;
            std::filesystem::create_directories(cacheDirectory, directoryError);
            if (!directoryError)
                VirtualGeometrySerializer::Save(asset->CachePath, asset->Hierarchy, error);
        }
        else
        {
            if (asset->Hierarchy.Config.MaxHierarchyLevels == 1)
                asset->HierarchyWarning =
                    "Cached leaf-only hierarchy; protected boundaries prevented simplification";
        }
        // Finish the import product even when the asset has no scene instances yet.
        auto compression = asset->CompressionSettings;
        if (const char* raw = std::getenv("DX12_VG_COMPRESSION_RAW"))
        {
            compression.Enabled = std::atoi(raw) == 0;
        }
        auto pagePath = asset->CachePath;
        pagePath += "."+std::to_string(VirtualGeometryCompression::SettingsKey(compression));
        pagePath += compression.Enabled ? ".encoded.vgmesh" : ".raw.vgmesh";
        auto manifest = std::make_shared<VirtualGeometryPageCache::PageManifest>();
        if (!VirtualGeometryPageCache::Open(pagePath,*manifest,error) ||
            manifest->Version != VirtualGeometryPageCache::FormatVersion::Production)
        {
            if (!VirtualGeometrySerializer::SaveEncoded(pagePath,asset->Hierarchy,error,compression) ||
                !VirtualGeometryPageCache::Open(pagePath,*manifest,error))
            {
                // Preserve validated raw geometry when disk space or page encoding is unavailable.
                asset->HierarchyWarning += " Page preparation deferred: "+error;
                manifest.reset();
            }
        }
        if (manifest)
        {
            manifest->StreamingPriority = settings.StreamingPriority;
            asset->Pages = std::move(manifest);
        }
    }
    catch (const std::exception &error)
    {
        asset->Error = error.what();
        asset->Meshlets = {};
    }
    asset->BuildMilliseconds =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    lock.lock();
    if (!asset->Error.empty())
    {
        ++state.Stats.Failures;
    }
    else if (asset->CacheHit)
    {
        ++state.Stats.CacheHits;
    }
    else
    {
        ++state.Stats.Builds;
    }
    state.Stats.BuildMilliseconds += asset->BuildMilliseconds;
    for (auto entry = state.Assets.begin(); entry != state.Assets.end();)
        if (entry->second.expired())
            entry = state.Assets.erase(entry);
        else
            ++entry;
    state.Assets[identity] = asset;
    return asset;
}

std::shared_future<std::shared_ptr<const RuntimeAsset>> AcquireAsync(
    std::vector<VirtualGeometryVertex> vertices, std::vector<uint32_t> indices,
    uint32_t materialId, float materialPartId, const VGImportSettings& settings)
{
    // A single worker bounds CPU/memory competition with import and page encoding.
    struct BuildQueue
    {
        struct Job
        {
            uint64_t Bytes = 0;
            std::packaged_task<std::shared_ptr<const RuntimeAsset>()> Work;
        };
        std::mutex Mutex;
        std::condition_variable Wake;
        std::deque<Job> Jobs;
        uint64_t Bytes = 0;
        bool Stopping = false;
        std::thread Worker;
        BuildQueue() : Worker([this]
        {
            for (;;)
            {
                std::unique_lock guard(Mutex);
                Wake.wait(guard, [this] { return Stopping || !Jobs.empty(); });
                if (Jobs.empty())
                {
                    return;
                }
                auto job = std::move(Jobs.front());
                Jobs.pop_front();
                guard.unlock();
                job.Work();
                guard.lock();
                Bytes -= job.Bytes;
            }
        }) {}
        ~BuildQueue()
        {
            {
                std::lock_guard guard(Mutex);
                Stopping = true;
                Jobs.clear(); // Broken promises are handled by the main-thread publication path.
            }
            Wake.notify_one();
            Worker.join();
        }
    };
    Data(); // Runtime state must outlive the worker during process shutdown.
    static BuildQueue queue;
    const uint64_t bytes = uint64_t(vertices.size())*sizeof(VirtualGeometryVertex) + uint64_t(indices.size())*4;
    std::unique_lock guard(queue.Mutex);
    constexpr uint64_t queueBudget = 512ull*1024*1024;
    if (bytes > queueBudget || queue.Bytes > queueBudget-bytes)
    {
        std::promise<std::shared_ptr<const RuntimeAsset>> rejected;
        auto asset = std::make_shared<RuntimeAsset>();
        asset->Error = "VirtualGeometry background queue memory budget exceeded; legacy rendering retained";
        rejected.set_value(asset);
        return rejected.get_future().share();
    }
    BuildQueue::Job job;
    job.Bytes = bytes;
    job.Work = std::packaged_task<std::shared_ptr<const RuntimeAsset>()>(
        [vertices = std::move(vertices), indices = std::move(indices), materialId, materialPartId, settings]
        {
            return Acquire(vertices, indices, materialId, materialPartId, "asset/virtualgeometry-cache", settings);
        });
    auto future = job.Work.get_future().share();
    queue.Bytes += bytes;
    queue.Jobs.push_back(std::move(job));
    guard.unlock();
    queue.Wake.notify_one();
    return future;
}
Statistics GetStatistics()
{
    auto &state = Data();
    std::lock_guard lock(state.Mutex);
    return state.Stats;
}
std::vector<std::shared_ptr<const RuntimeAsset>> LiveAssets()
{
    auto &state = Data();
    std::lock_guard lock(state.Mutex);
    std::vector<std::shared_ptr<const RuntimeAsset>> result;
    for (const auto &[key, entry] : state.Assets)
        if (auto asset = entry.lock())
            result.push_back(std::move(asset));
    std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) { return a->Key < b->Key; });
    return result;
}
} // namespace VirtualGeometryRuntime
