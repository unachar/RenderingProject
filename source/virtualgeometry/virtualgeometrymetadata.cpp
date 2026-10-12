#include "virtualgeometryserializationinternal.h"
#include "virtualgeometryinternal.h"

namespace VirtualGeometrySerialization
{
    void WriteMetadata(Writer& writer, const VirtualGeometryAsset& asset)
    {
        const auto& config = asset.Config;
        writer.U32(config.MaxClusterTriangles);
        writer.U32(config.MaxClusterVertices);
        writer.U32(config.TargetClustersPerGroup);
        writer.U32(config.MaxHierarchyLevels);
        writer.F32(config.SimplificationRatio);
        writer.F32(config.ConeWeight);
        writer.F32(config.NormalWeight);
        writer.F32(config.TexCoordWeight);
        writer.F32(config.ColorWeight);
        writer.F32(config.TargetRelativeError);
        writer.F32(config.BoundaryEpsilon);
        writer.U32(asset.SourceMaterialId);
        writer.F32(asset.SourceMaterialPartId);
        writer.U32(VirtualGeometryInternal::CheckedIndex(asset.DebugName.size()));
        writer.Bytes.insert(writer.Bytes.end(), asset.DebugName.begin(), asset.DebugName.end());
        const auto& stats = asset.Stats;
        writer.U64(stats.SourceVertices);
        writer.U64(stats.SourceTriangles);
        writer.U64(stats.RemovedDegenerateTriangles);
        writer.U64(stats.RemovedDuplicateTriangles);
        writer.U64(stats.FinalLeafTriangles);
        writer.U32(stats.LeafClusters);
        writer.U32(stats.TotalClusters);
        writer.U32(stats.TotalGroups);
        writer.U32(stats.HierarchyLevels);
        writer.U32(stats.MaximumTrianglesPerCluster);
        writer.U32(stats.MaximumVerticesPerCluster);
        writer.F64(stats.AverageTrianglesPerCluster);
        writer.F64(stats.AverageVerticesPerCluster);
        writer.U64(stats.SourceMemoryBytes);
        writer.U64(stats.RawMemoryBytes);
    }

    void ReadMetadata(Reader& reader, VirtualGeometryAsset& asset)
    {
        auto& config = asset.Config;
        config.MaxClusterTriangles = reader.U32();
        config.MaxClusterVertices = reader.U32();
        config.TargetClustersPerGroup = reader.U32();
        config.MaxHierarchyLevels = reader.U32();
        config.SimplificationRatio = reader.F32();
        config.ConeWeight = reader.F32();
        config.NormalWeight = reader.F32();
        config.TexCoordWeight = reader.F32();
        config.ColorWeight = reader.F32();
        config.TargetRelativeError = reader.F32();
        config.BoundaryEpsilon = reader.F32();
        asset.SourceMaterialId = reader.U32();
        asset.SourceMaterialPartId = reader.F32();
        auto name = reader.Take(reader.U32());
        asset.DebugName.assign(name.begin(), name.end());
        auto& stats = asset.Stats;
        stats.SourceVertices = reader.U64();
        stats.SourceTriangles = reader.U64();
        stats.RemovedDegenerateTriangles = reader.U64();
        stats.RemovedDuplicateTriangles = reader.U64();
        stats.FinalLeafTriangles = reader.U64();
        stats.LeafClusters = reader.U32();
        stats.TotalClusters = reader.U32();
        stats.TotalGroups = reader.U32();
        stats.HierarchyLevels = reader.U32();
        stats.MaximumTrianglesPerCluster = reader.U32();
        stats.MaximumVerticesPerCluster = reader.U32();
        stats.AverageTrianglesPerCluster = reader.F64();
        stats.AverageVerticesPerCluster = reader.F64();
        stats.SourceMemoryBytes = reader.U64();
        stats.RawMemoryBytes = reader.U64();
        stats.BuildMilliseconds = 0;
        reader.RequireEnd();
    }
}
