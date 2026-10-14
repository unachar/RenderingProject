#include "meshletbuilder.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <cstring>

namespace Meshlets
{
namespace
{
Position Sub(Position a, Position b)
{
    return {a.X - b.X, a.Y - b.Y, a.Z - b.Z};
}
Position Add(Position a, Position b)
{
    return {a.X + b.X, a.Y + b.Y, a.Z + b.Z};
}
Position Scale(Position a, float s)
{
    return {a.X * s, a.Y * s, a.Z * s};
}
float Dot(Position a, Position b)
{
    return a.X * b.X + a.Y * b.Y + a.Z * b.Z;
}
float Length(Position a)
{
    return sqrt(Dot(a, a));
}
Position Cross(Position a, Position b)
{
    return {a.Y * b.Z - a.Z * b.Y, a.Z * b.X - a.X * b.Z, a.X * b.Y - a.Y * b.X};
}
bool Finite(Position p)
{
    return isfinite(p.X) && isfinite(p.Y) && isfinite(p.Z);
}
bool ConfigValid(BuildConfig c)
{
    return c.MaxVertices >= 3 && c.MaxVertices <= 64 && c.MaxPrimitives >= 1 && c.MaxPrimitives <= 126 &&
           c.MeshletsPerCluster >= 1 && c.MeshletsPerCluster <= 256;
}
Bounds ComputeBounds(const Asset &asset, const Meshlet &meshlet, span<const Position> positions)
{
    Position minimum = positions[asset.Vertices[meshlet.VertexOffset]], maximum = minimum;
    for (uint32_t i = 0; i < meshlet.VertexCount; ++i)
    {
        auto p = positions[asset.Vertices[meshlet.VertexOffset + i]];
        minimum = {min(minimum.X, p.X), min(minimum.Y, p.Y), min(minimum.Z, p.Z)};
        maximum = {max(maximum.X, p.X), max(maximum.Y, p.Y), max(maximum.Z, p.Z)};
    }
    Bounds bounds;
    bounds.Center = Scale(Add(minimum, maximum), 0.5f);
    for (uint32_t i = 0; i < meshlet.VertexCount; ++i)
        bounds.Radius = max(
            bounds.Radius, Length(Sub(positions[asset.Vertices[meshlet.VertexOffset + i]], bounds.Center)));
    bounds.Radius = nextafter(bounds.Radius, numeric_limits<float>::infinity());
    vector<Position> normals;
    Position sum{};
    for (uint32_t i = 0; i < meshlet.PrimitiveCount; ++i)
    {
        uint32_t packed = asset.Primitives[meshlet.PrimitiveOffset + i];
        auto vertex = [&](uint32_t lane) {
            return positions[asset.Vertices[meshlet.VertexOffset + ((packed >> (lane * 8)) & 255)]];
        };
        auto n = Cross(Sub(vertex(1), vertex(0)), Sub(vertex(2), vertex(0)));
        float length = Length(n);
        if (length <= 1e-12f)
            continue;
        n = Scale(n, 1 / length);
        normals.push_back(n);
        sum = Add(sum, n);
    }
    if (Length(sum) > 1e-6f)
    {
        bounds.ConeAxis = Scale(sum, 1 / Length(sum));
        float cosine = 1;
        for (auto n : normals)
            cosine = min(cosine, Dot(n, bounds.ConeAxis));
        if (cosine > 0.001f)
            bounds.ConeCutoff = min(1.f, sqrt(max(0.f, 1 - cosine * cosine)) + 1e-5f);
    }
    return bounds;
}
struct Header
{
    uint32_t Magic = 0x354c534d, Version = 2;
    uint64_t Key = 0;
    uint64_t PayloadHash = 0;
    BuildConfig Config;
    uint32_t Counts[5]{};
};
uint64_t PayloadHash(const Asset &asset)
{
    uint64_t hash = 14695981039346656037ull;
    auto consume = [&](const auto &values) {
        const auto *bytes = reinterpret_cast<const uint8_t *>(values.data());
        for (size_t i = 0; i < values.size() * sizeof(values[0]); ++i)
        {
            hash ^= bytes[i];
            hash *= 1099511628211ull;
        }
    };
    consume(asset.Meshlets);
    consume(asset.Vertices);
    consume(asset.Primitives);
    consume(asset.Bounds);
    consume(asset.Clusters);
    return hash;
}
template <class T> bool Write(ofstream &stream, const vector<T> &data)
{
    stream.write(reinterpret_cast<const char *>(data.data()), streamsize(data.size() * sizeof(T)));
    return bool(stream);
}
template <class T> bool Read(ifstream &stream, vector<T> &data, uint32_t count)
{
    data.resize(count);
    stream.read(reinterpret_cast<char *>(data.data()), streamsize(data.size() * sizeof(T)));
    return bool(stream);
}
} // namespace
void RefreshMeshletBounds(Asset &asset, span<const Position> positions)
{
    asset.Bounds.resize(asset.Meshlets.size());
    for (size_t i=0;i<asset.Meshlets.size();++i)
        asset.Bounds[i]=ComputeBounds(asset,asset.Meshlets[i],positions);
}
Asset Build(span<const Position> positions, span<const uint32_t> indices, BuildConfig config)
{
    if (!ConfigValid(config) || indices.size() % 3)
        throw invalid_argument("Invalid meshlet build configuration/triangle list");
    if (indices.size() > UINT32_MAX || positions.size() > UINT32_MAX)
        throw overflow_error("Meshlet geometry too large");
    for (auto p : positions)
        if (!Finite(p))
            throw invalid_argument("Non-finite meshlet position");
    for (auto index : indices)
        if (index >= positions.size())
            throw out_of_range("Meshlet vertex index");
    Asset asset;
    asset.Config = config;
    Meshlet current;
    vector<uint32_t> local;
    auto finish = [&]() {
        if (!current.PrimitiveCount)
            return;
        current.VertexCount = static_cast<uint32_t>(local.size());
        asset.Vertices.insert(asset.Vertices.end(), local.begin(), local.end());
        asset.Meshlets.push_back(current);
        asset.Bounds.push_back(ComputeBounds(asset, current, positions));
        current = {static_cast<uint32_t>(asset.Vertices.size()), 0,
                   static_cast<uint32_t>(asset.Primitives.size()), 0};
        local.clear();
    };
    for (size_t i = 0; i < indices.size(); i += 3)
    {
        uint32_t extra = 0;
        for (uint32_t j = 0; j < 3; ++j)
        {
            bool earlier = false;
            for (uint32_t k = 0; k < j; ++k)
                earlier |= indices[i + k] == indices[i + j];
            if (!earlier && find(local.begin(), local.end(), indices[i + j]) == local.end())
                ++extra;
        }
        if (current.PrimitiveCount >= config.MaxPrimitives || local.size() + extra > config.MaxVertices)
            finish();
        uint32_t packed = 0;
        for (uint32_t j = 0; j < 3; ++j)
        {
            auto found = find(local.begin(), local.end(), indices[i + j]);
            uint32_t index = static_cast<uint32_t>(found - local.begin());
            if (found == local.end())
                local.push_back(indices[i + j]);
            packed |= index << (j * 8);
        }
        asset.Primitives.push_back(packed);
        ++current.PrimitiveCount;
    }
    finish();
    for (uint32_t first = 0; first < asset.Meshlets.size(); first += config.MeshletsPerCluster)
    {
        Cluster cluster;
        cluster.MeshletOffset = first;
        cluster.MeshletCount =
            min(config.MeshletsPerCluster, static_cast<uint32_t>(asset.Meshlets.size()) - first);
        Position minimum = asset.Bounds[first].Center, maximum = minimum;
        for (uint32_t j = 0; j < cluster.MeshletCount; ++j)
        {
            const auto &b = asset.Bounds[first + j];
            minimum = {min(minimum.X, b.Center.X - b.Radius), min(minimum.Y, b.Center.Y - b.Radius),
                       min(minimum.Z, b.Center.Z - b.Radius)};
            maximum = {max(maximum.X, b.Center.X + b.Radius), max(maximum.Y, b.Center.Y + b.Radius),
                       max(maximum.Z, b.Center.Z + b.Radius)};
        }
        cluster.Bounds.Center = Scale(Add(minimum, maximum), 0.5f);
        for (uint32_t j = 0; j < cluster.MeshletCount; ++j)
        {
            const auto &b = asset.Bounds[first + j];
            cluster.Bounds.Radius =
                max(cluster.Bounds.Radius, Length(Sub(b.Center, cluster.Bounds.Center)) + b.Radius);
        }
        cluster.Bounds.Radius = nextafter(cluster.Bounds.Radius, numeric_limits<float>::infinity());
        asset.Clusters.push_back(cluster);
    }
    string error;
    if (!Validate(asset, positions, error))
        throw runtime_error(error);
    return asset;
}
bool Validate(const Asset &asset, span<const Position> positions, string &error)
{
    auto fail = [&](const char *message) {
        error = message;
        return false;
    };
    if (!ConfigValid(asset.Config) || asset.Meshlets.size() != asset.Bounds.size())
        return fail("Invalid meshlet config/bounds count");
    for (auto p : positions)
        if (!Finite(p))
            return fail("Non-finite geometry");
    uint64_t vertices = 0, primitives = 0;
    for (size_t id = 0; id < asset.Meshlets.size(); ++id)
    {
        const auto &m = asset.Meshlets[id];
        const auto &b = asset.Bounds[id];
        if (m.VertexOffset != vertices || m.PrimitiveOffset != primitives || !m.VertexCount ||
            !m.PrimitiveCount || m.VertexCount > asset.Config.MaxVertices ||
            m.PrimitiveCount > asset.Config.MaxPrimitives ||
            uint64_t(m.VertexOffset) + m.VertexCount > asset.Vertices.size() ||
            uint64_t(m.PrimitiveOffset) + m.PrimitiveCount > asset.Primitives.size())
            return fail("Invalid meshlet range");
        if (!Finite(b.Center) || !isfinite(b.Radius) || b.Radius < 0 || !Finite(b.ConeAxis) ||
            !isfinite(b.ConeCutoff) || b.ConeCutoff < 0 || b.ConeCutoff > 1)
            return fail("Invalid bounds/cone");
        for (uint32_t i = 0; i < m.VertexCount; ++i)
        {
            auto index = asset.Vertices[m.VertexOffset + i];
            if (index >= positions.size() || Length(Sub(positions[index], b.Center)) > b.Radius + 1e-5f)
                return fail("Invalid vertex/bounds containment");
        }
        for (uint32_t i = 0; i < m.PrimitiveCount; ++i)
        {
            uint32_t packed = asset.Primitives[m.PrimitiveOffset + i];
            if (packed >> 24)
                return fail("Invalid packed primitive");
            for (uint32_t j = 0; j < 3; ++j)
                if (((packed >> (j * 8)) & 255) >= m.VertexCount)
                    return fail("Invalid primitive index");
        }
        auto computed = ComputeBounds(asset, m, positions);
        if (b.ConeCutoff < 1 &&
            (abs(Length(b.ConeAxis) - 1) > 1e-4f || Dot(b.ConeAxis, computed.ConeAxis) < 0.99999f ||
             b.ConeCutoff + 1e-6f < computed.ConeCutoff))
            return fail("Unsafe normal cone");
        vertices += m.VertexCount;
        primitives += m.PrimitiveCount;
    }
    if (vertices != asset.Vertices.size() || primitives != asset.Primitives.size())
        return fail("Unreferenced meshlet data");
    uint64_t children = 0;
    for (const auto &c : asset.Clusters)
    {
        if (c.MeshletOffset != children || !c.MeshletCount ||
            c.MeshletCount > asset.Config.MeshletsPerCluster ||
            uint64_t(c.MeshletOffset) + c.MeshletCount > asset.Meshlets.size() || !Finite(c.Bounds.Center) ||
            !isfinite(c.Bounds.Radius) || c.Bounds.Radius < 0 || c.Bounds.ConeCutoff != 1)
            return fail("Invalid cluster");
        for (uint32_t j = 0; j < c.MeshletCount; ++j)
        {
            const auto &b = asset.Bounds[c.MeshletOffset + j];
            if (Length(Sub(b.Center, c.Bounds.Center)) + b.Radius > c.Bounds.Radius + 1e-5f)
                return fail("Cluster does not contain child");
        }
        children += c.MeshletCount;
    }
    if (children != asset.Meshlets.size())
        return fail("Incomplete cluster hierarchy");
    error.clear();
    return true;
}
uint64_t GeometryKey(span<const Position> positions, span<const uint32_t> indices,
                     BuildConfig config)
{
    uint64_t hash = 14695981039346656037ull;
    auto consume = [&](const void *data, size_t bytes) {
        auto p = static_cast<const uint8_t *>(data);
        for (size_t i = 0; i < bytes; ++i)
        {
            hash ^= p[i];
            hash *= 1099511628211ull;
        }
    };
    consume(&config, sizeof(config));
    consume(positions.data(), positions.size_bytes());
    consume(indices.data(), indices.size_bytes());
    return hash;
}
bool Save(const filesystem::path &path, const Asset &asset, uint64_t key)
{
    try
    {
        if (!path.parent_path().empty())
            filesystem::create_directories(path.parent_path());
        Header header;
        header.Key = key;
        header.PayloadHash = PayloadHash(asset);
        header.Config = asset.Config;
        header.Counts[0] = static_cast<uint32_t>(asset.Meshlets.size());
        header.Counts[1] = static_cast<uint32_t>(asset.Vertices.size());
        header.Counts[2] = static_cast<uint32_t>(asset.Primitives.size());
        header.Counts[3] = static_cast<uint32_t>(asset.Bounds.size());
        header.Counts[4] = static_cast<uint32_t>(asset.Clusters.size());
        ofstream stream(path, ios::binary | ios::trunc);
        stream.write(reinterpret_cast<const char *>(&header), sizeof(header));
        return Write(stream, asset.Meshlets) && Write(stream, asset.Vertices) &&
               Write(stream, asset.Primitives) && Write(stream, asset.Bounds) &&
               Write(stream, asset.Clusters);
    }
    catch (...)
    {
        return false;
    }
}
bool Load(const filesystem::path &path, Asset &asset, uint64_t key, span<const Position> positions)
{
    try
    {
        ifstream stream(path, ios::binary | ios::ate);
        if (!stream)
            return false;
        const auto bytes = stream.tellg();
        stream.seekg(0);
        Header header;
        stream.read(reinterpret_cast<char *>(&header), sizeof(header));
        if (!stream || header.Magic != 0x354c534d || header.Version != 2 || header.Key != key ||
            !ConfigValid(header.Config))
            return false;
        const uint64_t expected = sizeof(header) + uint64_t(header.Counts[0]) * sizeof(Meshlet) +
                                  uint64_t(header.Counts[1]) * 4 + uint64_t(header.Counts[2]) * 4 +
                                  uint64_t(header.Counts[3]) * sizeof(Bounds) +
                                  uint64_t(header.Counts[4]) * sizeof(Cluster);
        if (expected != static_cast<uint64_t>(bytes) || expected > uint64_t(2) * 1024 * 1024 * 1024)
            return false;
        Asset loaded;
        loaded.Config = header.Config;
        if (!Read(stream, loaded.Meshlets, header.Counts[0]) ||
            !Read(stream, loaded.Vertices, header.Counts[1]) ||
            !Read(stream, loaded.Primitives, header.Counts[2]) ||
            !Read(stream, loaded.Bounds, header.Counts[3]) ||
            !Read(stream, loaded.Clusters, header.Counts[4]))
            return false;
        string error;
        if (PayloadHash(loaded) != header.PayloadHash || !Validate(loaded, positions, error))
            return false;
        asset = move(loaded);
        return true;
    }
    catch (...)
    {
        return false;
    }
}
array<uint32_t, 3> DispatchDimensions(uint32_t count)
{
    const uint32_t x = min(count, 65535u);
    return {x, count ? static_cast<uint32_t>((uint64_t(count) + x - 1) / x) : 1u, 1u};
}
} // namespace Meshlets
