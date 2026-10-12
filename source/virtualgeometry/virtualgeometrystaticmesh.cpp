#include "pch.h"
#include "virtualgeometrybuilder.h"
#include "virtualgeometryvalidator.h"
#include "staticmodel.h"
#include <cmath>

VirtualGeometryBuildResult VirtualGeometryBuilder::Build(
    const StaticMeshData& source, const VirtualGeometryBuildConfig& config, uint32_t materialId)
{
    if (source.CpuIndices.size() != source.IndexCount || source.CpuVertices.size() != source.VertexCount)
    {
        VirtualGeometryBuildResult result;
        result.Error = "Static mesh is missing complete CPU LOD0 geometry";
        return result;
    }
    if (!std::isfinite(source.AppliedMaterialPartId))
    {
        VirtualGeometryBuildResult result;
        result.Error = "Static mesh has a non-finite material part ID";
        return result;
    }
    std::vector<VirtualGeometryVertex> vertices;
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
    catch (const std::exception& exception)
    {
        VirtualGeometryBuildResult result;
        result.Error = exception.what();
        return result;
    }
}
