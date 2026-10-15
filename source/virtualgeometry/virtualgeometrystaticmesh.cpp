#include "pch.h"
#include "virtualgeometrybuilder.h"
#include "virtualgeometryvalidator.h"
#include "staticmodel.h"
#include <cmath>

bool CanUseVirtualGeometry(const StaticMeshData& source, std::string& reason)
{
    reason.clear();
    if (source.VertexCount == 0 || source.IndexCount < 3 || source.IndexCount % 3 != 0)
    {
        reason = "VirtualGeometry requires a non-empty triangle list";
        return false;
    }
    if (source.CpuIndices.size() != source.IndexCount || source.CpuVertices.size() != source.VertexCount)
    {
        reason = "Static mesh is missing complete CPU LOD0 geometry";
        return false;
    }
    for (const auto index : source.CpuIndices)
    {
        if (index >= source.VertexCount)
        {
            reason = "Static mesh index is outside the source vertex range";
            return false;
        }
    }
    if (!std::isfinite(source.AppliedMaterialPartId))
    {
        reason = "Static mesh has a non-finite material part ID";
        return false;
    }
    return true;
}

VirtualGeometryBuildResult VirtualGeometryBuilder::Build(
    const StaticMeshData& source, const VirtualGeometryBuildConfig& config, uint32_t materialId)
{
    std::string reason;
    if (!CanUseVirtualGeometry(source, reason))
    {
        VirtualGeometryBuildResult result;
        result.Error = reason;
        return result;
    }
    if (!isfinite(source.AppliedMaterialPartId))
    {
        VirtualGeometryBuildResult result;
        result.Error = "Static mesh has a non-finite material part ID";
        return result;
    }
    vector<VirtualGeometryVertex> vertices;
    try
    {
        vertices.reserve(source.CpuVertices.size());
        for (const auto& sourceVertex : source.CpuVertices)
        {
            VirtualGeometryVertex vertex;
            vertex.Position[0] = sourceVertex.Position.x;
            vertex.Position[1] = sourceVertex.Position.y;
            vertex.Position[2] = sourceVertex.Position.z;
            vertex.Normal[0] = sourceVertex.Normal.x;
            vertex.Normal[1] = sourceVertex.Normal.y;
            vertex.Normal[2] = sourceVertex.Normal.z;
            vertex.TexCoord[0] = sourceVertex.TexCoord.x;
            vertex.TexCoord[1] = sourceVertex.TexCoord.y;
            vertex.Diffuse[0] = sourceVertex.Diffuse.x;
            vertex.Diffuse[1] = sourceVertex.Diffuse.y;
            vertex.Diffuse[2] = sourceVertex.Diffuse.z;
            vertex.Diffuse[3] = sourceVertex.Diffuse.w;
            vertices.push_back(vertex);
        }
        auto result = Build(vertices, source.CpuIndices, config, materialId);
        if (result)
        {
            result.Asset.DebugName = source.MeshName;
            result.Asset.SourceMaterialPartId = source.AppliedMaterialPartId;
        }
        return result;
    }
    catch (const exception& exception)
    {
        VirtualGeometryBuildResult result;
        result.Error = exception.what();
        return result;
    }
}
