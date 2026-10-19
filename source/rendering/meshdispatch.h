#pragma once
#include <cstdint>

namespace MeshDispatch
{
    constexpr uint32_t TrianglesPerMeshlet = 64;
    constexpr uint32_t GroupsPerPage = 65535;

    constexpr uint32_t MeshletCount(uint32_t elementCount)
    {
        const uint32_t triangles = elementCount / 3;
        return triangles / TrianglesPerMeshlet + (triangles % TrianglesPerMeshlet != 0);
    }

    constexpr uint32_t PageSize(uint32_t meshlets, uint32_t first, uint32_t limit = GroupsPerPage)
    {
        if (first >= meshlets)
        {
            return 0;
        }
        if (!limit || limit > GroupsPerPage)
        {
            limit = GroupsPerPage;
        }
        const uint32_t remaining = meshlets - first;
        return remaining < limit ? remaining : limit;
    }
} // 名前空間 MeshDispatch
